/*
 * Программатор flash-памяти для микроконтроллеров Элвис Мультикор.
 *
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
#include "conf.h"

#define PROGNAME	"Programmer for Elvees Multicore CPU"
#define VERSION		"1.7"
#define BLOCKSZ		1024
#define DEFAULT_ADDR	0xBFC00000

/* Macros for converting between hex and binary. */
#define NIBBLE(x)	(isdigit(x) ? (x)-'0' : tolower(x)+10-'a')
#define HEX(buffer)	((NIBBLE((buffer)[0])<<4) + NIBBLE((buffer)[1]))

unsigned char memory_data [0x800000];	/* Code - up to 8 Mbytes */
int memory_len;
unsigned memory_base;
unsigned progress_count, progress_step;
int verify_only;
int debug;
multicore_t *multicore;
char *progname;
char *confname;
char *board;

void *fix_time ()
{
	static struct timeval t0;

	gettimeofday (&t0, 0);
	return &t0;
}

unsigned mseconds_elapsed (void *arg)
{
	struct timeval t1, *t0 = arg;
	unsigned mseconds;

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
	output_len = fread (output, 1, sizeof (memory_data), fd);
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
	unsigned address;
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

			if (! memory_base) {
				/* Автоматическое определение базового адреса. */
				memory_base = address;
			}
			if (address < memory_base) {
				fprintf (stderr, "%s: incorrect address %08X, must be %08X or greater\n",
					filename, address, memory_base);
				exit (1);
			}
			address -= memory_base;
			if (address+bytes > sizeof (memory_data)) {
				fprintf (stderr, "%s: address too large: %08X + %08X\n",
					filename, address + memory_base, bytes);
				exit (1);
			}
			while (bytes-- > 0) {
				output[address++] = HEX (data);
				data += 2;
			}
			if (output_len < (int) address)
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

void program_block (multicore_t *mc, unsigned addr, int len)
{
	/* Write flash memory. */
	multicore_program_block (mc, memory_base + addr,
		(len + 3) / 4, (unsigned*) (memory_data + addr));
}

void write_block (multicore_t *mc, unsigned addr, int len)
{
	/* Write static memory. */
	multicore_write_block (mc, memory_base + addr,
		(len + 3) / 4, (unsigned*) (memory_data + addr));
}

void progress ()
{
	++progress_count;
#if 0
	putchar ("/-\\|" [progress_count & 3]);
	putchar ('\b');
	fflush (stdout);
#endif
	if (progress_count % progress_step == 0) {
		putchar ('#');
		fflush (stdout);
	}
}

void verify_block (multicore_t *mc, unsigned addr, int len)
{
	int i;
	unsigned word, expected, block [BLOCKSZ/4];

	multicore_read_block (mc, memory_base + addr, (len+3)/4, block);
	for (i=0; i<len; i+=4) {
		expected = *(unsigned*) (memory_data + addr + i);
//		if (expected == 0xffffffff)
//			continue;
		word = block [i/4];
		if (debug > 1)
			printf ("read word %08X at address %08X\n",
				word, addr + i + memory_base);
		while (word != expected) {
			/* Возможно, не все нули прописались в flash-память.
			 * Пробуем повторить операцию. */
/* printf ("\nerror at address %08X: file=%08X, mem=%08X ",
addr + i + memory_base, expected, word); fflush (stdout); */
			if (verify_only || ! multicore_flash_rewrite (mc,
			    memory_base + addr + i, expected)) {
				printf ("\nerror at address %08X: file=%08X, mem=%08X\n",
					addr + i + memory_base, expected, word);
				exit (1);
			}
			printf ("%%\b");
			fflush (stdout);
			word = multicore_read_word (mc, memory_base + addr + i);
		}
	}
}

void probe_flash (multicore_t *mc, unsigned base)
{
	unsigned mfcode, devcode, bytes, width;
	char mfname[40], devname[40];

	printf ("Flash at %08X: ", base);
	if (! multicore_flash_detect (mc, base, &mfcode, &devcode,
	    mfname, devname, &bytes, &width)) {
		printf ("Incorrect id %08X\n", devcode);
		return;
	}
	printf ("%s %s (id %04x/%04x), %d Mbytes, %d bit wide\n",
		mfname, devname, mfcode & 0xFFFF, devcode & 0xFFFF,
		bytes / 1024 / 1024, width);
}

void quit (void)
{
	if (multicore != 0) {
		multicore_close (multicore);
		free (multicore);
		multicore = 0;
	}
}

void configure_parameter (char *section, char *param, char *value)
{
	unsigned word, first, last;

//fprintf (stderr, "section=%s, param=%s, value=%s\n", section, param, value);
	if (! section) {
		/* Remember default board name. */
		if (strcasecmp (param, "default") == 0) {
			if (! board)
				board = strdup (value);
			printf ("Board: %s\n", board);
		} else {
			fprintf (stderr, "%s: unknown parameter `%s'\n",
				confname, param);
			exit (-1);
		}
		return;
	}
	if (! board) {
		fprintf (stderr, "%s: parameter 'default' missing\n", confname);
		exit (-1);
	}
	if (strcasecmp (section, board) != 0)
		return;

	/* Needed section found. */
	if (strcasecmp (param, "csr") == 0) {
		word = strtol (value, 0, 0);
		multicore_write_word (multicore, 0x182F4008, word);

	} else if (strcasecmp (param, "cscon0") == 0) {
		word = strtol (value, 0, 0);
		multicore_write_word (multicore, 0x182F1000, word);

	} else if (strcasecmp (param, "cscon1") == 0) {
		word = strtol (value, 0, 0);
		multicore_write_word (multicore, 0x182F1004, word);

	} else if (strcasecmp (param, "cscon2") == 0) {
		word = strtol (value, 0, 0);
		multicore_write_word (multicore, 0x182F1008, word);

	} else if (strcasecmp (param, "cscon3") == 0) {
		word = strtol (value, 0, 0);
		multicore_write_word (multicore, 0x182F100C, word);

	} else if (strncasecmp (param, "flash ", 6) == 0) {
		if (sscanf (value, "%i-%i", &first, &last) != 2) {
			fprintf (stderr, "%s: incorrect value for parameter `%s'\n",
				confname, param);
			exit (-1);
		}
		printf ("  %s = %08X-%08X\n", param, first, last);
		multicore_flash_configure (multicore, first, last);
	} else {
		fprintf (stderr, "%s: unknown parameter `%s'\n",
			confname, param);
		exit (-1);
	}
/*	printf ("Configure: %s = %08X\n", param, word);*/
}

/*
 * Read configuration file and setup hardware registers.
 */
void configure ()
{
	confname = "mcprog.conf";
	if (access (confname, 0) < 0) {
#if defined (__CYGWIN32__) || defined (MINGW32)
		char *p = strrchr (progname, '\\');
		if (p) {
			confname = malloc (p - progname + 16);
			if (! confname) {
				fprintf (stderr, "%s: out of memory\n", progname);
				exit (-1);
			}
			strncpy (confname, progname, p - progname);
			strcpy (confname + (p - progname), "\\mcprog.conf");
		} else
			confname = "c:\\mcprog.conf";
#else
		confname = "/usr/local/etc/mcprog.conf";
#endif
	}
	conf_parse (confname, configure_parameter);
}

void do_probe ()
{
	unsigned addr, last;

	/* Open and detect the device. */
	multicore_init ();
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	printf ("Processor: %s (id %08X)\n", multicore_cpu_name (multicore),
		multicore_idcode (multicore));

	configure ();

	/* Probe all configured flash regions. */
	addr = ~0;
	for (;;) {
		addr = multicore_flash_next (multicore, addr, &last);
		if (! ~addr)
			break;
		probe_flash (multicore, addr);
	}
}

void do_program ()
{
	unsigned addr;
	unsigned mfcode, devcode, bytes, width;
	char mfname[40], devname[40];
	int len;
	void *t0;

	multicore_init ();
	printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
		memory_base + memory_len, memory_len);

	/* Open and detect the device. */
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	/*printf ("Processor: %s\n", multicore_cpu_name (multicore));*/

	configure ();
	if (! multicore_flash_detect (multicore, memory_base,
	    &mfcode, &devcode, mfname, devname, &bytes, &width)) {
		printf ("No flash memory detected.\n");
		return;
	}
	printf ("Flash: %s %s, size %d Mbytes, %d bit wide\n",
		mfname, devname, bytes / 1024 / 1024, width);

	if (! verify_only) {
		/* Erase flash. */
		multicore_erase (multicore, memory_base);
	}
	for (progress_step=1; ; progress_step<<=1) {
		len = 1 + memory_len / progress_step / BLOCKSZ;
		if (len < 64)
			break;
	}
	printf (verify_only ? "Verify: " : "Program: " );
	print_symbols ('.', len);
	print_symbols ('\b', len);
	fflush (stdout);

	progress_count = 0;
	t0 = fix_time ();
	for (addr=0; (int)addr<memory_len; addr+=BLOCKSZ) {
		len = BLOCKSZ;
		if (memory_len - addr < len)
			len = memory_len - addr;
		if (! verify_only)
			program_block (multicore, addr, len);
		progress ();
		verify_block (multicore, addr, len);
	}
	printf ("# done\n");
	printf ("Rate: %ld bytes per second\n",
		memory_len * 1000L / mseconds_elapsed (t0));
}

void do_write ()
{
	unsigned addr;
	int len;
	void *t0;

	multicore_init ();
	printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
		memory_base + memory_len, memory_len);

	/* Open and detect the device. */
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	/*printf ("Processor: %s\n", multicore_cpu_name (multicore));*/

	configure ();
	for (progress_step=1; ; progress_step<<=1) {
		len = 1 + memory_len / progress_step / BLOCKSZ;
		if (len < 64)
			break;
	}
	printf (verify_only ? "Verify: " : "Write: " );
	print_symbols ('.', len);
	print_symbols ('\b', len);
	fflush (stdout);

	progress_count = 0;
	t0 = fix_time ();
	for (addr=0; (int)addr<memory_len; addr+=BLOCKSZ) {
		len = BLOCKSZ;
		if (memory_len - addr < len)
			len = memory_len - addr;
		if (! verify_only)
			write_block (multicore, addr, len);
		progress ();
		verify_block (multicore, addr, len);
	}
	printf ("# done\n");
	printf ("Rate: %ld bytes per second\n",
		memory_len * 1000L / mseconds_elapsed (t0));
}

void do_read (char *filename)
{
	FILE *fd;
	unsigned len, addr, data [BLOCKSZ/4];
	void *t0;

	fd = fopen (filename, "wb");
	if (! fd) {
		perror (filename);
		exit (1);
	}
	multicore_init ();
	printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
		memory_base + memory_len, memory_len);

	/* Open and detect the device. */
	atexit (quit);
	multicore = multicore_open ();
	if (! multicore) {
		fprintf (stderr, "Error detecting device -- check cable!\n");
		exit (1);
	}
	configure ();
	for (progress_step=1; ; progress_step<<=1) {
		len = 1 + memory_len / progress_step / BLOCKSZ;
		if (len < 64)
			break;
	}
	printf ("Read: " );
	print_symbols ('.', len);
	print_symbols ('\b', len);
	fflush (stdout);

	progress_count = 0;
	t0 = fix_time ();
	for (addr=0; (int)addr<memory_len; addr+=BLOCKSZ) {
		len = BLOCKSZ;
		if (memory_len - addr < len)
			len = memory_len - addr;
		progress ();

		multicore_read_block (multicore, memory_base + addr,
			(len + 3) / 4, data);
		if (fwrite (data, 1, len, fd) != len) {
			fprintf (stderr, "%s: write error!\n", filename);
			exit (1);
		}
	}
	printf ("# done\n");
	printf ("Rate: %ld bytes per second\n",
		memory_len * 1000L / mseconds_elapsed (t0));
	fclose (fd);
}

int main (int argc, char **argv)
{
	int ch, read_mode = 0, memory_write_mode = 0;

	setvbuf (stdout, (char *)NULL, _IOLBF, 0);
	setvbuf (stderr, (char *)NULL, _IOLBF, 0);
	printf (PROGNAME ", Version " VERSION "\n");
	progname = argv[0];

	while ((ch = getopt(argc, argv, "vDhrwb:")) != -1) {
		switch (ch) {
		case 'v':
			++verify_only;
			continue;
		case 'D':
			++debug;
			continue;
		case 'r':
			++read_mode;
			continue;
		case 'w':
			++memory_write_mode;
			continue;
		case 'b':
			board = optarg;
			continue;
		case 'h':
			break;
		}
usage:		printf ("Probe:\n");
		printf ("        mcprog\n");
		printf ("\nWrite flash memory:\n");
		printf ("        mcprog [-v] file.sre\n");
		printf ("        mcprog [-v] file.bin [address]\n");
		printf ("\nWrite static memory:\n");
		printf ("        mcprog -w [-v] file.sre\n");
		printf ("        mcprog -w [-v] file.bin [address]\n");
		printf ("\nRead memory:\n");
		printf ("        mcprog -r file.bin address length\n");
		printf ("\nArgs:\n");
		printf ("        file.sre   Code file SREC format\n");
		printf ("        file.bin   Code file in binary format\n");
		printf ("        address    Address of flash memory, default 0x%08X\n",
			DEFAULT_ADDR);
		printf ("        -v         Verify only\n");
		printf ("        -w         Memory write mode\n");
		printf ("        -r         Read mode\n");
		printf ("        -b name    Specify board name\n");
		exit (0);
	}
	argc -= optind;
	argv += optind;
	switch (argc) {
	case 0:
		do_probe ();
		break;
	case 1:
		memory_len = read_srec (argv[0], memory_data);
		if (memory_len == 0) {
			memory_base = DEFAULT_ADDR;
			memory_len = read_bin (argv[0], memory_data);
		}
		if (memory_write_mode)
			do_write ();
		else
			do_program ();
		break;
	case 2:
		memory_base = strtoul (argv[1], 0, 0);
		memory_len = read_bin (argv[0], memory_data);
		if (memory_write_mode)
			do_write ();
		else
			do_program ();
		break;
	case 3:
		if (! read_mode)
			goto usage;
		memory_base = strtoul (argv[1], 0, 0);
		memory_len = strtoul (argv[2], 0, 0);
		do_read (argv[0]);
		break;
	default:
		goto usage;
	}
	quit ();
	return 0;
}
