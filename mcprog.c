/*
 * Программатор flash-памяти для микроконтроллеров Элвис Мультикор.
 *
 * Разработано в ИТМиВТ, 2008.
 * Автор: С.Вакуленко.
 *
 * Этот файл распространяется в надежде, что он окажется полезным, но
 * БЕЗ КАКИХ БЫ ТО НИ БЫЛО ГАРАНТИЙНЫХ ОБЯЗАТЕЛЬСТВ; в том числе без косвенных
 * гарантийных обязательств, связанных с ПОТРЕБИТЕЛЬСКИМИ СВОЙСТВАМИ и
 * ПРИГОДНОСТЬЮ ДЛЯ ОПРЕДЕЛЕННЫХ ЦЕЛЕЙ.
 *
 * Вы вправе распространять и/или изменять этот файл в соответствии
 * с условиями Генеральной Общественной Лицензии GNU (GPL) в том виде,
 * как она была опубликована Фондом Свободного ПО; либо версии 2 Лицензии
 * либо (по вашему желанию) любой более поздней версии. Подробности
 * смотрите в прилагаемом файле 'COPYING.txt'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include "multicore.h"

#define PROGNAME	"*** Programmer for Elvees Multicore CPU"
#define VERSION		"1.3"
#define BLOCKSZ		1024

/* Macros for converting between hex and binary. */
#define NIBBLE(x)	(isdigit(x) ? (x)-'0' : tolower(x)+10-'a')
#define HEX(buffer)	((NIBBLE((buffer)[0])<<4) + NIBBLE((buffer)[1]))

unsigned char flash_data [0x200000];	/* Code - up to 2 Mbytes */
long flash_len;
unsigned long flash_base = 0xBFC00000;
unsigned long progress_count, progress_step;
int debug;
multicore_t *multicore;

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
			if (output_len == 0)
				break;
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
					fprintf (stderr, "%s: incorrect address %08lx\n",
						filename, address);
					exit (1);
				}
			}
			if (address < flash_base) {
				fprintf (stderr, "%s: incorrect address %08lx, must be %08lx or greater\n",
					filename, address, flash_base);
				exit (1);
			}
			address -= flash_base;
			if (address+bytes > sizeof (flash_data)) {
				fprintf (stderr, "%s: address too large: %08lx + %08x\n",
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

void program_block (multicore_t *mc, unsigned long addr, int len)
{
	int i;

	/* Write flash memory. */
	for (i=0; i<len; i+=4) {
		multicore_write_word (mc, flash_base + addr + i,
			*(unsigned long*) (flash_data + addr + i));
	}
}

void verify_block (multicore_t *mc, unsigned long addr, int len)
{
	int i;
	unsigned long word;

	++progress_count;
	putchar ("/-\\|" [progress_count & 3]);
	putchar ('\b');
	fflush (stdout);
	if (progress_count % progress_step == 0) {
		putchar ('#');
		fflush (stdout);
	}

	multicore_read_start (mc);
	for (i=0; i<len; i+=4) {
		word = multicore_read_next (mc, flash_base + addr + i);
		if (debug > 1)
			printf ("read word %08lx at address %08lx\n",
				word, addr + i + flash_base);
		if (word != *(unsigned long*) (flash_data+addr+i)) {
			printf ("\nerror at address %08lx: file=%08lx, mem=%08lx\n",
				addr + i + flash_base,
				*(unsigned long*) (flash_data+addr+i), word);
			exit (1);
		}
	}
}

void probe_flash (multicore_t *mc, unsigned base)
{
	unsigned mfcode, devcode, bytes, width;
	char mfname[40], devname[40];

	printf ("Flash at 0x%08x: ", base);
	if (! multicore_flash_detect (mc, base, &mfcode, &devcode,
	    mfname, devname, &bytes, &width)) {
		printf ("Incorrect id %08x\n", devcode);
		return;
	}
	printf ("%s %s (%08x/%08x), %d Mbytes, %d bit wide\n",
		mfname, devname, mfcode, devcode, bytes / 1024 / 1024, width);
}

void quit (void)
{
	if (multicore != 0) {
		multicore_close (multicore);
		free (multicore);
		multicore = 0;
	}
}

void do_probe ()
{
	/* Open and detect the device. */
	multicore_init ();
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	printf ("Processor: %s (id %08x)\n", multicore_cpu_name (multicore),
		multicore_idcode (multicore));

	probe_flash (multicore, 0x1FC00000);
	probe_flash (multicore, 0x1FA00000);
	probe_flash (multicore, 0x02000000);
}

void do_program (int verify_only)
{
	unsigned long addr;
	unsigned mfcode, devcode, bytes, width;
	char mfname[40], devname[40];
	int len;
	void *t0;

	multicore_init ();
	printf ("Memory: %08lx-%08lx, total %ld bytes\n", flash_base,
		flash_base + flash_len, flash_len);

	/* Open and detect the device. */
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	/*printf ("Processor: %s\n", multicore_cpu_name (multicore));*/

	if (! multicore_flash_detect (multicore, flash_base,
	    &mfcode, &devcode, mfname, devname, &bytes, &width)) {
		printf ("No flash memory detected.\n");
		return;
	}
	printf ("Flash: %s %s, size %d Mbytes\n",
		mfname, devname, bytes / 1024 / 1024);

	if (! verify_only) {
		/* Erase flash. */
		multicore_erase (multicore, flash_base);
	}
	for (progress_step=1; ; progress_step<<=1) {
		len = 1 + flash_len / progress_step / BLOCKSZ;
		if (len < 64)
			break;
	}
	printf (verify_only ? "Verify: " : "Program: " );
	print_symbols ('.', len);
	print_symbols ('\b', len);
	fflush (stdout);

	progress_count = 0;
	t0 = fix_time ();
	for (addr=0; (long)addr<flash_len; addr+=BLOCKSZ) {
		len = BLOCKSZ;
		if (flash_len - addr < len)
			len = flash_len - addr;
		if (! verify_only)
			program_block (multicore, addr, len);
		verify_block (multicore, addr, len);
	}
	printf ("# done\n");
	printf ("Rate: %ld bytes per second\n",
		flash_len * 1000L / mseconds_elapsed (t0));
}

int main (int argc, char **argv)
{
	int ch, verify_only = 0;

	setvbuf (stdout, (char *)NULL, _IOLBF, 0);
	setvbuf (stderr, (char *)NULL, _IOLBF, 0);
	printf (PROGNAME ", Version " VERSION ", Copyright (C) 2008 IPMCE\n");

	while ((ch = getopt(argc, argv, "vDh")) != -1) {
		switch (ch) {
		case 'v':
			++verify_only;
			continue;
		case 'D':
			++debug;
			continue;
		case 'h':
			break;
		}
usage:		printf ("\nProbe:\n");
		printf ("        mcprog\n");
		printf ("\nWrite flash memory:\n");
		printf ("        mcprog [-v] file.sre\n");
		printf ("        mcprog [-v] file.bin [address]\n");
		printf ("Args:\n");
		printf ("        file.sre   Code file SREC format\n");
		printf ("        file.bin   Code file in binary format\n");
		printf ("        address    Address of flash memory, default %08lx\n",
			flash_base);
		printf ("        -v         Verify only\n");
		exit (0);
	}
	argc -= optind;
	argv += optind;
	if (argc == 0) {
		do_probe ();
	} else if (argc == 1) {
		flash_len = read_srec (argv[0], flash_data);
		if (flash_len == 0)
			flash_len = read_bin (argv[0], flash_data);
		do_program (verify_only);
	} else if (argc == 2) {
		flash_base = strtoul (argv[1], 0, 0);
		flash_len = read_bin (argv[0], flash_data);
		do_program (verify_only);
	} else
		goto usage;

	quit ();
	return 0;
}
