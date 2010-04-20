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
 * TAP instructions for Elvees JTAG.
 */
#define TAP_IDCODE		0x03	/* Select the ID register */
//#define TAP_IDCODE		0x0E	/* ARM7 */
#define TAP_DEBUG_REQUEST	0x04	/* Stop processor */
#define TAP_DEBUG_ENABLE	0x05	/* Put processor in debug mode */
#define TAP_BYPASS		0x0F	/* Select the BYPASS register */

/*
 * USB endpoints.
 */
#define IN_EP			0x02
#define OUT_EP			0x81

#define MAXPKTSZ		64

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

static usb_dev_handle *adapter;

int debug;

/*
 * JTAG i/o.
 */
int bitbang_io (int tms, int tdi)
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
	n = usb_bulk_write (adapter, IN_EP, (char*) data + bytes_written,
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
	bytes_read = usb_bulk_read (adapter, OUT_EP, (char*) reply,
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
	if (debug)
		fprintf (stderr, "bitbang_io() got %02x %02x %02x %02x %02x %s\n",
			reply[0], reply[1], reply[2], reply[3], reply[4],
			(reply[3] & READ_TDO) ? "TDO" : "");
	return (reply[3] & READ_TDO) != 0;
}

void bitbang_close (void)
{
	/* Setup default value of signals. */
	bitbang_io (1, 1);

        if (usb_release_interface (adapter, 0) != 0) {
		fprintf (stderr, "bitbang_close() usb release interface failed\n");
	}
	usb_close (adapter);
}

/*
 * Bitbang adapter initialization
 */
void bitbang_init (void)
{
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
	fprintf (stderr, "USB adapter not found: vid=%04x, pid=%04x\n",
		BITBANG_VID, BITBANG_PID);
	exit (1);
found:
	adapter = usb_open (dev);
	if (! adapter) {
		fprintf (stderr, "usb_open() failed\n");
		exit (1);
	}
	usb_claim_interface (adapter, 0);

	/* Reset the ftdi device. */
	if (usb_control_msg (adapter,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_RESET, 0, 0, 0, 0, 1000) != 0) {
		fprintf (stderr, "FTDI reset failed\n");
		exit (1);
	}

	/* Sync bit bang mode. */
	if (usb_control_msg (adapter,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_BITMODE, TCK | TDI | TMS | NTRST | NSYSRST | 0x400,
	    0, 0, 0, 1000) != 0) {
		fprintf (stderr, "Can't set sync bitbang mode\n");
		exit (1);
	}

	/* Ровно 500 нсек между выдачами. */
	unsigned divisor = 1;
	unsigned char latency_timer = 1;

	int baud = 3000000 / divisor * 16;
	fprintf (stderr, "Speed %d samples/sec\n", baud);

	/* Frequency divisor is 14-bit non-zero value. */
	if (usb_control_msg (adapter,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_BAUD_RATE, divisor,
	    0, 0, 0, 1000) != 0) {
		fprintf (stderr, "Can't set baud rate\n");
		exit (1);
	}

	if (usb_control_msg (adapter,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
	    SIO_SET_LATENCY_TIMER, latency_timer, 0, 0, 0, 1000) != 0) {
		fprintf (stderr, "unable to set latency timer\n");
		exit (1);
	}
	if (usb_control_msg (adapter,
	    USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN,
	    SIO_GET_LATENCY_TIMER, 0, 0, (char*) &latency_timer, 1, 1000) != 1) {
		fprintf (stderr, "unable to get latency timer\n");
		exit (1);
	}
	fprintf (stderr, "Latency timer: %u usec\n", latency_timer);

	/* Reset the JTAG TAP controller. */
	bitbang_io (1, 1);
	bitbang_io (1, 1);
	bitbang_io (1, 1);
	bitbang_io (1, 1);	/* 5 cycles with TMS=1 */
	bitbang_io (1, 1);
	bitbang_io (0, 1);	/* TMS=0 to enter run-test/idle */
}

/*
 * Step to next state in TAP machine.
 * Advances to next TAP state depending on TMS value.
 */
static inline void tap_step (int tms)
{
	bitbang_io (tms, 1);
}

/*
 * Enter a new instr. in TAP machine.
 * Assumes TAP is on Run-Test/Idle or Update-DR/Update-IR states at entry.
 * On exit, stays at Update-IR.
 * Returns nonzero, if a processor is in debug mode.
 */
int tap_instr (int nbits, unsigned newinst)
{
	unsigned status = 0, mask;

	tap_step (1);		/* goto Select-DR-Scan */
	tap_step (1);		/* goto Select-IR-Scan */
	tap_step (0);		/* goto Capture-IR */
	tap_step (0);		/* goto Shift-IR */
	for (mask=1; nbits; --nbits) {
		/* If last bit, put TMS=1 to exit Shift-IR state */
		if (bitbang_io (nbits == 1, newinst & mask))
			status |= mask;
		mask <<= 1;
	}
	tap_step (1);		/* goto Update-IR */
/*fprintf (stderr, "tap_instr() status = %#x\n", status);*/
	return status & 4;
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
void tap_data (int nbits, unsigned *newdata, unsigned *olddata)
{
	unsigned databits = 0;
	unsigned mask = 1;
	unsigned readbits = 0;
	int bits_sent = 0;

	tap_step (1);			/* goto Select-DR-Scan */
	tap_step (0);			/* goto Capture-DR */
	tap_step (0);			/* goto Shift-DR */
	for (; nbits; --nbits) {
		if (! (bits_sent & 31)) {	/* every 32 bits sent... */
			if (newdata)		/* ...get a new word */
				databits = *newdata++;
			mask = 1;
			readbits = 0;
		}
		/* If last bit, put TMS=1 to exit Shift-DR state */
		if (bitbang_io (nbits == 1, databits & mask))
			readbits |= mask;	/* compose readout data */
		mask <<= 1;
		if (olddata) {
			*olddata = readbits;	/* save every new bit */
			if (mask == 0)		/* after 32 bits, move ptr */
				olddata++;
		}
		bits_sent++;
	}
	tap_step (1);			/* goto Update-DR */
}

/*
 * Read the Device Identification code
 */
unsigned tap_read_idcode (void)
{
	unsigned idcode = 0;

	/* Reset the JTAG TAP controller. */
	tap_step (1);
	tap_step (1);
	tap_step (1);
	tap_step (1);			/* 5 cycles with TMS=1 */
	tap_step (1);
	tap_step (0);			/* TMS=0 to enter run-test/idle */

	tap_instr (4, TAP_IDCODE);	/* Select IDCODE register */
	tap_data (32, 0, &idcode);	/* Read out 32 bits into idcode */
	return idcode;
}

static unsigned tap_bypass (unsigned data)
{
	unsigned result, mask;

	result = 0;
	mask = 1;
	do {
		if (bitbang_io (0, data & mask))
			result += mask;
	} while (mask <<= 1);
	return result;
}

int tap_test (int iterations)
{
	unsigned test, last_bit = 0;
	unsigned pattern = 0;

	tap_instr (4, TAP_BYPASS);	/* Enter BYPASS mode. */
	tap_step (1);			/* goto Select-DR-Scan */
	tap_step (0);			/* goto Capture-DR */
	tap_step (0);			/* goto Shift-DR */
	do {
		pattern = 1664525ul * pattern + 1013904223ul; /* simple PRNG */
		test = tap_bypass (pattern);
		fprintf (stderr, "tap_test sent %08x received %08x\n",
			pattern<<1 | last_bit, test);
		if ((test & 1) != last_bit ||
		    (test >> 1) != (pattern & 0x7FFFFFFFul)) {
			/* Reset the JTAG TAP controller. */
			tap_step (1);
			tap_step (1);
			tap_step (1);
			tap_step (1);	/* 5 cycles with TMS=1 */
			tap_step (1);
			tap_step (0);	/* TMS=0 to enter run-test/idle */
			return 0;
		}
		last_bit = pattern >> 31;
	} while (--iterations > 0);
	tap_step (1);			/* goto Exit1-DR */
	tap_step (1);			/* goto Update-DR */
	tap_step (0);			/* Run-Test/idle => exec instr. */
	return 1;
}

#ifdef STANDALONE
int main (int argc, char **argv)
{
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
		if (strcmp ("test", argv[0]) == 0) {
			bitbang_init ();
			if (tap_test (20))
				printf ("TAP tested OK.\n");
			else
				printf ("TAP test FAILED!\n");
			bitbang_close ();

		} else if (strcmp ("idcode", argv[0]) == 0) {
			int i;
			bitbang_init ();
			for (i=0; i<1000000; ++i)
				printf ("IDCODE = %08x\n", tap_read_idcode());
			bitbang_close ();
		} else
			goto usage;
		break;
	}
	return 0;
}
#endif /* STANDALONE */
