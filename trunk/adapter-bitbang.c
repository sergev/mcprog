/*
 * Интерфейс через адаптер FT232R к процессору Элвис Мультикор.
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
} bitbang_adapter_t;

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
#define BITBANG_VID		0x0403
#define BITBANG_PID		0x6001

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
 * JTAG i/o.
 */
static int bitbang_io (bitbang_adapter_t *a, int tms, int tdi)
{
	unsigned char data [3], reply [5];
	int bytes_written, bytes_read, n;

	data[0] = NTRST | NSYSRST;
	if (tms)
		data[0] |= TMS;
	if (tdi)
		data[0] |= TDI;
	data[1] = data[0] | TCK;
	data[2] = data[0];

	/* Write data. */
	bytes_written = 0;
write_more:
	n = usb_bulk_write (a->usbdev, IN_EP, (char*) data + bytes_written,
		sizeof(data) - bytes_written, 1000);
        if (n < 0) {
		fprintf (stderr, "bitbang_io(): usb bulk write failed\n");
		exit (1);
	}
	/*if (debug)
		fprintf (stderr, "bitbang_io() write %u bytes\n", n);*/
	bytes_written += n;
	if (bytes_written < sizeof(data))
		goto write_more;

	/* Get reply. */
	bytes_read = usb_bulk_read (a->usbdev, OUT_EP, (char*) reply,
		sizeof(reply), 2000);
	if (bytes_read != sizeof(reply)) {
		fprintf (stderr, "bitbang_io(): usb bulk read failed\n");
		switch (bytes_read) {
		case 0: fprintf (stderr, "bitbang_io() empty read\n"); break;
		case 1: fprintf (stderr, "bitbang_io() got 1 byte: %02x\n", reply[bytes_read]); break;
		case 2: fprintf (stderr, "bitbang_io() got 2 bytes: %02x %02x\n", reply[bytes_read], reply[bytes_read+1]); break;
		case 3: fprintf (stderr, "bitbang_io() got 3 bytes: %02x %02x %02x\n", reply[bytes_read], reply[bytes_read+1], reply[bytes_read+2]); break;
		case 4: fprintf (stderr, "bitbang_io() got 4 bytes: %02x %02x %02x %02x\n", reply[bytes_read], reply[bytes_read+1], reply[bytes_read+2], reply[bytes_read+3]); break;
		}
		exit (1);
	}
	if (debug > 1)
		fprintf (stderr, "bitbang_io() got %02x %02x %02x %02x %02x %s\n",
			reply[0], reply[1], reply[2], reply[3], reply[4],
			(reply[3] & READ_TDO) ? "TDO" : "");
	return (reply[3] & READ_TDO) != 0;
}

#if 0
/*
 * JTAG reset.
 */
static int bitbang_reset (bitbang_adapter_t *a, int trst, int sysrst)
{
	unsigned char data [2], reply [4];
	int bytes_written, bytes_read, n;

	data[0] = NTRST | NSYSRST;
	data[1] = data[0];
	if (trst)
		data[0] &= ~NTRST;
	if (sysrst)
		data[0] &= ~NSYSRST;

	/* Write data. */
	bytes_written = 0;
write_more:
	n = usb_bulk_write (a->usbdev, IN_EP, (char*) data + bytes_written,
		sizeof(data) - bytes_written, 1000);
        if (n < 0) {
		fprintf (stderr, "bitbang_reset(): usb bulk write failed\n");
		exit (1);
	}
	if (debug)
		fprintf (stderr, "bitbang_reset(%d, %d) write %u bytes\n", trst, sysrst, n);
	bytes_written += n;
	if (bytes_written < sizeof(data))
		goto write_more;

	/* Get reply. */
	bytes_read = usb_bulk_read (a->usbdev, OUT_EP, (char*) reply,
		sizeof(reply), 2000);
	if (bytes_read != sizeof(reply)) {
		fprintf (stderr, "bitbang_reset(): usb bulk read failed\n");
		exit (1);
	}
	return (reply[3] & READ_TDO) != 0;
}
#endif

static void bitbang_close (adapter_t *adapter)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;

	/* Setup default value of signals. */
	bitbang_io (a, 1, 1);

        if (usb_release_interface (a->usbdev, 0) != 0) {
		fprintf (stderr, "bitbang_close() usb release interface failed\n");
	}
	usb_close (a->usbdev);
	free (a);
}

/*
 * Step to next state in TAP machine.
 * Advances to next TAP state depending on TMS value.
 */
static inline void tap_step (bitbang_adapter_t *a, int tms)
{
	bitbang_io (a, tms, 1);
}

/*
 * Enter a new instr. in TAP machine.
 * Assumes TAP is on Run-Test/Idle or Update-DR/Update-IR states at entry.
 * On exit, stays at Update-IR.
 * Returns nonzero, if a processor is in debug mode.
 */
static int tap_instr (bitbang_adapter_t *a, int nbits, unsigned newinst)
{
	unsigned status = 0, mask;

	tap_step (a, 1);		/* goto Select-DR-Scan */
	tap_step (a, 1);		/* goto Select-IR-Scan */
	tap_step (a, 0);		/* goto Capture-IR */
	tap_step (a, 0);		/* goto Shift-IR */
	for (mask=1; nbits; --nbits) {
		/* If last bit, put TMS=1 to exit Shift-IR state */
		if (bitbang_io (a, nbits == 1, newinst & mask))
			status |= mask;
		mask <<= 1;
	}
	tap_step (a, 1);		/* goto Update-IR */
/*fprintf (stderr, "tap_instr() status = %#x\n", status);*/
	return status;
}

/*
 * Enter a bit stream in the selected DR.
 *
 * Assumes TAP is on Run-Test/Idle or Update-DR/Update-IR states at entry.
 * On exit, stays at Update-DR/Update-IR.
 * Newdata is an array of 32-bit words with the bit stream to send,
 * LSB first. The total number of bits to send is nbits.
 * The readout bit stream is stored in olddata[].
 * If newdata is NULL, zeros are sent in the data stream.
 * If olddata is NULL, readout bits are thrown away.
 */
static void tap_data (bitbang_adapter_t *a,
	int nbits, unsigned char *newdata, unsigned char *olddata)
{
	unsigned databits = 0, readbits = 0;
	unsigned char mask = 0;

	tap_step (a, 1);			/* goto Select-DR-Scan */
	tap_step (a, 0);			/* goto Capture-DR */
	tap_step (a, 0);			/* goto Shift-DR */
	for (; nbits; --nbits) {
		if (mask == 0) {		/* every 8 bits sent... */
			if (newdata)		/* ...get a new word */
				databits = *newdata++;
			mask = 1;
			readbits = 0;
		}
		/* If last bit, put TMS=1 to exit Shift-DR state */
		if (bitbang_io (a, nbits == 1, databits & mask))
			readbits |= mask;	/* compose readout data */
		mask <<= 1;
		if (olddata) {
			*olddata = readbits;	/* save every new bit */
			if (mask == 0)		/* after 32 bits, move ptr */
				olddata++;
		}
	}
	tap_step (a, 1);			/* goto Update-DR */
}

/*
 * Read the Device Identification code
 */
static unsigned bitbang_get_idcode (adapter_t *adapter)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;
	unsigned idcode = 0;

	/* Reset the JTAG TAP controller. */
	tap_step (a, 1);
	tap_step (a, 1);
	tap_step (a, 1);
	tap_step (a, 1);		/* 5 cycles with TMS=1 */
	tap_step (a, 1);
	tap_step (a, 0);		/* TMS=0 to enter run-test/idle */

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
static unsigned bitbang_oncd_read (adapter_t *adapter, int reg, int reglen)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;
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
static void bitbang_oncd_write (adapter_t *adapter,
	unsigned value, int reg, int reglen)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;
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
static void bitbang_stop_cpu (adapter_t *adapter)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;
	unsigned old_ir, i, oscr;

fprintf (stderr, "bitbang_stop_cpu() called\n");
#if 0
	/* Сброс процессора. */
	bitbang_reset (a, 0, 1);
#endif
	/* Debug request. */
	tap_instr (a, 4, TAP_DEBUG_REQUEST);

	/* Wait while processor enters debug mode. */
	i = 0;
	for (;;) {
		old_ir = tap_instr (a, 4, TAP_DEBUG_ENABLE);
/*fprintf (stderr, "bitbang_stop_cpu() old_ir = %#x\n", old_ir);*/
		if (old_ir == 5)
			break;
		mdelay (10);
		if (++i >= 50) {
			fprintf (stderr, "Timeout while entering debug mode\n");
			exit (1);
		}
	}
	bitbang_oncd_write (adapter, 0, OnCD_OBCR, 12);
	oscr = bitbang_oncd_read (adapter, OnCD_OSCR, 32);
	oscr |= OSCR_TME;
	bitbang_oncd_write (adapter, oscr, OnCD_OSCR, 32);
/*fprintf (stderr, "bitbang_stop_cpu() returned\n");*/
}

/*
 * Инициализация адаптера Bitbang.
 * Возвращаем указатель на структуру данных, выделяемую динамически.
 * Если адаптер не обнаружен, возвращаем 0.
 */
adapter_t *adapter_open_bitbang (void)
{
	bitbang_adapter_t *a;
	struct usb_bus *bus;
	struct usb_device *dev;

	usb_init();
	usb_find_busses();
	usb_find_devices();
	for (bus = usb_get_busses(); bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
			if (dev->descriptor.idVendor == BITBANG_VID &&
			    dev->descriptor.idProduct == BITBANG_PID)
				goto found;
		}
	}
	/*fprintf (stderr, "USB adapter not found: vid=%04x, pid=%04x\n",
		BITBANG_VID, BITBANG_PID);*/
	return 0;
found:
	a = calloc (1, sizeof (*a));
	if (! a) {
		fprintf (stderr, "Out of memory\n");
		return 0;
	}
	a->usbdev = usb_open (dev);
	if (! a->usbdev) {
		fprintf (stderr, "Bitbang: usb_open() failed\n");
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
		fprintf (stderr, "Can't set sync bitbang mode\n");
		goto failed;
	}

	/* Ровно 500 нсек между выдачами. */
	unsigned divisor = 0;
	unsigned char latency_timer = 1;

	int baud = (divisor == 0) ? 3000000 :
		(divisor == 1) ? 2000000 : 3000000 / divisor;
	if (debug)
		fprintf (stderr, "Bitbang: speed %d samples/sec\n", baud);

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
		fprintf (stderr, "Bitbang: latency timer: %u usec\n", latency_timer);

	/* Reset the JTAG TAP controller. */
	bitbang_io (a, 1, 1);
	bitbang_io (a, 1, 1);
	bitbang_io (a, 1, 1);
	bitbang_io (a, 1, 1);	/* 5 cycles with TMS=1 */
	bitbang_io (a, 1, 1);
	bitbang_io (a, 0, 1);	/* TMS=0 to enter run-test/idle */

	/* Обязательные функции. */
	a->adapter.name = "Bitbang";
	a->adapter.close = bitbang_close;
	a->adapter.get_idcode = bitbang_get_idcode;
	a->adapter.stop_cpu = bitbang_stop_cpu;
	a->adapter.oncd_read = bitbang_oncd_read;
	a->adapter.oncd_write = bitbang_oncd_write;
#if 0
	/* Расширенные возможности. */
	a->adapter.read_block = bitbang_read_block;
	a->adapter.write_block = bitbang_write_block;
	a->adapter.write_nwords = bitbang_write_nwords;
	a->adapter.program_block32 = bitbang_program_block32;
	a->adapter.program_block32_unprotect = bitbang_program_block32_unprotect;
	a->adapter.program_block32_protect = bitbang_program_block32_protect;
	a->adapter.program_block64 = bitbang_program_block64;
#endif
	return &a->adapter;
}

#ifdef STANDALONE
static int bitbang_test (adapter_t *adapter, int iterations)
{
	bitbang_adapter_t *a = (bitbang_adapter_t*) adapter;
	unsigned result, mask, last_bit = 0;
	unsigned pattern = 0;

	tap_instr (a, 4, TAP_BYPASS);	/* Enter BYPASS mode. */
	tap_step (a, 1);		/* goto Select-DR-Scan */
	tap_step (a, 0);		/* goto Capture-DR */
	tap_step (a, 0);		/* goto Shift-DR */
	do {
		pattern = 1664525ul * pattern + 1013904223ul; /* simple PRNG */

		/* Pass the pattern through TDI-TDO. */
		result = 0;
		mask = 1;
		do {
			if (bitbang_io (a, 0, pattern & mask))
				result += mask;
		} while (mask <<= 1);

		fprintf (stderr, "tap_test sent %08x received %08x\n",
			pattern<<1 | last_bit, result);
		if ((result & 1) != last_bit ||
		    (result >> 1) != (pattern & 0x7FFFFFFFul)) {
			/* Reset the JTAG TAP controller. */
			tap_step (a, 1);
			tap_step (a, 1);
			tap_step (a, 1);
			tap_step (a, 1); /* 5 cycles with TMS=1 */
			tap_step (a, 1);
			tap_step (a, 0); /* TMS=0 to enter run-test/idle */
			return 0;
		}
		last_bit = pattern >> 31;
	} while (--iterations > 0);
	tap_step (a, 1);		/* goto Exit1-DR */
	tap_step (a, 1);		/* goto Update-DR */
	tap_step (a, 0);		/* Run-Test/idle => exec instr. */
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
		printf ("        jtag-bitbang [-v] command\n");
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
		adapter = adapter_open_bitbang ();
		if (! adapter) {
			fprintf (stderr, "No bitbang adapter found.\n");
			exit (1);
		}
		if (strcmp ("test", argv[0]) == 0) {
			if (bitbang_test (adapter, 20))
				printf ("TAP tested OK.\n");
			else
				printf ("TAP test FAILED!\n");

		} else if (strcmp ("idcode", argv[0]) == 0) {
			int i;
			for (i=0; i<10; ++i)
				printf ("IDCODE = %08x\n", bitbang_get_idcode (adapter));
		} else {
			bitbang_close (adapter);
			goto usage;
		}

		bitbang_close (adapter);
		break;
	}
	return 0;
}
#endif /* STANDALONE */
