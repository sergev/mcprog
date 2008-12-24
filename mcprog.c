/*
 * Flash programmer for Elvees Multicore microcontrollers.
 *
 * By Sergey Vakulenko, <vak@cronyx.ru>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include "multicore.h"

#define PROGNAME	"Programmer for Elvees Multicore CPU"
#define VERSION		"1.2"
#define SLINESZ		32

/* Macros for converting between hex and binary. */
#define NIBBLE(x)	(isdigit(x) ? (x)-'0' : tolower(x)+10-'a')
#define HEX(buffer)	((NIBBLE((buffer)[0])<<4) + NIBBLE((buffer)[1]))

unsigned char flash_data [0x200000];	/* Code - up to 2 Mbytes */
long flash_len;

unsigned long flash_base;
unsigned long count;

multicore_t *multicore;
int debug;
int program = 1;

void *fix_time ()
{
	static struct timeval t0;

	gettimeofday (&t0, 0);
	return &t0;
}

unsigned long mseconds_elapsed (void *arg)
{
	struct timeval t1, *t0 = arg;
	unsigned long mseconds;

	gettimeofday (&t1, 0);
	mseconds = (t1.tv_sec - t0->tv_sec) * 1000 +
		(t1.tv_usec - t0->tv_usec) / 1000;
	if (mseconds < 1)
		mseconds = 1;
	return mseconds;
}

/*
 * Read binary file.
 */
int read_bin (char *filename, unsigned char *output)
{
	FILE *fd;
	int output_len;

	fd = fopen (filename, "rb");
	if (! fd) {
		perror (filename);
		exit (1);
	}
	output_len = fread (output, 1, sizeof (flash_data), fd);
	fclose (fd);
	if (output_len <= 0) {
		fprintf (stderr, "%s: read error\n", filename);
		exit (1);
	}
	return output_len;
}

/*
 * Read the S record file.
 */
int read_srec (char *filename, unsigned char *output)
{
	FILE *fd;
	unsigned char buf [256];
	unsigned char *data;
	unsigned long address;
	int bytes, output_len;

	fd = fopen (filename, "r");
	if (! fd) {
		perror (filename);
		exit (1);
	}
	output_len = 0;
	while (fgets ((char*) buf, sizeof(buf), fd)) {
		if (buf[0] == '\n')
			continue;
		if (buf[0] != 'S') {
			if (output_len == 0) {
				fclose (fd);
				return 0;
			}
			fprintf (stderr, "%s: bad file format\n", filename);
			exit (1);
		}
		if (buf[1] == '7' || buf[1] == '8' || buf[1] == '9')
			break;

		/* Starting an S-record.  */
		if (! isxdigit (buf[2]) || ! isxdigit (buf[3])) {
			fprintf (stderr, "%s: bad record: %s\n", filename, buf);
			exit (1);
		}
		bytes = HEX (buf + 2);

		/* Ignore the checksum byte.  */
		--bytes;

		address = 0;
		data = buf + 4;
		switch (buf[1]) {
		case '3':
			address = HEX (data);
			data += 2;
			--bytes;
			/* Fall through.  */
		case '2':
			address = (address << 8) | HEX (data);
			data += 2;
			--bytes;
			/* Fall through.  */
		case '1':
			address = (address << 8) | HEX (data);
			data += 2;
			address = (address << 8) | HEX (data);
			data += 2;
			bytes -= 2;

			if (! flash_base) {
				/* Автоматическое определение базового адреса. */
				if (address >= 0x1FC00000 && address < 0x1FE00000)
					flash_base = 0x1FC00000;
				else if (address >= 0x1FA00000 && address < 0x1FC00000)
					flash_base = 0x1FA00000;
				else if (address >= 0x02000000 && address < 0x02400000)
					flash_base = 0x02000000;
				else if (address >= 0x9FC00000 && address < 0x9FE00000)
					flash_base = 0x9FC00000;
				else if (address >= 0x9FA00000 && address < 0x9FC00000)
					flash_base = 0x9FA00000;
				else if (address >= 0x82000000 && address < 0x82400000)
					flash_base = 0x82000000;
				else if (address >= 0xBFC00000 && address < 0xBFE00000)
					flash_base = 0xBFC00000;
				else if (address >= 0xBFA00000 && address < 0xBFC00000)
					flash_base = 0xBFA00000;
				else if (address >= 0xA2000000 && address < 0xA2400000)
					flash_base = 0xA2000000;
				else {
					fprintf (stderr, "%s: incorrect address %#lx\n",
						filename, address);
					exit (1);
				}
			}
			if (address < flash_base) {
				fprintf (stderr, "%s: incorrect address %#lx, must be %#lx or greater\n",
					filename, address, flash_base);
				exit (1);
			}
			address -= flash_base;
			if (address+bytes > sizeof (flash_data)) {
				fprintf (stderr, "%s: address too large: %#lx + %#x\n",
					filename, address + flash_base, bytes);
				exit (1);
			}
			while (bytes-- > 0) {
				output[address++] = HEX (data);
				data += 2;
			}
			if (output_len < (long) address)
				output_len = address;
			break;
		}
	}
	fclose (fd);
	return output_len;
}

void print_symbols (char symbol, int cnt)
{
	while (cnt-- > 0)
		putchar (symbol);
}

void program_block (multicore_t *mc, unsigned long addr, unsigned short len)
{
	unsigned short i;
	unsigned long word;

	if (program) {
		for (i=0; i<len; i+=4) {
			multicore_write_word (mc, flash_base + addr + i,
				*(unsigned long*) (flash_data + addr + i));
		}
	}
	++count;
	putchar ("/-\\|" [count & 3]);
	putchar ('\b');
	fflush (stdout);
	if (count % 128 == 0) {
		putchar ('#');
		fflush (stdout);
	}

	multicore_read_start (mc);
	for (i=0; i<len; i+=4) {
		word = multicore_read_next (mc, flash_base + addr + i);
		if (debug > 1)
			printf ("read word %#08lx at address %#lx\n",
				word, addr + i + flash_base);
		if (word != *(unsigned long*) (flash_data+addr+i)) {
			printf ("\nerror at address %#lx: file=%#08lx, mem=%#08lx\n",
				addr + i + flash_base,
				*(unsigned long*) (flash_data+addr+i), word);
			exit (1);
		}
	}
}

void quit (void)
{
	if (multicore != 0) {
		multicore_close (multicore);
		free (multicore);
		multicore = 0;
	}
}

int main (int argc, char **argv)
{
	char *inname1;
	unsigned long addr;
	int ch;
	extern int optind;
	void *t0;

	setvbuf (stdout, (char *)NULL, _IOLBF, 0);
	setvbuf (stderr, (char *)NULL, _IOLBF, 0);
	printf (PROGNAME ", Version " VERSION ", Copyright (C) 2008 IPMCE\n");

	while ((ch = getopt(argc, argv, "vDb:")) != -1) {
		switch (ch) {
		case 'v':
			program = 0;
			continue;
		case 'b':
			flash_base = strtoul (optarg, 0, 0);
			continue;
		case 'D':
			++debug;
			continue;
		}
usage:		printf ("\nUsage:\n");
		printf ("        mcprog [-v] [-D] [-b addr] filename\n");
		printf ("Args:\n");
		printf ("        filename   Code file in binary or SREC format\n");
		printf ("        -b addr    Base address of flash memory\n");
		printf ("        -v         Verify only\n");
		printf ("        -D         Print debugging information\n");
		exit (0);
	}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		goto usage;

	multicore_init ();
	inname1 = argv[0];

	flash_len = read_srec (inname1, flash_data);
	if (flash_len == 0)
		flash_len = read_bin (inname1, flash_data);

	printf ("Memory: %#lx-%#lx, total %ld bytes\n", flash_base,
		flash_base + flash_len, flash_len);

	/* Open and detect the device. */
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	printf ("Device: %s, Flash %s\n", multicore_cpu_name (multicore),
		multicore_flash_name (multicore));

	if (program) {
		/* Erase flash. */
		multicore_erase (multicore, flash_base);
	}

	count = 1 + flash_len / 32 / 1024;
	printf (program ? "Program: " : "Verify: ");
	print_symbols ('.', count);
	print_symbols ('\b', count);
	fflush (stdout);

	count = 0;
	t0 = fix_time ();
	for (addr=0; (long)addr<flash_len; addr+=256)
		program_block (multicore, addr, (flash_len - addr >= 256) ?
			256 : (unsigned short) (flash_len - addr));
	printf ("# done.\n");
	printf ("Rate: %ld bytes per second\n",
		flash_len * 1000L / mseconds_elapsed (t0));
	quit ();
	return 0;
}
