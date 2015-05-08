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
#include <locale.h>

#include "target.h"
#include "conf.h"
#include "swinfo.h"
#include "localize.h"

#define VERSION         "1.90"
#define BLOCKSZ         1024
#define DEFAULT_ADDR    0xBFC00000

/* Macros for converting between hex and binary. */
#define NIBBLE(x)       (isdigit(x) ? (x)-'0' : tolower(x)+10-'a')
#define HEX(buffer)     ((NIBBLE((buffer)[0])<<4) + NIBBLE((buffer)[1]))

unsigned char memory_data [0x1000000];   /* Code - up to 16 Mbytes */
int memory_len;
unsigned memory_base;
unsigned start_addr = DEFAULT_ADDR;
unsigned progress_count, progress_step;
int disable_block = 0;
int erase_mode = -1;
int check_erase;
int verify_only;
int debug_level;
target_t *target;
char *progname;
char *confname;
char *board;
char *board_serial = 0;
const char *copyright;

/*
 * Check heximal string
 */
int check_hex (char *str)
{
    int i, n;

    if ((str[0]!='0')||(str[1]!='x')) return 1;
    n = strlen(str);
    if (n > 10) return 1;
    for (i=2; i<n; i++) {
        if (! isxdigit(str[i])) return 1;
    };
    return 0;
};

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
        fprintf (stderr, _("%s: read error\n"), filename);
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
            fprintf (stderr, _("%s: bad file format\n"), filename);
            exit (1);
        }
        if (buf[1] == '7' || buf[1] == '8' || buf[1] == '9')
            break;

        /* Starting an S-record.  */
        if (! isxdigit (buf[2]) || ! isxdigit (buf[3])) {
            fprintf (stderr, _("%s: bad record: %s\n"), filename, buf);
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
                fprintf (stderr, _("%s: incorrect address %08X, must be %08X or greater\n"),
                    filename, address, memory_base);
                exit (1);
            }
            address -= memory_base;
            if (address+bytes > sizeof (memory_data)) {
                fprintf (stderr, _("%s: address too large: %08X + %08X\n"),
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
 * Read HEX file.
 */
int read_hex (char *filename, unsigned char *output)
{
    FILE *fd;
    unsigned char buf [256], data[16], record_type, sum;
    unsigned address, high;
    int bytes, output_len, i;

    fd = fopen (filename, "r");
    if (! fd) {
        perror (filename);
        exit (1);
    }
    output_len = 0;
    high = 0;
    while (fgets ((char*) buf, sizeof(buf), fd)) {
        if (buf[0] == '\n')
            continue;
        if (buf[0] != ':') {
            if (output_len == 0)
                break;
            fprintf (stderr, _("%s: bad HEX file format\n"), filename);
            exit (1);
        }
        if (! isxdigit (buf[1]) || ! isxdigit (buf[2]) ||
            ! isxdigit (buf[3]) || ! isxdigit (buf[4]) ||
            ! isxdigit (buf[5]) || ! isxdigit (buf[6]) ||
            ! isxdigit (buf[7]) || ! isxdigit (buf[8])) {
            fprintf (stderr, _("%s: bad record: %s\n"), filename, buf);
            exit (1);
        }
    record_type = HEX (buf+7);
    if (record_type == 1) {
        /* End of file. */
            break;
        }
    if (record_type == 5) {
        /* Start address, ignore. */
        continue;
    }

    bytes = HEX (buf+1);
        if (bytes & 1) {
            fprintf (stderr, _("%s: odd length\n"), filename);
            exit (1);
        }
    if (strlen ((char*) buf) < bytes * 2 + 11) {
            fprintf (stderr, _("%s: too short hex line\n"), filename);
            exit (1);
        }
    address = high << 16 | HEX (buf+3) << 8 | HEX (buf+5);
        if (address & 3) {
            fprintf (stderr, _("%s: odd address\n"), filename);
            exit (1);
        }

    sum = 0;
    for (i=0; i<bytes; ++i) {
            data [i] = HEX (buf+9 + i + i);
        sum += data [i];
    }
    sum += record_type + bytes + (address & 0xff) + (address >> 8 & 0xff);
    if (sum != (unsigned char) - HEX (buf+9 + bytes + bytes)) {
            fprintf (stderr, _("%s: bad hex checksum\n"), filename);
            exit (1);
        }

    if (record_type == 4) {
        /* Extended address. */
            if (bytes != 2) {
                fprintf (stderr, _("%s: invalid hex linear address record length\n"),
                    filename);
                exit (1);
            }
        high = data[0] << 8 | data[1];
        continue;
    }
    if (record_type != 0) {
            fprintf (stderr, _("%s: unknown hex record type: %d\n"),
                filename, record_type);
            exit (1);
        }

        /* Data record found. */
        if (! memory_base) {
            /* Автоматическое определение базового адреса. */
            memory_base = address;
        }
        if (address < memory_base) {
            fprintf (stderr, _("%s: incorrect address %08X, must be %08X or greater\n"),
                filename, address, memory_base);
            exit (1);
        }
        address -= memory_base;
        if (address+bytes > sizeof (memory_data)) {
            fprintf (stderr, _("%s: address too large: %08X + %08X\n"),
                filename, address + memory_base, bytes);
            exit (1);
        }
        for (i=0; i<bytes; i++) {
            output[address++] = data [i];
        }
        if (output_len < (int) address)
            output_len = address;
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
            printf (_("Board serial number:      %s\n"), pinfo->board_sn);
    if (isprint (pinfo->sw_version[0]))
            printf (_("Software version:         %s\n"), pinfo->sw_version);
        if (isprint (pinfo->filename[0])) {
            printf (_("Software file name:       %s\n"), pinfo->filename);
            creat_date = localtime (&pinfo->time);
            printf (_("Date of creation:         %02d.%02d.%04d\n"),
                     creat_date->tm_mday, creat_date->tm_mon + 1, creat_date->tm_year + 1900);
            printf (_("Memory size:              %d\n"), pinfo->len);
            printf (_("CRC:                      %08X\n\n"), pinfo->crc);
        }
    } else {
        printf (_("No information\n\n"));
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
    int try;
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
            printf (_("read word %08X at address %08X\n"),
                word, addr + i + memory_base);
        try = 0;
        while (word != expected) {
            /* Возможно, не все нули прописались в flash-память.
             * Пробуем повторить операцию. */
            if (verify_only || ! target_flash_rewrite (mc,
                memory_base + addr + i, expected)) {
                printf (_("\nerror at address %08X: file=%08X, mem=%08X\n"),
                    addr + i + memory_base, expected, word);
                exit (1);
//              break;
            }
            printf ("%%\b");
            fflush (stdout);
            word = target_read_word (mc, memory_base + addr + i);
            if (++try > 3) {
                printf (_("\nerror at address %08X: file=%08X, mem=%08X\n"),
                    addr + i + memory_base, expected, word);
                exit (1);
            }
        }
    }
}

void probe_flash (target_t *mc, unsigned base)
{
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];

    printf (_("Flash at %08X: "), base);
    if (! target_flash_detect (mc, base, &mfcode, &devcode,
        mfname, devname, &bytes, &width)) {
        printf (_("Incorrect id %08X\n"), devcode);
        return;
    }
    printf ("%s %s ", mfname, devname);
    if (width == 8)
        printf ("(id %02x/%02x)", mfcode & 0xFF, devcode & 0xFF);
    else
        printf ("(id %08x/%08x)", mfcode, devcode);
    if (bytes % (1024*1024) == 0)
        printf (_(", %d Mbytes, %d bit wide\n"), bytes / 1024 / 1024, width);
    else
        printf (_(", %d kbytes, %d bit wide\n"), bytes / 1024, width);
}

void quit (void)
{
    if (target != 0) {
        if (start_addr != DEFAULT_ADDR)
            printf (_("Start: %08X\n"), start_addr);
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
    unsigned word, first, last, addr;

//fprintf (stderr, "section=%s, param=%s, value=%s\n", section, param, value);
    if (! section) {
        /* Remember default board name. */
        if (strcasecmp (param, "default") == 0) {
            if (! board)
                board = strdup (value);
            printf (_("Board: %s\n"), board);
        } else {
            fprintf (stderr, _("%s: unknown parameter `%s'\n"),
                confname, param);
            exit (-1);
        }
        return;
    }
    if (! board) {
        fprintf (stderr, _("%s: parameter 'default' missing\n"), confname);
        exit (-1);
    }
    if (strcasecmp (section, board) != 0)
        return;

    /* Needed section found. */
    if (strcasecmp (param, "csr") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F4008, word);
        if (debug_level > 1)
            printf("CSR=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f4008));
    } else if (strcasecmp (param, "cscon0") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1000, word);
        if (debug_level > 1)
            printf("CSCON0=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f1000));
    } else if (strcasecmp (param, "cscon1") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1004, word);
        if (debug_level > 1)
            printf("CSCON1=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f1004));
    } else if (strcasecmp (param, "cscon2") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1008, word);
        if (debug_level > 1)
            printf("CSCON2=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f1008));
    } else if (strcasecmp (param, "cscon3") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F100C, word);
        target_set_cscon3 (target, word);
        if (debug_level > 1)
            printf("CSCON3=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f100c));
    } else if (strcasecmp (param, "sdrcon") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1014, word);
        if (debug_level > 1)
            printf("SDRCON=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f1014));
    } else if (strcasecmp (param, "sdrtmr") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F1018, word);
        if (debug_level > 1)
            printf("SDRTMR=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f1018));
    } else if (strcasecmp (param, "sdrcsr") == 0) {
        word = strtoul (value, 0, 0);
        target_write_word (target, 0x182F101C, word);
        if (debug_level > 1)
            printf("SDRCSR=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f101c));
    } else if (strcasecmp (param, "cr_pll") == 0) {
        sscanf(value,"%x",&word);
        target_write_word (target, 0x182F4000, word);
        if (debug_level > 1)
            printf("CR_PLL=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f4000));
    } else if (strcasecmp (param, "clk_en") == 0) {
        sscanf(value,"%x",&word);
        target_write_word (target, 0x182F4004, word);
        if (debug_level > 1)
            printf("CLK_EN=%08x (%s)(%08x)\n",word,value,target_read_word(target,0x182f4004));
    } else if (strncasecmp (param, "flash ", 6) == 0) {
        if (sscanf (value, "%i-%i", &first, &last) != 2) {
            fprintf (stderr, _("%s: incorrect value for parameter `%s'\n"),
                confname, param);
            exit (-1);
        }
        printf ("  %s = %08X-%08X\n", param, first, last);
        target_flash_configure (target, first, last);
    } else if ((check_hex(param) == 0) && (check_hex(value) == 0)) {
        sscanf(param,"%x",&addr);
        sscanf(value,"%x",&word);
        target_write_word (target, addr, word);
        if (debug_level > 1)
            printf("%08x=%08x (%s)(%08x)\n",addr,word,value,target_read_word(target,addr));
    } else {
        fprintf (stderr, _("%s: unknown parameter `%s'\n"),
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
                fprintf (stderr, _("%s: out of memory\n"), progname);
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
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    printf (_("Processor: %s (id %08X)\n"), target_cpu_name (target),
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
    unsigned offset, i, sz, mem_size, end, mem [16*1024];
    void *t0;
    int len;

    mem_size = target_flash_bytes (t);
    addr &= ~(mem_size-1);
    end = addr + mem_size;

    printf (_("Checking clean: "));

    for (progress_step=1; ; progress_step<<=1) {
        len = 1 + mem_size / progress_step / sizeof(mem);
        if (len < 64)
            break;
    }

    print_symbols ('.', len);
    print_symbols ('\b', len);
    fflush (stdout);

    progress_count = 0;
    t0 = fix_time ();

    for (offset=0; addr+offset<end; offset+=sizeof(mem)) {
        sz = sizeof (mem);
        if (sz + offset > end)
            sz = end - offset;
        sz /= sizeof(unsigned);
        target_read_block (t, addr + offset, sz, mem);
        for (i=0; i<sz; i++) {
            if (mem[i] != 0xffffffff) {
                printf (_("Flash @ %08X is NOT clean!\n"), addr);
                return 0;
            }
        }
        progress ();
    }
    printf (_("# done\n"));
    printf (_("Rate: %ld bytes per second\n"),
        mem_size * 1000L / mseconds_elapsed (t0));
    printf (_("Flash @ %08X is clean!\n"), addr);

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

    if (erase_mode < 0)
        erase_mode = 1; // default erase mode

    printf (_("Memory: %08X-%08X, total %d bytes\n"), memory_base,
        memory_base + memory_len, memory_len);

    if (store_info) {
        /* Store length and checksum. */
    len = (memory_len < AREA_SIZE) ? (memory_len) : (AREA_SIZE);
        pinfo = find_info ((char *)memory_data, len);
        if (!pinfo) {
                printf (_("No software information label found. Did you labeled it with verstamp utility?\n"));
                exit(1);
        }
        memset ( &pinfo->len, 0, sizeof(sw_info) - sizeof(pinfo->label));
        pinfo->len = memory_len;
        if (board_serial) {
        if (strlen (board_serial) > sizeof(pinfo->board_sn))
        printf (_("Warning: board number is too large. Must be %ld bytes at most. Will be cut\n"),
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

    printf (_("\nLoaded software information:\n----------------------------\n"));
        print_board_info (pinfo);
    }

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    printf (_("Processor: %s\n"), target_cpu_name (target));

    configure ();
    if (! target_flash_detect (target, memory_base,
        &mfcode, &devcode, mfname, devname, &bytes, &width)) {
        printf (_("No flash memory detected.\n"));
        return;
    }
    printf (_("Flash: %s %s"), mfname, devname);
    if (bytes % (1024*1024) == 0)
        printf (_(", size %d Mbytes, %d bit wide\n"), bytes / 1024 / 1024, width);
    else
        printf (_(", size %d kbytes, %d bit wide\n"), bytes / 1024, width);

    if (! verify_only) {
        /* Erase flash. */
        if (! check_erase || ! check_clean (target, memory_base)) {
        if (erase_mode != 0)
          target_erase (target, memory_base);
    }
    }
    for (progress_step=1; ; progress_step<<=1) {
        len = 1 + memory_len / progress_step / BLOCKSZ;
        if (len < 64)
            break;
    }
    printf (verify_only ? _("Verify: ") : _("Program: "));
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
    printf (_("# done\n"));
    printf (_("Rate: %ld bytes per second\n"),
        memory_len * 1000L / mseconds_elapsed (t0));
}

void do_write ()
{
    unsigned addr;
    int len;
    void *t0;

    printf (_("Memory: %08X-%08X, total %d bytes\n"), memory_base,
        memory_base + memory_len, memory_len);

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    printf (_("Processor: %s\n"), target_cpu_name (target));

    configure ();
    for (progress_step=1; ; progress_step<<=1) {
        len = 1 + memory_len / progress_step / BLOCKSZ;
        if (len < 64)
            break;
    }
    printf (verify_only ? _("Verify: ") : _("Write: "));
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
    printf (_("# done\n"));
    printf (_("Rate: %ld bytes per second\n"),
        memory_len * 1000L / mseconds_elapsed (t0));
}

void do_read (char *filename)
{
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];
    FILE *fd;
    unsigned len, addr, data [BLOCKSZ/4];
    void *t0;

    fd = fopen (filename, "wb");
    if (! fd) {
        perror (filename);
        exit (1);
    }
    printf (_("Memory: %08X-%08X, total %d bytes\n"), memory_base,
        memory_base + memory_len, memory_len);

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    configure ();

    if (! target_flash_detect (target, memory_base,
        &mfcode, &devcode, mfname, devname, &bytes, &width)) {
        printf (_("No flash memory detected.\n"));
        return;
    }
    printf (_("Flash: %s %s"), mfname, devname);
    if (bytes % (1024*1024) == 0)
        printf (_(", size %d Mbytes, %d bit wide\n"), bytes / 1024 / 1024, width);
    else
        printf (_(", size %d kbytes, %d bit wide\n"), bytes / 1024, width);

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
    printf (_("# done\n"));
    printf (_("Rate: %ld bytes per second\n"),
        memory_len * 1000L / mseconds_elapsed (t0));
    fclose (fd);
}

void do_erase ()
{
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    printf (_("Processor: %s\n"), target_cpu_name (target));

    configure ();
    if (! target_flash_detect (target, memory_base,
        &mfcode, &devcode, mfname, devname, &bytes, &width)) {
        printf (_("No flash memory detected.\n"));
        return;
    }
    printf (_("Flash: %s %s"), mfname, devname);
    if (bytes % (1024*1024) == 0)
        printf (_(", size %d Mbytes, %d bit wide\n"), bytes / 1024 / 1024, width);
    else
        printf (_(", size %d kbytes, %d bit wide\n"), bytes / 1024, width);

    target_erase (target, memory_base);
}

void do_check_clean ()
{
    unsigned mfcode, devcode, bytes, width;
    char mfname[40], devname[40];

    /* Open and detect the device. */
    atexit (quit);
    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
        exit (1);
    }
    target_stop (target);
    printf (_("Processor: %s\n"), target_cpu_name (target));

    configure ();
    if (! target_flash_detect (target, memory_base,
        &mfcode, &devcode, mfname, devname, &bytes, &width)) {
        printf (_("No flash memory detected.\n"));
        return;
    }
    printf (_("Flash: %s %s"), mfname, devname);
    if (bytes % (1024*1024) == 0)
        printf (_(", size %d Mbytes, %d bit wide\n"), bytes / 1024 / 1024, width);
    else
        printf (_(", size %d kbytes, %d bit wide\n"), bytes / 1024, width);

    check_clean (target, memory_base);
}

void do_info()
{
    sw_info *pinfo;

    target = target_open (1, disable_block);
    if (! target) {
        fprintf (stderr, _("Error detecting device -- check cable!\n"));
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

        printf (_("\nFlash #%d, address %08X\n----------------------------------\n"),
            i, flash_base);
        pinfo = find_info ((char *)memory_data, AREA_SIZE);
        print_board_info (pinfo);
    }
}

/*
 * Print copying part of license
 */
static void gpl_show_copying (void)
{
    printf ("%s.\n\n", copyright);
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
    printf ("%s.\n\n", copyright);
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
    int erase_only = 0;
    static const struct option long_options[] = {
        { "help",        0, 0, 'h' },
        { "warranty",    0, 0, 'W' },
        { "copying",     0, 0, 'C' },
        { "version",     0, 0, 'V' },
        { NULL,          0, 0, 0 },
    };

    /* Set locale and message catalogs. */
    setlocale (LC_ALL, "");
#if defined (__CYGWIN32__) || defined (MINGW32)
    /* Files with localized messages should be placed in
     * the current directory or in c:/Program Files/mcprog. */
    if (access ("./ru/LC_MESSAGES/mcprog.mo", R_OK) == 0)
        bindtextdomain ("mcprog", ".");
    else
        bindtextdomain ("mcprog", "c:/Program Files/mcprog");
#else
    bindtextdomain ("mcprog", "/usr/local/share/locale");
#endif
    textdomain ("mcprog");

    setvbuf (stdout, (char *)NULL, _IOLBF, 0);
    setvbuf (stderr, (char *)NULL, _IOLBF, 0);
    printf (_("Programmer for Elvees MIPS32 processors, Version %s\n"), VERSION);
    progname = argv[0];
    copyright = _("Copyright (C) 2010-2013 Serge Vakulenko");
    signal (SIGINT, interrupted);
#ifdef __linux__
    signal (SIGHUP, interrupted);
#endif
    signal (SIGTERM, interrupted);

    while ((ch = getopt_long (argc, argv, "vDhriwb:sn:cg:CVWe:d",
      long_options, 0)) != -1) {
        switch (ch) {
        case 'E':
            ++erase_only;
            continue;
        case 'e':
            erase_mode=strtoul (optarg, 0, 0);
            if (erase_mode < 0 || erase_mode > 2)
                break;
            continue;
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
        case 'd':
            ++disable_block;
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
        printf ("%s.\n\n", copyright);
        printf ("MCprog comes with ABSOLUTELY NO WARRANTY; for details\n");
        printf ("use `--warranty' option. This is Open Source software. You are\n");
        printf ("welcome to redistribute it under certain conditions. Use the\n");
        printf ("'--copying' option for details.\n\n");
        printf ("Probe:\n");
        printf ("       mcprog\n");
        printf ("\nWrite flash memory:\n");
        printf ("       mcprog [-v][-e0,-e2] file.srec\n");
        printf ("       mcprog [-v][-e0,-e2] file.hex\n");
        printf ("       mcprog [-v][-e0,-e2] file.bin [address]\n");
        printf ("\nWrite static memory:\n");
        printf ("       mcprog -w [-v] [-g address] file.srec\n");
        printf ("       mcprog -w [-v] [-g address] file.hex\n");
        printf ("       mcprog -w [-v] [-g address] file.bin [address]\n");
        printf ("\nRead memory:\n");
        printf ("       mcprog -r file.bin address length\n");
        printf ("\nErase flash chip:\n");
        printf ("       mcprog -e1 [address]\n");
        printf ("\nCheck flash is clean:\n");
        printf ("       mcprog -c [address]\n");
        printf ("\nArgs:\n");
        printf ("       file.srec           Code file in SREC format\n");
        printf ("       file.hex            Code file in HEX format\n");
        printf ("       file.bin            Code file in binary format\n");
        printf ("       address             Address of flash memory, default 0x%08X\n",
            DEFAULT_ADDR);
        printf ("       -c                  Check clean\n");
        printf ("       -e erase            Erase mode\n");
        printf ("                           (0 - do not erase, 1 (default) - erase chip,\n");
        printf ("                            2 - erase only place for programmed file)\n");
        printf ("       -v                  Verify only\n");
        printf ("       -w                  Memory write mode\n");
        printf ("       -r                  Read mode\n");
        printf ("       -i                  Read software information\n");
        printf ("       -b type             Specify board type\n");
        printf ("       -s                  Compute and store software information\n");
        printf ("       -n serial           Specify board serial number\n");
        printf ("       -g addr             Start execution from address\n");
        printf ("       -d                  Disable block mode (only for Elvees USB JTAG adapter)\n");
        printf ("       -D                  Debug mode\n");
        printf ("       -h, --help          Print this help message\n");
        printf ("       -V, --version       Print version\n");
        printf ("       -C, --copying       Print copying information\n");
        printf ("       -W, --warranty      Print warranty information\n");
        printf ("\n");
        return 0;
    }
    printf ("%s.\n", copyright);
    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        if (info_mode) {
            memory_base = DEFAULT_ADDR;
            do_info();
        } else if (erase_mode == 1) {
            memory_base = DEFAULT_ADDR;
            do_erase ();
            if (check_erase) do_check_clean ();
        } else if (check_erase) {
            memory_base = DEFAULT_ADDR;
            do_check_clean ();
        } else {
            do_probe ();
        }
        break;
    case 1:
        if (erase_mode == 1) {
            memory_base = strtoul (argv[0], 0, 0);
            do_erase ();
            if (check_erase) do_check_clean ();
        } else if (check_erase) {
            memory_base = strtoul (argv[0], 0, 0);
            do_check_clean ();
        } else {
            memory_len = read_srec (argv[0], memory_data);
            if (memory_len == 0) {
                memory_len = read_hex (argv[0], memory_data);
                if (memory_len == 0) {
                    memory_base = DEFAULT_ADDR;
                    memory_len = read_bin (argv[0], memory_data);
                }
            }
            if (info_mode)
                do_info();
            else if (memory_write_mode)
                do_write ();
            else
                do_program (argv[0], store_info);
        }
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

