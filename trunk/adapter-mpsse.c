/*
 * Интерфейс через адаптер FT2232 к процессору Элвис Мультикор.
 * Автор: С.Вакуленко.
 *
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

	/* Буфер для вывода-ввода в режиме MPSSE. */
	unsigned char output [128];
	int output_len;
} mpsse_adapter_t;

/*
 * We use a layout of original usbjtag adapter, designed by Hubert Hoegl:
 * http://www.hs-augsburg.de/~hhoegl/proj/usbjtag/usbjtag.html
 *
 *   Bit 7 (0x80): unused.
 *   Bit 6 (0x40): /SYSRST output.
 *   Bit 5 (0x20): unused.
 *   Bit 4 (0x10): /TRST output.
 *   Bit 3 (0x08): TMS output.
 *   Bit 2 (0x04): TDO input.
 *   Bit 1 (0x02): TDI output.
 *   Bit 0 (0x01): TCK output.
 *
 * Sync bit bang mode is implemented, as described in FTDI Application
 * Note AN232R-01: "Bit Bang Modes for the FT232R and FT245R".
 */
#define TCK			(1 << 0)
#define TDI			(1 << 1)
#define READ_TDO		(1 << 2)
#define TMS			(1 << 3)
#define NTRST			(1 << 4)
#define NSYSRST			(1 << 6)

/*
 * Identifiers of USB adapter.
 */
#define OLIMEX_VID		0x0403
#define OLIMEX_PID		0x7777	/* ARM-USB-Tiny */

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
 * Add one TCK/TMS/TDI sample to send buffer.
 */
static void mpsse_write (mpsse_adapter_t *a, int tck, int tms, int tdi)
{
	unsigned out_value = NTRST | NSYSRST;
	if (tck)
		out_value |= TCK;
	if (tms)
		out_value |= TMS;
	if (tdi)
		out_value |= TDI;

	if (a->output_len >= (int) sizeof (a->output)) {
		fprintf (stderr, "mpsse_write: buffer overflow\n");
		exit (-1);
	}
	a->output [a->output_len++] = out_value;
}

/*
 * Extract input data from mpsse buffer.
 */
static void mpsse_read (mpsse_adapter_t *a,
	unsigned offset, unsigned nbits, unsigned char *data)
{
	unsigned n;

	for (n=0; n<nbits; n++) {
		if (a->output [offset + n*2 + 1] & READ_TDO)
			data [n/8] |= 1 << (n & 7);
		else
			data [n/8] &= ~(1 << (n & 7));
	}
}

/*
 * Add a TMS step (2 samples) to send buffer.
 */
static void mpsse_step (mpsse_adapter_t *a, int tms)
{
	mpsse_write (a, 0, tms, 1);
	mpsse_write (a, 1, tms, 1);
}

/*
 * Perform sync mpsse output/input transaction.
 * Befor call, an array a->output[] should be filled with data to send.
 * Counter a->output_len contains a number of bytes to send.
 * On return, received data are put back to array a->output[].
 */
static void mpsse_send_recv (mpsse_adapter_t *a)
{
	int bytes_to_write, bytes_written, n, txdone, rxdone;
	int empty_rxfifo, bytes_to_read, bytes_read;
	unsigned char reply [64];

	mpsse_write (a, 0, 1, 1);

	/* First two receive bytes contain modem and line status. */
	empty_rxfifo = sizeof(reply) - 2;

	/* Indexes in data buffer. */
	txdone = 0;
	rxdone = 0;
	while (rxdone < a->output_len) {
		/* Try to send as much as possible,
		 * but avoid overflow of receive buffer.
		 * Unfortunately, transfer sizes bigger that
		 * 64 bytes cause hang ups. */
		bytes_to_write = 64;
		if (bytes_to_write > a->output_len - txdone)
			bytes_to_write = a->output_len - txdone;
		if (bytes_to_write > empty_rxfifo)
			bytes_to_write = empty_rxfifo;

		/* Write data. */
		bytes_written = 0;
		while (bytes_written < bytes_to_write) {
			if (debug)
				fprintf (stderr, "usb bulk write %d bytes\n",
					bytes_to_write - bytes_written);
			n = usb_bulk_write (a->usbdev, IN_EP,
				(char*) a->output + txdone + bytes_written,
				bytes_to_write - bytes_written, 1000);
			if (n < 0) {
				fprintf (stderr, "usb bulk write failed\n");
				exit (-1);
			}
			/*if (n != bytes_to_write)
				fprintf (stderr, "usb bulk written %d bytes of %d",
					n, bytes_to_write - bytes_written);*/
			bytes_written += n;
		}
		txdone += bytes_written;
		empty_rxfifo -= bytes_written;

		if (empty_rxfifo == 0 || txdone == a->output_len) {
			/* Get reply. */
			bytes_to_read = sizeof(reply) - empty_rxfifo - 2;
			bytes_read = 0;
			while (bytes_read < bytes_to_read) {
				n = usb_bulk_read (a->usbdev, OUT_EP,
					(char*) reply,
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
					memcpy (a->output + rxdone, reply + 2, n - 2);
					bytes_read += n;
					rxdone += n - 2;
				}
			}
			empty_rxfifo = sizeof(reply) - 2;
		}
	}
	a->output_len = 0;
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
	unsigned status = 0, mask, input_offset, tms, tdi, n;

	mpsse_step (a, 1);		/* goto Select-DR-Scan */
	mpsse_step (a, 1);		/* goto Select-IR-Scan */
	mpsse_step (a, 0);		/* goto Capture-IR */
	mpsse_step (a, 0);		/* goto Shift-IR */
	input_offset = a->output_len;
	mask = 1;
	for (n=0; n<nbits; n++) {
		/* If last bit, put TMS=1 to exit Shift-IR state */
		tms = (n == nbits-1);
		tdi = newinst & mask;
		mask <<= 1;
		mpsse_write (a, 0, tms, tdi);
		mpsse_write (a, 1, tms, tdi);
	}
	mpsse_step (a, 1);		/* goto Update-IR */
	mpsse_send_recv (a);
	mpsse_read (a, input_offset, nbits, (unsigned char*) &status);
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
	unsigned databits = 0, input_offset, tms, tdi, n;
	unsigned char mask = 0;

	mpsse_step (a, 1);			/* goto Select-DR-Scan */
	mpsse_step (a, 0);			/* goto Capture-DR */
	mpsse_step (a, 0);			/* goto Shift-DR */
	input_offset = a->output_len;
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
		mpsse_write (a, 0, tms, tdi);
		mpsse_write (a, 1, tms, tdi);
	}
	mpsse_step (a, 1);			/* goto Update-DR */
	mpsse_send_recv (a);
	if (olddata)
		mpsse_read (a, input_offset, nbits, olddata);
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
	    SIO_RESET, 0, 0, 0, 0, 1000) != 0) {
		fprintf (stderr, "FTDI reset failed\n");
failed:		usb_release_interface (a->usbdev, 0);
		usb_close (a->usbdev);
		free (a);
		return 0;
	}

	/* Sync bit bang mode. */
	if (usb_control_msg (a->usbdev,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_BITMODE, TCK | TDI | TMS | NTRST | NSYSRST | 0x400,
	    0, 0, 0, 1000) != 0) {
		fprintf (stderr, "Can't set sync mpsse mode\n");
		goto failed;
	}

	/* Ровно 500 нсек между выдачами. */
	unsigned divisor = 0;
	unsigned char latency_timer = 1;

	int baud = (divisor == 0) ? 3000000 :
		(divisor == 1) ? 2000000 : 3000000 / divisor;
	if (debug)
		fprintf (stderr, "MPSSE: speed %d samples/sec\n", baud);

	/* Frequency divisor is 14-bit non-zero value. */
	if (usb_control_msg (a->usbdev,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_BAUD_RATE, divisor,
	    0, 0, 0, 1000) != 0) {
		fprintf (stderr, "Can't set baud rate\n");
		goto failed;
	}

	if (usb_control_msg (a->usbdev,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_LATENCY_TIMER, latency_timer, 0, 0, 0, 1000) != 0) {
		fprintf (stderr, "unable to set latency timer\n");
		goto failed;
	}
	if (usb_control_msg (a->usbdev,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN,
	    SIO_GET_LATENCY_TIMER, 0, 0, (char*) &latency_timer, 1, 1000) != 1) {
		fprintf (stderr, "unable to get latency timer\n");
		goto failed;
	}
	if (debug)
		fprintf (stderr, "MPSSE: latency timer: %u usec\n", latency_timer);

	/* Reset the JTAG TAP controller. */
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);
	mpsse_step (a, 1);	/* 5 cycles with TMS=1 */
	mpsse_step (a, 1);
	mpsse_step (a, 0);	/* TMS=0 to enter run-test/idle */
	mpsse_send_recv (a);

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
	unsigned result, mask, tdi, last_bit = 0;
	unsigned input_offset, pattern = 0;

	tap_instr (a, 4, TAP_BYPASS);	/* Enter BYPASS mode. */
	mpsse_step (a, 1);		/* goto Select-DR-Scan */
	mpsse_step (a, 0);		/* goto Capture-DR */
	mpsse_step (a, 0);		/* goto Shift-DR */
	do {
		pattern = 1664525ul * pattern + 1013904223ul; /* simple PRNG */

		/* Pass the pattern through TDI-TDO. */
		input_offset = a->output_len;
		for (mask=1; mask; mask<<=1) {
			tdi = pattern & mask;
			mpsse_write (a, 0, 0, tdi);
			mpsse_write (a, 1, 0, tdi);
		}
		mpsse_send_recv (a);
		mpsse_read (a, input_offset, 32, (unsigned char*) &result);

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
			mpsse_send_recv (a);
			return 0;
		}
		last_bit = pattern >> 31;
	} while (--iterations > 0);

	mpsse_step (a, 1);		/* goto Exit1-DR */
	mpsse_step (a, 1);		/* goto Update-DR */
	mpsse_step (a, 0);		/* Run-Test/idle => exec instr. */
	mpsse_send_recv (a);
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
