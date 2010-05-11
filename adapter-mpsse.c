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

	/* Индексы и буферы для вывода. */
	int output_len;
	unsigned tms_prolog;
	unsigned tms_prolog_nbits;
	unsigned tms_epilog;
	unsigned tms_epilog_nbits;
	unsigned long long tdi;
	unsigned tdi_nbits;

	/* Индексы для ввода. */
	int tdo_start;
	int tdo_end;
} mpsse_adapter_t;

/*
 * Можно использовать готовый адаптер Olimex ARM-USB-Tiny с переходником
 * с разъёма ARM 2x10 на разъём MIPS 2x5:
 *
 * Сигнал   Контакт ARM	   Контакт MIPS
 * ------------------------------------
 * /TRST	3		3
 *  TDI		5 		7
 *  TMS		7 		5
 *  TCK		9 		1
 *  TDO		13		9
 * /SYSRST	15		6
 *  GND		4,6,8,10,12,	2,8
 *		14,16,18,20
 */

/*
 * Identifiers of USB adapter.
 */
#define OLIMEX_VID		0x15ba
#define OLIMEX_PID		0x0004	/* ARM-USB-Tiny */

/*
 * USB endpoints.
 */
#define IN_EP			0x02
#define OUT_EP			0x81

/* Requests */
#define SIO_RESET		0 /* Reset the port */
#define SIO_MODEM_CTRL		1 /* Set the modem control register */
#define SIO_SET_FLOW_CTRL	2 /* Set flow control register */
#define SIO_SET_BAUD_RATE	3 /* Set baud rate */
#define SIO_SET_DATA		4 /* Set the data characteristics of the port */
#define SIO_POLL_MODEM_STATUS	5
#define SIO_SET_EVENT_CHAR	6
#define SIO_SET_ERROR_CHAR	7
#define SIO_SET_LATENCY_TIMER	9
#define SIO_GET_LATENCY_TIMER	10
#define SIO_SET_BITMODE		11
#define SIO_READ_PINS		12
#define SIO_READ_EEPROM		0x90
#define SIO_WRITE_EEPROM	0x91
#define SIO_ERASE_EEPROM	0x92

/* Биты регистра IRd */
#define	IRd_RUN		0x20	/* 0 - step mode, 1 - run continuosly */
#define	IRd_READ	0x40	/* 0 - write, 1 - read registers */
#define	IRd_FLUSH_PIPE	0x40	/* for EnGO: instruction pipe changed */
#define	IRd_STEP_1CLK	0x80	/* for step mode: run for 1 clock only */

static char *oncd_regname[] = {
	"OSCR",		"OMBC",		"OMLR0",	"OMLR1",
	"OBCR",		"IRdec",	"OTC",		"PCdec",
	"PCexec",	"PCmem",	"PCfetch",	"OMAR",
	"OMDR",		"MEM",		"PCwb",		"EXIT",
};

static char *oscr_bitname[] = {
	"SlctMEM",	"RO",		"TME",		"IME",
	"MPE",		"RDYm",		"MBO",		"TO",
	"SWO",		"SO",		"DBM",		"NDS",
	"VBO",		"NFEXP",	"WP0",		"WP1",
};

/*
 * Add one TMS/TDI sample to send buffer.
 */
static void mpsse_write (mpsse_adapter_t *a, int tms, int tdi)
{
	if (a->output_len == 0) {
		/* Начало JTAG-транзакции. */
		a->tms_prolog = 0;
		a->tms_prolog_nbits = 0;
		a->tms_epilog = 0;
		a->tms_epilog_nbits = 0;
		a->tdi = 0;
		a->tdi_nbits = 0;
	}

	if (a->tdo_start < 0) {
		/* Пролог TMS. */
		if (a->tms_prolog_nbits >= 14) {
			fprintf (stderr, "mpsse_write: prolog overflow\n");
			exit (-1);
		}
		if (tms)
			a->tms_prolog |= 1 << a->tms_prolog_nbits;
		a->tms_prolog_nbits++;

	} else if (a->tdo_end < 0) {
		/* Данные. */
		if (a->tdi_nbits >= 8*sizeof(a->tdi)) {
			fprintf (stderr, "mpsse_write: data overflow\n");
			exit (-1);
		}
		if (tdi)
			a->tdi |= 1ULL << a->tdi_nbits;
		a->tdi_nbits++;
	} else {
		/* Эпилог TMS. */
		if (a->tms_epilog_nbits >= 7) {
			fprintf (stderr, "mpsse_write: epilog overflow\n");
			exit (-1);
		}
		if (tms)
			a->tms_epilog |= 1 << a->tms_epilog_nbits;
		a->tms_epilog_nbits++;
	}

	/* Общее количество битов в выходном потоке. */
	a->output_len++;
}

/*
 * Add a TMS step (2 samples) to send buffer.
 */
static void mpsse_step (mpsse_adapter_t *a, int tms)
{
	mpsse_write (a, tms, 1);
}

static void bulk_write (mpsse_adapter_t *a, unsigned char *output, int bytes_to_write)
{
	int bytes_written;

	if (debug)
		fprintf (stderr, "usb bulk write %d bytes\n", bytes_to_write);
	bytes_written = usb_bulk_write (a->usbdev, IN_EP, (char*) output,
		bytes_to_write, 1000);
	if (bytes_written < 0) {
		fprintf (stderr, "usb bulk write failed\n");
		exit (-1);
	}
	if (bytes_written != bytes_to_write)
		fprintf (stderr, "usb bulk written %d bytes of %d",
			bytes_written, bytes_to_write);

}

/*
 * Perform MPSSE output/input transaction.
 * On return, received data are put to array reply[].
 */
static void mpsse_send_recv (mpsse_adapter_t *a, unsigned char *read_data)
{
	int bytes_to_write, bytes_to_read, bytes_read, n;
	unsigned char output [64];
	unsigned char reply [64];

	/* Формируем пакет команд MPSSE. */
	bytes_to_write = 0;
	bytes_to_read = 0;
	if (a->tms_prolog_nbits > 0) {
		/* Пролог TMS, от 1 до 14 бит. */
		output [bytes_to_write++] = 0x4b;
		if (a->tms_prolog_nbits < 8) {
			output [bytes_to_write++] = a->tms_prolog_nbits - 1;
			output [bytes_to_write++] = a->tms_prolog | 0x80;
		} else {
			output [bytes_to_write++] = 6;
			output [bytes_to_write++] = a->tms_prolog | 0x80;
			output [bytes_to_write++] = 0x4b;
			output [bytes_to_write++] = a->tms_prolog_nbits - 7;
			output [bytes_to_write++] = (a->tms_prolog >> 7) | 0x80;
		}
	}
	if (a->tdi_nbits > 0) {
		/* Данные, от 1 до 64 бит. */
		unsigned nbytes = a->tdi_nbits / 8;
		unsigned nbits = a->tdi_nbits - nbytes*8;
		if (nbytes > 0) {
			/* Целые байты. */
			if (read_data) {
				output [bytes_to_write++] = 0x39;
				bytes_to_read += nbytes;
			} else
				output [bytes_to_write++] = 0x19;
			output [bytes_to_write++] = nbytes - 1;
			output [bytes_to_write++] = (nbytes - 1) >> 8;
			while (nbytes-- > 0) {
				output [bytes_to_write++] = a->tdi;
				a->tdi >>= 8;
			}
		}
		if (nbits > 0) {
			/* Последний нецелый байт. */
			if (read_data) {
				output [bytes_to_write++] = 0x3B;
				bytes_to_read++;
			} else
				output [bytes_to_write++] = 0x1B;
			output [bytes_to_write++] = nbits - 1;
			output [bytes_to_write++] = a->tdi;
		}
	}
	if (a->tms_epilog_nbits > 0) {
		/* Эпилог TMS, от 1 до 7 бит. */
		output [bytes_to_write++] = 0x4b;
		output [bytes_to_write++] = a->tms_epilog_nbits - 1;
		output [bytes_to_write++] = a->tms_epilog | 0x80;
	}

	/* Шлём пакет. */
	bulk_write (a, output, bytes_to_write);

	/* Получаем ответ. */
	bytes_read = 0;
	while (bytes_read < bytes_to_read) {
		n = usb_bulk_read (a->usbdev, OUT_EP, (char*) reply,
			bytes_to_read - bytes_read + 2, 2000);
		if (n < 0) {
			fprintf (stderr, "usb bulk read failed\n");
			exit (-1);
		}
		if (n != bytes_to_read + 2)
			fprintf (stderr, "usb bulk read %d bytes of %d\n",
				n, bytes_to_read - bytes_read + 2);
		else if (debug)
			fprintf (stderr, "usb bulk read %d bytes\n", n);

		if (n > 2) {
			/* Copy data. */
			memcpy (read_data + bytes_read, reply + 2, n - 2);
			bytes_read += n - 2;
		}
	}
	a->output_len = 0;
	a->tdo_start = -1;
	a->tdo_end = -1;
}

static void mpsse_reset (mpsse_adapter_t *a, int trst, int sysrst)
{
	unsigned char output [64];
	unsigned high_direction = 0x0f;
	unsigned high_output = 0;

	if (! trst)
		high_output |= 1;

	if (sysrst)
		high_output |= 2;

	/* command "set data bits high byte" */
	output [0] = 0x82;
	output [1] = high_output;
	output [2] = high_direction;

	bulk_write (a, output, 3);
	fprintf (stderr, "mpsse_reset (trst=%d, sysrst=%d) high_output=0x%2.2x, high_direction: 0x%2.2x\n",
		trst, sysrst, high_output, high_direction);
}

static void mpsse_close (adapter_t *adapter)
{
	mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;

	usb_release_interface (a->usbdev, 0);
	usb_close (a->usbdev);
	free (a);
}

/*
 * Enter a new instr. in TAP machine.
 * Assumes TAP is on Run-Test/Idle or Update-DR/Update-IR states at entry.
 * On exit, stays at Update-IR.
 * Returns nonzero, if a processor is in debug mode.
 */
static int tap_instr (mpsse_adapter_t *a, int nbits, unsigned newinst)
{
	unsigned status = 0, mask, tms, tdi, n;

	mpsse_step (a, 1);		/* goto Select-DR-Scan */
	mpsse_step (a, 1);		/* goto Select-IR-Scan */
	mpsse_step (a, 0);		/* goto Capture-IR */
	mpsse_step (a, 0);		/* goto Shift-IR */
	a->tdo_start = a->output_len;
	mask = 1;
	for (n=0; n<nbits; n++) {
		/* If last bit, put TMS=1 to exit Shift-IR state */
		tms = (n == nbits-1);
		tdi = newinst & mask;
		mask <<= 1;
		mpsse_write (a, tms, tdi);
	}
	a->tdo_end = a->output_len;
	mpsse_step (a, 1);		/* goto Update-IR */
	mpsse_send_recv (a, (unsigned char*) &status);
	return status;
}

/*
 * Enter a bit stream in the selected DR.
 *
 * Assumes TAP is on Run-Test/Idle or Update-DR/Update-IR states at entry.
 * On exit, stays at Update-DR/Update-IR.
 * Newdata is an array of bytes with the bit stream to send,
 * LSB first. The total number of bits to send is nbits.
 * The readout bit stream is stored in olddata[].
 * If newdata is NULL, zeros are sent in the data stream.
 * If olddata is NULL, readout bits are thrown away.
 */
static void tap_data (mpsse_adapter_t *a,
	unsigned nbits, unsigned char *newdata, unsigned char *olddata)
{
	unsigned databits = 0, tms, tdi, n;
	unsigned char mask = 0;

	mpsse_step (a, 1);			/* goto Select-DR-Scan */
	mpsse_step (a, 0);			/* goto Capture-DR */
	mpsse_step (a, 0);			/* goto Shift-DR */
	a->tdo_start = a->output_len;
	for (n=0; n<nbits; n++) {
		if (mask == 0) {		/* every 8 bits sent... */
			if (newdata)		/* ...get a new word */
				databits = *newdata++;
			mask = 1;
		}
		/* If last bit, put TMS=1 to exit Shift-DR state */
		tms = (n == nbits-1);
		tdi = databits & mask;
		mask <<= 1;
		mpsse_write (a, tms, tdi);
	}
	a->tdo_end = a->output_len;
	mpsse_step (a, 1);			/* goto Update-DR */
	mpsse_send_recv (a, olddata);
}

/*
 * Read the Device Identification code
 */
static unsigned mpsse_get_idcode (adapter_t *adapter)
{
	mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
	unsigned idcode = 0;

	/* Reset the JTAG TAP controller. */
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);		/* 5 cycles with TMS=1 */
	mpsse_step (a, 1);
	mpsse_step (a, 0);		/* TMS=0 to enter run-test/idle */

	tap_instr (a, 4, TAP_IDCODE);	/* Select IDCODE register */
	tap_data (a, 32, 0,		/* Read out 32 bits into idcode */
		(unsigned char*) &idcode);
	return idcode;
}

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
 * Чтение регистра OnCD.
 */
static unsigned mpsse_oncd_read (adapter_t *adapter, int reg, int reglen)
{
	mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
	unsigned long long data;
	unsigned value;

	data = reg | IRd_READ;
	tap_data (a, 9 + reglen,
		(unsigned char*) &data, (unsigned char*) &data);
	value = data >> 9;
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
			if (reg & IRd_RUN)        fprintf (stderr, "+RUN");
			if (reg & IRd_READ)       fprintf (stderr, "+READ");
			if (reg & IRd_FLUSH_PIPE) fprintf (stderr, "+FLUSH_PIPE");
			if (reg & IRd_STEP_1CLK)  fprintf (stderr, "+STEP_1CLK");
			fprintf (stderr, "\n");
		}
	}
	data = reg;
	if (reglen > 0)
		data |= (unsigned long long) value << 9;
	tap_data (a, 9 + reglen, (unsigned char*) &data, 0);
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
	tap_instr (a, 4, TAP_DEBUG_REQUEST);

	/* Wait while processor enters debug mode. */
	i = 0;
	for (;;) {
		old_ir = tap_instr (a, 4, TAP_DEBUG_ENABLE);
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
	fprintf (stderr, "found USB adapter: vid %04x, pid %04x, type %03x\n",
		dev->descriptor.idVendor, dev->descriptor.idProduct,
		dev->descriptor.bcdDevice);
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
failed:		usb_release_interface (a->usbdev, 0);
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
	unsigned divisor = 0;
	unsigned char latency_timer = 5;

	int baud = 3000000 / (divisor + 1);
	if (debug)
		fprintf (stderr, "MPSSE: speed %d samples/sec\n", baud);

	/* Frequency divisor is 14-bit non-zero value. */
	if (usb_control_msg (a->usbdev,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_BAUD_RATE, divisor,
	    1, 0, 0, 1000) != 0) {
		fprintf (stderr, "Can't set baud rate\n");
		goto failed;
	}

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

	mpsse_reset (a, 0, 0);

	/* Reset the JTAG TAP controller. */
	a->tdo_start = -1;
	a->tdo_end = -1;
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);	/* 5 cycles with TMS=1 */
	mpsse_step (a, 1);
	mpsse_step (a, 0);	/* TMS=0 to enter run-test/idle */
	mpsse_send_recv (a, 0);

	/* Обязательные функции. */
	a->adapter.name = "FT2232";
	a->adapter.close = mpsse_close;
	a->adapter.get_idcode = mpsse_get_idcode;
	a->adapter.stop_cpu = mpsse_stop_cpu;
	a->adapter.oncd_read = mpsse_oncd_read;
	a->adapter.oncd_write = mpsse_oncd_write;
	return &a->adapter;
}

#ifdef STANDALONE
static int mpsse_test (adapter_t *adapter, int iterations)
{
	mpsse_adapter_t *a = (mpsse_adapter_t*) adapter;
	unsigned result, mask, tdi, last_bit = 0, pattern = 0;

	tap_instr (a, 4, TAP_BYPASS);	/* Enter BYPASS mode. */
	mpsse_step (a, 1);		/* goto Select-DR-Scan */
	mpsse_step (a, 0);		/* goto Capture-DR */
	mpsse_step (a, 0);		/* goto Shift-DR */
	do {
		pattern = 1664525ul * pattern + 1013904223ul; /* simple PRNG */

		/* Pass the pattern through TDI-TDO. */
		a->tdo_start = a->output_len;
		for (mask=1; mask; mask<<=1) {
			tdi = pattern & mask;
			mpsse_write (a, 0, tdi);
		}
		a->tdo_end = a->output_len;
		mpsse_send_recv (a, (unsigned char*) &result);

		fprintf (stderr, "sent %08x received %08x\n",
			pattern<<1 | last_bit, result);
		if ((result & 1) != last_bit ||
		    (result >> 1) != (pattern & 0x7FFFFFFFul)) {
			/* Reset the JTAG TAP controller. */
			mpsse_step (a, 1);
			mpsse_step (a, 1);
			mpsse_step (a, 1);
			mpsse_step (a, 1); /* 5 cycles with TMS=1 */
			mpsse_step (a, 1);
			mpsse_step (a, 0); /* TMS=0 to enter run-test/idle */
			mpsse_send_recv (a, 0);
			return 0;
		}
		last_bit = pattern >> 31;
	} while (--iterations > 0);

	mpsse_step (a, 1);		/* goto Exit1-DR */
	mpsse_step (a, 1);		/* goto Update-DR */
	mpsse_step (a, 0);		/* Run-Test/idle => exec instr. */
	mpsse_send_recv (a, 0);
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
usage:		printf ("Test for FT232R JTAG adapter.\n");
		printf ("Usage:\n");
		printf ("        jtag-mpsse [-v] command\n");
		printf ("\nOptions:\n");
		printf ("        -v         Verbose mode\n");
		printf ("\nCommands:\n");
		printf ("        test       Test TAP controller in BYPASS mode\n");
		printf ("        idcode     Read IDCODE register\n");
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
			for (i=0; i<20; ++i)
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
