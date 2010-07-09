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
#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>
#include "target.h"
#include "conf.h"
#include "swinfo.h"

#define PROGNAME        "Programmer for Elvees MIPS32 processors"
#define COPYRIGHT       "Copyright (C) 2010 Serge Vakulenko"
#define VERSION         "1.8"
#define BLOCKSZ         1024
#define DEFAULT_ADDR    0xBFC00000

/* Macros for converting between hex and binary. */
#define NIBBLE(x)       (isdigit(x) ? (x)-'0' : tolower(x)+10-'a')
#define HEX(buffer)     ((NIBBLE((buffer)[0])<<4) + NIBBLE((buffer)[1]))

unsigned char memory_data [0x800000];   /* Code - up to 8 Mbytes */
int memory_len;
unsigned memory_base;
unsigned start_addr = DEFAULT_ADDR;
unsigned progress_count, progress_step;
int check_erase;
int verify_only;
int debug_level;
target_t *target;
char *progname;
char *confname;
char *board;
char *board_serial = 0;

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
    if (output_len < 0) {
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

/*
 * Compute data checksum using rot13 algorithm.
 * Link: http://vak.ru/doku.php/proj/hash/efficiency
 */
static unsigned compute_checksum (unsigned sum, const unsigned char *data, unsigned bytes)
{
    while (bytes-- > 0) {
        sum += *data++;
        sum -= (sum << 13) | (sum >> 19);
        /* Two shifts are converted by GCC 4
         * to a single rotation instruction. */
    }
    return sum;
}

void print_symbols (char symbol, int cnt)
{
    while (cnt-- > 0)
        putchar (symbol);
}

void print_board_info (sw_info *pinfo)
{
    struct tm *creat_date;
    if (pinfo) {
	if (isprint (pinfo->board_sn[0]))
            printf ("Board serial number:      %s\n", pinfo->board_sn);
	if (isprint (pinfo->sw_version[0]))
            printf ("Software version:         %s\n", pinfo->sw_version);
        if (isprint (pinfo->filename[0])) {
            printf ("Software file name:       %s\n", pinfo->filename);
			creat_date = localtime (&pinfo->time);
            printf ("Date of creation:         %02d.%02d.%04d\n",
                     creat_date->tm_mday, creat_date->tm_mon + 1, creat_date->tm_year + 1900);
            printf ("Memory size:              %d\n", pinfo->len);
            printf ("CRC:                      %08X\n\n", pinfo->crc);
        }
    } else {
        printf ("No information\n\n");
    }
}

void program_block (target_t *mc, unsigned addr, int len)
{
    /* Write flash memory. */
    target_program_block (mc, memory_base + addr,
        (len + 3) / 4, (unsigned*) (memory_data + addr));
}

void write_block (target_t *mc, unsigned addr, int len)
{
    /* Write static memory. */
    target_write_block (mc, memory_base + addr,
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

void verify_block (target_t *mc, unsigned addr, int len)
{
    int i;
    unsigned word, expected, block [BLOCKSZ/4];

//printf("memory_base+addr=0x%x;(len+3)/4=%d\n",memory_base+addr,(len+3)/4);
    target_read_block (mc, memory_base + addr, (len+3)/4, block);
//printf("block[0]=%x\n",block[0]);
    for (i=0; i<len; i+=4) {
        expected = *(unsigned*) (memory_data + addr + i);
//      if (expected == 0xffffffff)
//          continue;
        word = block [i/4];
        if (debug_level > 1)
            printf ("read word %08X at address %08X\n",
                word, addr + i + memory_base);
        while (word != expected) {
            /* Возможно, не все нули прописались в flash-память.
             * Пробуем повторить операцию. */
//printf ("\nerror at address %08X: file=%08X, mem=%08X ",
//addr + i + memory_base, expected, word); fflush (stdout);
            if (verify_only || ! target_flash_rewrite (mc,
                memory_base + addr + i, expected)) {
                printf ("\nerror at address %08X: file=%08X, mem=%08X\n",
                    addr + i + memory_base, expected, word);
                exit (1);
//              break;
            }
            printf ("%%\b");
            fflush (stdout);
            word = target_read_word (mc, memory_base + addr + i);
        }
    }
}

void probe_flash (target_t *mc, unsigned base)
{
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];

    printf ("Flash at %08X: ", base);
    if (! target_flash_detect (mc, base, &mfcode, &devcode,
        mfname, devname, &bytes, &width)) {
        printf ("Incorrect id %08X\n", devcode);
        return;
    }
    printf ("%s %s ", mfname, devname);
    if (width == 8)
        printf ("(id %02x/%02x)", mfcode & 0xFF, devcode & 0xFF);
    else
        printf ("(id %08x/%08x)", mfcode, devcode);
    if (bytes % (1024*1024) == 0)
        printf (", %d Mbytes, %d bit wide\n", bytes / 1024 / 1024, width);
    else
        printf (", %d kbytes, %d bit wide\n", bytes / 1024, width);
}

void quit (void)
{
    if (target != 0) {
        if (start_addr != DEFAULT_ADDR)
            printf ("Start: %08X\n", start_addr);
        target_run (target, start_addr);
        target_close (target);
        free (target);
        target = 0;
    }
}

void interrupted (int signum)
{
    quit();
    _exit (-1);
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
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F4008, word);

    } else if (strcasecmp (param, "cscon0") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1000, word);

    } else if (strcasecmp (param, "cscon1") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1004, word);

    } else if (strcasecmp (param, "cscon2") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1008, word);

    } else if (strcasecmp (param, "cscon3") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F100C, word);

    } else if (strcasecmp (param, "cr_pll") == 0) {
        sscanf(value,"%x",&word);
        target_write_word (target, 0x182F4000, word);
//printf("CR_PLL=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f4000));

    } else if (strcasecmp (param, "clk_en") == 0) {
        sscanf(value,"%x",&word);
        target_write_word (target, 0x182F4004, word);
//printf("CLK_EN=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f4004));
    } else if (strncasecmp (param, "flash ", 6) == 0) {
        if (sscanf (value, "%i-%i", &first, &last) != 2) {
            fprintf (stderr, "%s: incorrect value for parameter `%s'\n",
                confname, param);
            exit (-1);
        }
        printf ("  %s = %08X-%08X\n", param, first, last);
        target_flash_configure (target, first, last);
    } else {
        fprintf (stderr, "%s: unknown parameter `%s'\n",
            confname, param);
        exit (-1);
    }
/*  printf ("Configure: %s = %08X\n", param, word);*/
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
    atexit (quit);
    target = target_open (1);
    if (! target) {
        fprintf (stderr, "Error detecting device -- check cable!\n");
        exit (1);
    }
    target_stop (target);
    printf ("Processor: %s (id %08X)\n", target_cpu_name (target),
        target_idcode (target));

    configure ();

    /* Probe all configured flash regions. */
    addr = ~0;
    for (;;) {
        addr = target_flash_next (target, addr, &last);
        if (! ~addr)
            break;
        probe_flash (target, addr);
    }
}

/*
 * Check chip clean.
 */
static int check_clean (target_t *t, unsigned addr)
{
    unsigned offset, i, sz, end, mem [16*1024];

    sz = target_flash_bytes (t);
    addr &= ~(sz-1);
    end = addr + sz;
    for (offset=0; addr+offset<end; offset+=sizeof(mem)) {
        sz = sizeof (mem);
        if (sz + offset > end)
            sz = end - offset;
        sz /= sizeof(unsigned);
        target_read_block (t, addr + offset, sz, mem);
        for (i=0; i<sz; i++) {
            if (mem[i] != 0xffffffff)
                return 0;
        }
    }
    printf ("Clean flash: %08X\n", addr);
    return 1;
};

void do_program (char *filename, int store_info)
{
    unsigned addr;
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];
    int len;
    void *t0;
    sw_info *pinfo;
	sw_info zero_sw_info;
    struct stat file_stat;

    printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
        memory_base + memory_len, memory_len);

    if (store_info) {
        /* Store length and checksum. */
	len = (memory_len < AREA_SIZE) ? (memory_len) : (AREA_SIZE);
        pinfo = find_info ((char *)memory_data, len);
        if (!pinfo) {
                printf ("No software information label found. Did you labeled it with verstamp utility?\n");
                exit(1);
        }
        memset ( &pinfo->len, 0, sizeof(sw_info) - sizeof(pinfo->label));
        pinfo->len = memory_len;
        if (board_serial) {
	    if (strlen (board_serial) > sizeof(pinfo->board_sn))
		printf ("Warning: board number is too large. Must be %d bytes at most. Will be cut\n",
			sizeof(pinfo->board_sn));
	    strcpy (pinfo->board_sn, board_serial);
	}
	strcpy (pinfo->filename, basename(filename));
	stat (filename, &file_stat);
	pinfo->time = file_stat.st_mtime;

	/* Calculating checksum by parts. Instead of sw_info zeroes are summed */
	memset ( &zero_sw_info, 0, sizeof(sw_info));
	pinfo->crc = compute_checksum (0, memory_data, (char*)pinfo - (char*)memory_data);
	pinfo->crc = compute_checksum (pinfo->crc, (unsigned char*) &zero_sw_info, sizeof(sw_info));
	pinfo->crc = compute_checksum (pinfo->crc, (unsigned char*) pinfo + sizeof(sw_info),
	     memory_len - ((char *)pinfo + sizeof(sw_info) - (char *)memory_data));

	printf ("\nLoaded software information:");
	printf ("\n----------------------------\n");
        print_board_info (pinfo);
    }

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1);
    if (! target) {
        fprintf (stderr, "Error detecting device -- check cable!\n");
        exit (1);
    }
    target_stop (target);
    /*printf ("Processor: %s\n", target_cpu_name (target));*/

    configure ();
    if (! target_flash_detect (target, memory_base,
        &mfcode, &devcode, mfname, devname, &bytes, &width)) {
        printf ("No flash memory detected.\n");
        return;
    }
    printf ("Flash: %s %s", mfname, devname);
    if (bytes % (1024*1024) == 0)
        printf (", size %d Mbytes, %d bit wide\n", bytes / 1024 / 1024, width);
    else
        printf (", size %d kbytes, %d bit wide\n", bytes / 1024, width);

    if (! verify_only) {
        /* Erase flash. */
        if (! check_erase || ! check_clean (target, memory_base))
            target_erase (target, memory_base);
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
            program_block (target, addr, len);
        progress ();
        verify_block (target, addr, len);
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

    printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
        memory_base + memory_len, memory_len);

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1);
    if (! target) {
        fprintf (stderr, "Error detecting device -- check cable!\n");
        exit (1);
    }
    target_stop (target);
    /*printf ("Processor: %s\n", target_cpu_name (target));*/

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
            write_block (target, addr, len);
        progress ();
        verify_block (target, addr, len);
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
    printf ("Memory: %08X-%08X, total %d bytes\n", memory_base,
        memory_base + memory_len, memory_len);

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1);
    if (! target) {
        fprintf (stderr, "Error detecting device -- check cable!\n");
        exit (1);
    }
    target_stop (target);
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

        target_read_block (target, memory_base + addr,
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


void do_info()
{
    sw_info *pinfo;

    target = target_open (1);
    if (! target) {
        fprintf (stderr, "Error detecting device -- check cable!\n");
        exit (1);
    }

    target_stop (target);
    configure ();

    unsigned i;
    unsigned flash_base;
    for (i = 0; i < NFLASH; ++i) {
	flash_base = target_flash_address(target, i);
        if (flash_base == ~0) break;

        target_read_block (target, flash_base, (AREA_SIZE + 3) / 4, (unsigned *)memory_data);

        printf ("\nFlash #%d, address %08X\n", i, flash_base);
	printf ("----------------------------------\n");
        pinfo = find_info ((char *)memory_data, AREA_SIZE);
        print_board_info (pinfo);
    }
}

/*
 * Print copying part of license
 */
static void gpl_show_copying (void)
{
    printf (COPYRIGHT ".\n");
    printf ("\n");
    printf ("This program is free software; you can redistribute it and/or modify\n");
    printf ("it under the terms of the GNU General Public License as published by\n");
    printf ("the Free Software Foundation; either version 2 of the License, or\n");
    printf ("(at your option) any later version.\n");
    printf ("\n");
    printf ("This program is distributed in the hope that it will be useful,\n");
    printf ("but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
    printf ("MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n");
    printf ("GNU General Public License for more details.\n");
    printf ("\n");
}

/*
 * Print NO WARRANTY part of license
 */
static void gpl_show_warranty (void)
{
    printf (COPYRIGHT ".\n");
    printf ("\n");
    printf ("BECAUSE THE PROGRAM IS LICENSED FREE OF CHARGE, THERE IS NO WARRANTY\n");
    printf ("FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW.  EXCEPT WHEN\n");
    printf ("OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES\n");
    printf ("PROVIDE THE PROGRAM \"AS IS\" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED\n");
    printf ("OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF\n");
    printf ("MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE ENTIRE RISK AS\n");
    printf ("TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU.  SHOULD THE\n");
    printf ("PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING,\n");
    printf ("REPAIR OR CORRECTION.\n");
    printf("\n");
    printf ("IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING\n");
    printf ("WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MAY MODIFY AND/OR\n");
    printf ("REDISTRIBUTE THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES,\n");
    printf ("INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING\n");
    printf ("OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED\n");
    printf ("TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY\n");
    printf ("YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER\n");
    printf ("PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE\n");
    printf ("POSSIBILITY OF SUCH DAMAGES.\n");
    printf("\n");
}

int main (int argc, char **argv)
{
    int ch, read_mode = 0, memory_write_mode = 0, info_mode = 0, store_info = 0;
    static const struct option long_options[] = {
        { "help",        0, 0, 'h' },
        { "warranty",    0, 0, 'W' },
        { "copying",     0, 0, 'C' },
        { "version",     0, 0, 'V' },
        { NULL,          0, 0, 0 },
    };

    setvbuf (stdout, (char *)NULL, _IOLBF, 0);
    setvbuf (stderr, (char *)NULL, _IOLBF, 0);
    printf (PROGNAME ", Version " VERSION "\n");
    progname = argv[0];
    signal (SIGINT, interrupted);
#ifdef __linux__
    signal (SIGHUP, interrupted);
#endif
    signal (SIGTERM, interrupted);

    while ((ch = getopt_long (argc, argv, "vDhriwb:sn:cg:CVW",
      long_options, 0)) != -1) {
        switch (ch) {
        case 'c':
            ++check_erase;
            continue;
        case 'v':
            ++verify_only;
            continue;
        case 'D':
            ++debug_level;
            continue;
        case 'r':
            ++read_mode;
            continue;
        case 'i':
            ++info_mode;
            continue;
        case 'w':
            ++memory_write_mode;
            continue;
        case 'g':
            start_addr = strtoul (optarg, 0, 0);
            continue;
        case 'b':
            board = optarg;
            continue;
        case 'n':
            board_serial = optarg;
            continue;
        case 's':
            ++store_info;
            continue;
        case 'h':
            break;
        case 'V':
            /* Version already printed above. */
            return 0;
        case 'C':
            gpl_show_copying ();
            return 0;
        case 'W':
            gpl_show_warranty ();
            return 0;
        }
usage:
        printf (COPYRIGHT ".\n\n");
        printf ("MCprog comes with ABSOLUTELY NO WARRANTY; for details\n");
        printf ("use `--warranty' option. This is Open Source software. You are\n");
        printf ("welcome to redistribute it under certain conditions. Use the\n");
        printf ("'--copying' option for details.\n\n");
        printf ("Probe:\n");
        printf ("       mcprog\n");
        printf ("\nWrite flash memory:\n");
        printf ("       mcprog [-v] file.sre\n");
        printf ("       mcprog [-v] file.bin [address]\n");
        printf ("\nWrite static memory:\n");
        printf ("       mcprog -w [-v] [-g address] file.sre\n");
        printf ("       mcprog -w [-v] [-g address] file.bin [address]\n");
        printf ("\nRead memory:\n");
        printf ("       mcprog -r file.bin address length\n");
        printf ("\nArgs:\n");
        printf ("       file.sre            Code file SREC format\n");
        printf ("       file.bin            Code file in binary format\n");
        printf ("       address             Address of flash memory, default 0x%08X\n",
            DEFAULT_ADDR);
        printf ("       -c                  Check clean\n");
        printf ("       -v                  Verify only\n");
        printf ("       -w                  Memory write mode\n");
        printf ("       -r                  Read mode\n");
	printf ("       -i                  Read software information\n");
        printf ("       -b type             Specify board type\n");
        printf ("       -s                  Compute and store software information\n");
	printf ("       -n serial           Specify board serial number\n");
        printf ("       -g addr             Start execution from address\n");
        printf ("       -D                  Debug mode\n");
        printf ("       -h, --help          Print this help message\n");
        printf ("       -V, --version       Print version\n");
        printf ("       -C, --copying       Print copying information\n");
        printf ("       -W, --warranty      Print warranty information\n");
        printf ("\n");
        return 0;
    }
    printf (COPYRIGHT ".\n");
    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        if (info_mode) {
            memory_base = DEFAULT_ADDR;
            do_info();
	} else {
            do_probe ();
        }
        break;
    case 1:
        memory_len = read_srec (argv[0], memory_data);
        if (memory_len == 0) {
            memory_base = DEFAULT_ADDR;
            memory_len = read_bin (argv[0], memory_data);
        }
        if (info_mode)
            do_info();
        else if (memory_write_mode)
            do_write ();
        else
            do_program (argv[0], store_info);
        break;
    case 2:
        memory_base = strtoul (argv[1], 0, 0);
        memory_len = read_bin (argv[0], memory_data);
        if (memory_write_mode)
            do_write ();
        else
            do_program (argv[0], store_info);
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
