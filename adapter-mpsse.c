/*
 * Интерфейс через адаптер FT2232 к процессору Элвис Мультикор.
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
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <usb.h>

#include "adapter.h"
#include "oncd.h"

typedef struct {
    /* Общая часть. */
    adapter_t adapter;

    /* Доступ к устройству через libusb. */
    usb_dev_handle *usbdev;

    /* Буфер для посылаемого пакета MPSSE. */
    unsigned char output [256];
    int bytes_to_write;

    /* Буфер для принятых данных. */
    unsigned char input [64];
    int bytes_to_read;
    int bytes_per_word;
    unsigned long long fix_high_bit;
    unsigned long long high_byte_mask;
    unsigned long long high_bit_mask;
    unsigned high_byte_bits;
} mpsse_adapter_t;

/*
 * Можно использовать готовый адаптер Olimex ARM-USB-Tiny с переходником
 * с разъёма ARM 2x10 на разъём MIPS 2x5:
 *
 * Сигнал   Контакт ARM       Контакт MIPS
 * ------------------------------------
 * /TRST        3               3
 *  TDI         5               7
 *  TMS         7               5
 *  TCK         9               1
 *  TDO         13              9
 * /SYSRST      15              6
 *  GND         4,6,8,10,12,    2,8
 *              14,16,18,20
 */

/*
 * Identifiers of USB adapter.
 */
#define OLIMEX_VID              0x15ba
#define OLIMEX_PID              0x0004  /* ARM-USB-Tiny */

/*
 * USB endpoints.
 */
#define IN_EP                   0x02
#define OUT_EP                  0x81

/* Requests */
#define SIO_RESET               0 /* Reset the port */
#define SIO_MODEM_CTRL          1 /* Set the modem control register */
#define SIO_SET_FLOW_CTRL       2 /* Set flow control register */
#define SIO_SET_BAUD_RATE       3 /* Set baud rate */
#define SIO_SET_DATA            4 /* Set the data characteristics of the port */
#define SIO_POLL_MODEM_STATUS   5
#define SIO_SET_EVENT_CHAR      6
#define SIO_SET_ERROR_CHAR      7
#define SIO_SET_LATENCY_TIMER   9
#define SIO_GET_LATENCY_TIMER   10
#define SIO_SET_BITMODE         11
#define SIO_READ_PINS           12
#define SIO_READ_EEPROM         0x90
#define SIO_WRITE_EEPROM        0x91
#define SIO_ERASE_EEPROM        0x92

/* Биты регистра IRd */
#define IRd_RUN                 0x20    /* 0 - step mode, 1 - run continuosly */
#define IRd_READ                0x40    /* 0 - write, 1 - read registers */
#define IRd_FLUSH_PIPE          0x40    /* for EnGO: instruction pipe changed */
#define IRd_STEP_1CLK           0x80    /* for step mode: run for 1 clock only */

/* Команды MPSSE. */
#define CLKWNEG                 0x01
#define BITMODE                 0x02
#define CLKRNEG                 0x04
#define LSB                     0x08
#define WTDI                    0x10
#define RTDO                    0x20
#define WTMS                    0x40

static const char *oncd_regname[] = {
    "OSCR",     "OMBC",     "OMLR0",    "OMLR1",
    "OBCR",     "IRdec",    "OTC",      "PCdec",
    "PCexec",   "PCmem",    "PCfetch",  "OMAR",
    "OMDR",     "MEM",      "PCwb",     "EXIT",
};

static const char *oscr_bitname[] = {
    "SlctMEM",  "RO",       "TME",      "IME",
    "MPE",      "RDYm",     "MBO",      "TO",
    "SWO",      "SO",       "DBM",      "NDS",
    "VBO",      "NFEXP",    "WP0",      "WP1",
};

static void print_oscr_bits (unsigned value)
{
    int i;

    fprintf (stderr, " (");
    for (i=15; i>=0; i--)
        if (value & (1 << i)) {
            fprintf (stderr, "%s", oscr_bitname [i]);
            if (value & ((1 << i) - 1))
                fprintf (stderr, ",");
        }
    fprintf (stderr, ")");
}

/*
 * Посылка пакета данных USB-устройству.
 */
static void bulk_write (mpsse_adapter_t *a, unsigned char *output, int nbytes)
{
    int bytes_written;

    if (debug) {
        int i;
        fprintf (stderr, "usb bulk write %d bytes:", nbytes);
        for (i=0; i<nbytes; i++)
            fprintf (stderr, "%c%02x", i ? '-' : ' ', output[i]);
        fprintf (stderr, "\n");
    }
    bytes_written = usb_bulk_write (a->usbdev, IN_EP, (char*) output,
        nbytes, 1000);
    if (bytes_written < 0) {
        fprintf (stderr, "usb bulk write failed\n");
        exit (-1);
    }
    if (bytes_written != nbytes)
        fprintf (stderr, "usb bulk written %d bytes of %d",
            bytes_written, nbytes);

}

/*
 * Если в выходном буфере есть накопленные данные -
 * отправка их устройству.
 */
static void mpsse_flush_output (mpsse_adapter_t *a)
{
    int bytes_read, n;
    unsigned char reply [64];

    if (a->bytes_to_write <= 0)
        return;

    bulk_write (a, a->output, a->bytes_to_write);
    a->bytes_to_write = 0;
    if (a->bytes_to_read <= 0)
        return;

    /* Получаем ответ. */
    bytes_read = 0;
    while (bytes_read < a->bytes_to_read) {
        n = usb_bulk_read (a->usbdev, OUT_EP, (char*) reply,
            a->bytes_to_read - bytes_read + 2, 2000);
        if (n < 0) {
            fprintf (stderr, "usb bulk read failed\n");
            exit (-1);
        }
        if (debug) {
            if (n != a->bytes_to_read + 2)
                fprintf (stderr, "usb bulk read %d bytes of %d\n",
                    n, a->bytes_to_read - bytes_read + 2);
            else {
                int i;
                fprintf (stderr, "usb bulk read %d bytes:", n);
                for (i=0; i<n; i++)
                    fprintf (stderr, "%c%02x", i ? '-' : ' ', reply[i]);
                fprintf (stderr, "\n");
            }
        }
        if (n > 2) {
            /* Copy data. */
            memcpy (a->input + bytes_read, reply + 2, n - 2);
            bytes_read += n - 2;
        }
    }
    if (debug) {
        int i;
        fprintf (stderr, "mpsse_flush_output received %d bytes:", a->bytes_to_read);
        for (i=0; i<a->bytes_to_read; i++)
            fprintf (stderr, "%c%02x", i ? '-' : ' ', a->input[i]);
        fprintf (stderr, "\n");
    }
    a->bytes_to_read = 0;
}

static void mpsse_send (mpsse_adapter_t *a,
    unsigned tms_prolog_nbits, unsigned tms_prolog,
    unsigned tdi_nbits, unsigned long long tdi, int read_flag)
{
    unsigned tms_epilog_nbits = 0, tms_epilog = 0;

    if (tdi_nbits > 0) {
        /* Если есть данные, добавляем:
         * стандартный пролог TMS 1-0-0,
         * стандартный эпилог TMS 1. */
        tms_prolog |= 1 << tms_prolog_nbits;
        tms_prolog_nbits += 3;
        tms_epilog = 1;
        tms_epilog_nbits = 1;
    }
    /* Проверяем, есть ли место в выходном буфере.
     * Максимальный размер одного пакета - 23 байта (6+8+3+3+3). */
    if (a->bytes_to_write > sizeof (a->output) - 23)
        mpsse_flush_output (a);

    /* Формируем пакет команд MPSSE. */
    if (tms_prolog_nbits > 0) {
        /* Пролог TMS, от 1 до 14 бит.
         * 4b - Clock Data to TMS Pin (no Read) */
        a->output [a->bytes_to_write++] = WTMS + BITMODE + CLKWNEG + LSB;
        if (tms_prolog_nbits < 8) {
            a->output [a->bytes_to_write++] = tms_prolog_nbits - 1;
            a->output [a->bytes_to_write++] = tms_prolog;
        } else {
            a->output [a->bytes_to_write++] = 7 - 1;
            a->output [a->bytes_to_write++] = tms_prolog & 0x7f;
            a->output [a->bytes_to_write++] = WTMS + BITMODE + CLKWNEG + LSB;
            a->output [a->bytes_to_write++] = tms_prolog_nbits - 7 - 1;
            a->output [a->bytes_to_write++] = tms_prolog >> 7;
        }
    }
    if (tdi_nbits > 0) {
        /* Данные, от 1 до 64 бит. */
        if (tms_epilog_nbits > 0) {
            /* Последний бит надо сопровождать сигналом TMS=1. */
            tdi_nbits--;
        }
        unsigned nbytes = tdi_nbits / 8;
        unsigned last_byte_bits = tdi_nbits & 7;
        if (read_flag) {
            a->high_byte_bits = last_byte_bits;
            a->fix_high_bit = 0;
            a->high_byte_mask = 0;
            a->bytes_per_word = nbytes;
            if (a->high_byte_bits > 0)
                a->bytes_per_word++;
            a->bytes_to_read += a->bytes_per_word;
        }
        if (nbytes > 0) {
            /* Целые байты.
             * 39 - Clock Data Bytes In and Out LSB First
             * 19 - Clock Data Bytes Out LSB First (no Read) */
            a->output [a->bytes_to_write++] = read_flag ?
                (WTDI + RTDO + CLKWNEG + LSB) :
                (WTDI + CLKWNEG + LSB);
            a->output [a->bytes_to_write++] = nbytes - 1;
            a->output [a->bytes_to_write++] = (nbytes - 1) >> 8;
            while (nbytes-- > 0) {
                a->output [a->bytes_to_write++] = tdi;
                tdi >>= 8;
            }
        }
        if (last_byte_bits) {
            /* Последний нецелый байт.
             * 3b - Clock Data Bits In and Out LSB First
             * 1b - Clock Data Bits Out LSB First (no Read) */
            a->output [a->bytes_to_write++] = read_flag ?
                (WTDI + RTDO + BITMODE + CLKWNEG + LSB) :
                (WTDI + BITMODE + CLKWNEG + LSB);
            a->output [a->bytes_to_write++] = last_byte_bits - 1;
            a->output [a->bytes_to_write++] = tdi;
            tdi >>= last_byte_bits;
            a->high_byte_mask = 0xffULL << (a->bytes_per_word - 1) * 8;
        }
        if (tms_epilog_nbits > 0) {
            /* Последний бит, точнее два.
             * 6b - Clock Data to TMS Pin with Read
             * 4b - Clock Data to TMS Pin (no Read) */
            tdi_nbits++;
            a->output [a->bytes_to_write++] = read_flag ?
                (WTMS + RTDO + BITMODE + CLKWNEG + LSB) :
                (WTMS + BITMODE + CLKWNEG + LSB);
            a->output [a->bytes_to_write++] = 1;
            a->output [a->bytes_to_write++] = tdi << 7 | 1 | tms_epilog << 1;
            tms_epilog_nbits--;
            tms_epilog >>= 1;
            if (read_flag) {
                /* Последний бит придёт в следующем байте.
                 * Вычисляем маску для коррекции. */
                a->fix_high_bit = 0x40ULL << (a->bytes_per_word * 8);
                a->bytes_per_word++;
                a->bytes_to_read++;
            }
        }
        if (read_flag)
            a->high_bit_mask = 1ULL << (tdi_nbits - 1);
    }
    if (tms_epilog_nbits > 0) {
        /* Эпилог TMS, от 1 до 7 бит.
         * 4b - Clock Data to TMS Pin (no Read) */
        a->output [a->bytes_to_write++] = WTMS + BITMODE + CLKWNEG + LSB;
        a->output [a->bytes_to_write++] = tms_epilog_nbits - 1;
        a->output [a->bytes_to_write++] = tms_epilog;
    }
}

static unsigned long long mpsse_fix_data (mpsse_adapter_t *a, unsigned long long word)
{
    unsigned long long fix_high_bit = word & a->fix_high_bit;
    //if (debug) fprintf (stderr, "fix (%08llx) high_bit=%08llx\n", word, a->fix_high_bit);

    if (a->high_byte_bits) {
        /* Корректируем старший байт принятых данных. */
        unsigned long long high_byte = a->high_byte_mask &
            ((word & a->high_byte_mask) >> (8 - a->high_byte_bits));
        word = (word & ~a->high_byte_mask) | high_byte;
        //if (debug) fprintf (stderr, "Corrected byte %08llx -> %08llx\n", a->high_byte_mask, high_byte);
    }
    word &= a->high_bit_mask - 1;
    if (fix_high_bit) {
        /* Корректируем старший бит принятых данных. */
        word |= a->high_bit_mask;
        //if (debug) fprintf (stderr, "Corrected bit %08llx -> %08llx\n", a->high_bit_mask, word >> 9);
    }
    return word;
}

static unsigned long long mpsse_recv (mpsse_adapter_t *a)
{
    unsigned long long word;

    /* Шлём пакет. */
    mpsse_flush_output (a);

    /* Обрабатываем одно слово. */
    memcpy (&word, a->input, sizeof (word));
    return mpsse_fix_data (a, word);
}

static void mpsse_reset (mpsse_adapter_t *a, int trst, int sysrst, int led)
{
    unsigned char output [3];
    unsigned low_output = 0x08; /* TCK idle high */
    unsigned low_direction = 0x1b;
    unsigned high_direction = 0x0f;
    unsigned high_output = 0;

    /* command "set data bits low byte" */
    output [0] = 0x80;
    output [1] = low_output;
    output [2] = low_direction;
    bulk_write (a, output, 3);

    if (! trst)
        high_output |= 1;

    if (sysrst)
        high_output |= 2;

    if (led)
        high_output |= 8;

    /* command "set data bits high byte" */
    output [0] = 0x82;
    output [1] = high_output;
    output [2] = high_direction;

    bulk_write (a, output, 3);
    if (debug)
        fprintf (stderr, "mpsse_reset (trst=%d, sysrst=%d) high_output=0x%2.2x, high_direction: 0x%2.2x\n",
            trst, sysrst, high_output, high_direction);
}

static void mpsse_speed (mpsse_adapter_t *a, int divisor)
{
    unsigned char output [3];

    /* command "set TCK divisor" */
    output [0] = 0x86;
    output [1] = divisor;
    output [2] = divisor >> 8;
    bulk_write (a, output, 3);
}

static void mpsse_close (adapter_t *adapter)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;

    mpsse_flush_output (a);
    mpsse_reset (a, 0, 0, 0);
    usb_release_interface (a->usbdev, 0);
    usb_close (a->usbdev);
    free (a);
}

/*
 * Read the Device Identification code
 */
static unsigned mpsse_get_idcode (adapter_t *adapter)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned idcode;

    /* Reset the JTAG TAP controller: TMS 1-1-1-1-1-0.
     * After reset, the IDCODE register is always selected.
     * Read out 32 bits of data. */
    mpsse_send (a, 6, 31, 32, 0, 1);
    idcode = mpsse_recv (a);
    return idcode;
}

/*
 * Чтение регистра OnCD.
 */
static unsigned mpsse_oncd_read (adapter_t *adapter, int reg, int reglen)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned value;

    mpsse_send (a, 0, 0, 9 + reglen, reg | IRd_READ, 1);
    value = mpsse_recv (a) >> 9;
    if (debug) {
        if (reg == OnCD_OSCR) {
            fprintf (stderr, "OnCD read %04x", value);
            if (value)
                print_oscr_bits (value);
            fprintf (stderr, " from OSCR\n");
        } else
            fprintf (stderr, "OnCD read %08x from %s\n", value, oncd_regname [reg]);
    }
    return value;
}

/*
 * Запись регистра OnCD.
 */
static void mpsse_oncd_write (adapter_t *adapter,
    unsigned value, int reg, int reglen)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned long long data;

    if (debug) {
        if (reg == OnCD_OSCR) {
            fprintf (stderr, "OnCD write %04x", value);
            if (value)
                print_oscr_bits (value);
            fprintf (stderr, " to OSCR\n");
        } else {
            fprintf (stderr, "OnCD write %08x to %s",
                value, oncd_regname [reg & 15]);
            if (reg & IRd_RUN)    fprintf (stderr, "+RUN");
            if (reg & IRd_READ)   fprintf (stderr, "+READ");
            if (reg & IRd_FLUSH_PIPE) fprintf (stderr, "+FLUSH_PIPE");
            if (reg & IRd_STEP_1CLK)  fprintf (stderr, "+STEP_1CLK");
            fprintf (stderr, "\n");
        }
    }
    data = reg;
    if (reglen > 0)
        data |= (unsigned long long) value << 9;
    mpsse_send (a, 0, 0, 9 + reglen, data, 0);
}

/*
 * Перевод кристалла в режим отладки путём манипуляций
 * регистрами данных JTAG.
 */
static void mpsse_stop_cpu (adapter_t *adapter)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned old_ir, i, oscr;

    /* Debug request. */
    mpsse_send (a, 1, 1, 4, TAP_DEBUG_REQUEST, 0);

    /* Wait while processor enters debug mode. */
    i = 0;
    for (;;) {
        mpsse_send (a, 1, 1, 4, TAP_DEBUG_ENABLE, 1);
        old_ir = mpsse_recv (a);
        if (old_ir == 5)
            break;
        mdelay (10);
        if (++i >= 50) {
            fprintf (stderr, "Timeout while entering debug mode\n");
            exit (1);
        }
    }
    mpsse_oncd_write (adapter, 0, OnCD_OBCR, 12);
    oscr = mpsse_oncd_read (adapter, OnCD_OSCR, 32);
    oscr |= OSCR_TME;
    mpsse_oncd_write (adapter, oscr, OnCD_OSCR, 32);
}

static void mpsse_read_block (adapter_t *adapter,
    unsigned nwords, unsigned addr, unsigned *data)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned oscr, i;
    unsigned long long word;

    /* Allow memory access */
    oscr = mpsse_oncd_read (adapter, OnCD_OSCR, 32);
    oscr |= OSCR_SlctMEM | OSCR_RO;
    mpsse_oncd_write (adapter, oscr, OnCD_OSCR, 32);

    while (nwords > 0) {
        unsigned n = nwords;
        if (n > 6)
            n = 6;
        for (i=0; i<n; i++) {
            mpsse_oncd_write (adapter, addr + i*4, OnCD_OMAR, 32);
            mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);
            mpsse_send (a, 0, 0, 9 + 32, OnCD_OMDR | IRd_READ, 1);
        }

        /* Шлём пакет. */
        mpsse_flush_output (a);

        /* Обрабатываем слово за словом, со сдвигом 9 бит. */
        for (i=0; i<n; i++) {
            word = *(unsigned long long*) &a->input [i * a->bytes_per_word];
            data[i] = mpsse_fix_data (a, word) >> 9;
//fprintf (stderr, "addr = %08x, i = %d, bytes_per_word = %d, ", addr + i*4, i, a->bytes_per_word);
//fprintf (stderr, "fixed = %08x\n", data[i]);
        }
        data += n;
        addr += n*4;
        nwords -= n;
    }
}

static void mpsse_write_block (adapter_t *adapter,
    unsigned nwords, unsigned addr, unsigned *data)
{
    /* Allow memory access */
    unsigned oscr = mpsse_oncd_read (adapter, OnCD_OSCR, 32);
    oscr |= OSCR_SlctMEM;
    oscr &= ~OSCR_RO;
    mpsse_oncd_write (adapter, oscr, OnCD_OSCR, 32);

    while (nwords-- > 0) {
        mpsse_oncd_write (adapter, addr, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, *data, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);
        addr += 4;
        data++;
    }
}

static void mpsse_write_nwords (adapter_t *adapter, unsigned nwords, va_list args)
{
    /* Allow memory access */
    unsigned oscr = mpsse_oncd_read (adapter, OnCD_OSCR, 32);
    oscr |= OSCR_SlctMEM;
    oscr &= ~OSCR_RO;
    mpsse_oncd_write (adapter, oscr, OnCD_OSCR, 32);

    while (nwords-- > 0) {
        unsigned addr = va_arg (args, unsigned);
        unsigned data = va_arg (args, unsigned);
        mpsse_oncd_write (adapter, addr, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, data, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);
    }
}

static void mpsse_program_block32 (adapter_t *adapter,
    unsigned nwords, unsigned base, unsigned addr, unsigned *data,
    unsigned addr_odd, unsigned addr_even,
    unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0)
{
    /* Allow memory access */
    unsigned oscr = mpsse_oncd_read (adapter, OnCD_OSCR, 32);
    oscr |= OSCR_SlctMEM;
    oscr &= ~OSCR_RO;
    mpsse_oncd_write (adapter, oscr, OnCD_OSCR, 32);

    while (nwords-- > 0) {
        mpsse_oncd_write (adapter, base + addr_odd, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, cmd_aa, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);

        mpsse_oncd_write (adapter, base + addr_even, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, cmd_55, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);

        mpsse_oncd_write (adapter, base + addr_odd, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, cmd_a0, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);

        mpsse_oncd_write (adapter, addr, OnCD_OMAR, 32);
        mpsse_oncd_write (adapter, *data, OnCD_OMDR, 32);
        mpsse_oncd_write (adapter, 0, OnCD_MEM, 0);

        addr += 4;
        data++;
    }
}

/*
 * Инициализация адаптера F2232.
 * Возвращаем указатель на структуру данных, выделяемую динамически.
 * Если адаптер не обнаружен, возвращаем 0.
 */
adapter_t *adapter_open_mpsse (void)
{
    mpsse_adapter_t *a;
    struct usb_bus *bus;
    struct usb_device *dev;

    usb_init();
    usb_find_busses();
    usb_find_devices();
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor == OLIMEX_VID &&
                dev->descriptor.idProduct == OLIMEX_PID)
                goto found;
        }
    }
    /*fprintf (stderr, "USB adapter not found: vid=%04x, pid=%04x\n",
        OLIMEX_VID, OLIMEX_PID);*/
    return 0;
found:
    /*fprintf (stderr, "found USB adapter: vid %04x, pid %04x, type %03x\n",
        dev->descriptor.idVendor, dev->descriptor.idProduct,
        dev->descriptor.bcdDevice);*/
    a = calloc (1, sizeof (*a));
    if (! a) {
        fprintf (stderr, "Out of memory\n");
        return 0;
    }
    a->usbdev = usb_open (dev);
    if (! a->usbdev) {
        fprintf (stderr, "MPSSE: usb_open() failed\n");
        free (a);
        return 0;
    }
    usb_claim_interface (a->usbdev, 0);

    /* Reset the ftdi device. */
    if (usb_control_msg (a->usbdev,
        USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
        SIO_RESET, 0, 1, 0, 0, 1000) != 0) {
        fprintf (stderr, "FTDI reset failed\n");
failed:     usb_release_interface (a->usbdev, 0);
        usb_close (a->usbdev);
        free (a);
        return 0;
    }

    /* MPSSE mode. */
    if (usb_control_msg (a->usbdev,
        USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
        SIO_SET_BITMODE, 0x20b, 1, 0, 0, 1000) != 0) {
        fprintf (stderr, "Can't set sync mpsse mode\n");
        goto failed;
    }

    /* Ровно 500 нсек между выдачами. */
    unsigned divisor = 5;
    unsigned char latency_timer = 1;

    if (usb_control_msg (a->usbdev,
        USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
        SIO_SET_LATENCY_TIMER, latency_timer, 1, 0, 0, 1000) != 0) {
        fprintf (stderr, "unable to set latency timer\n");
        goto failed;
    }
    if (usb_control_msg (a->usbdev,
        USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN,
        SIO_GET_LATENCY_TIMER, 0, 1, (char*) &latency_timer, 1, 1000) != 1) {
        fprintf (stderr, "unable to get latency timer\n");
        goto failed;
    }
    if (debug)
        fprintf (stderr, "MPSSE: latency timer: %u usec\n", latency_timer);

    mpsse_reset (a, 0, 0, 1);

    int baud = 6000000 / (divisor + 1);
    if (debug)
        fprintf (stderr, "MPSSE: speed %d samples/sec\n", baud);
    mpsse_speed (a, divisor);

    /* Disable TDI to TDO loopback. */
    unsigned char enable_loopback[] = "\x85";
    bulk_write (a, enable_loopback, 1);

    /* Reset the JTAG TAP controller. */
    mpsse_send (a, 6, 31, 0, 0, 0);         /* TMS 1-1-1-1-1-0 */

    /* Обязательные функции. */
    a->adapter.name = "FT2232";
    a->adapter.close = mpsse_close;
    a->adapter.get_idcode = mpsse_get_idcode;
    a->adapter.stop_cpu = mpsse_stop_cpu;
    a->adapter.oncd_read = mpsse_oncd_read;
    a->adapter.oncd_write = mpsse_oncd_write;

    /* Расширенные возможности. */
    a->adapter.block_words = 999999;
    a->adapter.program_block_words = 999999;
    a->adapter.read_block = mpsse_read_block;
    a->adapter.write_block = mpsse_write_block;
    a->adapter.write_nwords = mpsse_write_nwords;
    a->adapter.program_block32 = mpsse_program_block32;
    return &a->adapter;
}

#ifdef STANDALONE
static int mpsse_test (adapter_t *adapter, int iterations)
{
    mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
    unsigned result, pattern = 0;

    /* Enter BYPASS mode. */
    mpsse_send (a, 1, 1, 4, TAP_BYPASS, 0);
    do {
        /* Simple pseudo-random generator. */
        pattern = 1664525ul * pattern + 1013904223ul;

        /* Pass the pattern through TDI-TDO. */
        mpsse_send (a, 0, 0, 33, pattern, 1);
        result = mpsse_recv (a);
        fprintf (stderr, "sent %08x received %08x\n", pattern, result);
        if (result != pattern) {
            /* Reset the JTAG TAP controller:
             * TMS 1-1-1-1-1-0. */
            mpsse_send (a, 6, 31, 0, 0, 0);
            return 0;
        }
    } while (--iterations > 0);

    mpsse_send (a, 3, 3, 0, 0, 0);          /* TMS 1-1-0 */
    return 1;
}

int debug;

#if defined (__CYGWIN32__) || defined (MINGW32)
#include <windows.h>
void mdelay (unsigned msec)
{
    Sleep (msec);
}
#else
void mdelay (unsigned msec)
{
    usleep (msec * 1000);
}
#endif

int main (int argc, char **argv)
{
    adapter_t *adapter;
    int ch;

    setvbuf (stdout, (char *)NULL, _IOLBF, 0);
    setvbuf (stderr, (char *)NULL, _IOLBF, 0);

    while ((ch = getopt(argc, argv, "vh")) != -1) {
        switch (ch) {
        case 'v':
            ++debug;
            continue;
        case 'h':
            break;
        }
usage:  printf ("Test for FT232R JTAG adapter.\n");
        printf ("Usage:\n");
        printf ("       jtag-mpsse [-v] command\n");
        printf ("\nOptions:\n");
        printf ("       -v         Verbose mode\n");
        printf ("\nCommands:\n");
        printf ("       test       Test TAP controller in BYPASS mode\n");
        printf ("       idcode     Read IDCODE register\n");
        exit (0);
    }
    argc -= optind;
    argv += optind;
    switch (argc) {
    default:
        goto usage;
    case 1:
        adapter = adapter_open_mpsse ();
        if (! adapter) {
            fprintf (stderr, "No mpsse adapter found.\n");
            exit (1);
        }
        if (strcmp ("test", argv[0]) == 0) {
            if (mpsse_test (adapter, 20))
                printf ("TAP tested OK.\n");
            else
                printf ("TAP test FAILED!\n");

        } else if (strcmp ("idcode", argv[0]) == 0) {
            int i;
            for (i=0; i<10; ++i)
                printf ("IDCODE = %08x\n", mpsse_get_idcode (adapter));
        } else {
            mpsse_close (adapter);
            goto usage;
        }

        mpsse_close (adapter);
        break;
    }
    return 0;
}
#endif /* STANDALONE */
