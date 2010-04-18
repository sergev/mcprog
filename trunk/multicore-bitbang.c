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

#include <ftdi.h>

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
#define BITBANG_SPEED		100000

/*
 * TAP instructions for Elvees JTAG.
 */
#define TAP_IDCODE		0x03	/* Select the ID register */
#define TAP_DEBUG_REQUEST	0x04	/* Stop processor */
#define TAP_DEBUG_ENABLE	0x05	/* Put processor in debug mode */
#define TAP_BYPASS		0x0F	/* Select the BYPASS register */

static struct ftdi_context adapter;

int debug;

/*
 * JTAG i/o.
 */
int bitbang_io (int tms, int tdi)
{
	unsigned char data [3], input [3];
	int bytes_written, bytes_read, n;

	data[0] = NTRST | NSYSRST;
	if (tms)
		data[0] |= TMS;
	if (tdi)
		data[0] |= TDI;
	data[1] = data[0] | TCK;
	data[2] = data[0];

	if (debug)
		fprintf (stderr, "bitbang_io() write %u bytes\n",
			(unsigned) sizeof(data));
	bytes_written = ftdi_write_data (&adapter, data, sizeof(data));
	if (bytes_written < 0) {
		fprintf (stderr, "ftdi_write_data: %s\n", ftdi_get_error_string(&adapter));
		exit (1);
	}
	if (bytes_written != sizeof(data)) {
		fprintf (stderr, "ftdi_write_data: written %d bytes, expected %u\n",
			bytes_written, (unsigned) sizeof(data));
		exit (1);
	}
	bytes_read = 0;
again:
	adapter.readbuffer_remaining = 0;
	n = ftdi_read_data (&adapter, input + bytes_read, sizeof(input) - bytes_read);
	if (n < 0) {
		fprintf (stderr, "bitbang_io(): read failed\n");
		exit (1);
	}
	if (debug)
		fprintf (stderr, "bitbang_io() got %d bytes\n", n);
	bytes_read += n;
	if (bytes_read < sizeof(input))
		goto again;
	/*fprintf (stderr, "bitbang_io() got %02x %02x %s\n",
		input[0], input[1], (input[1] & READ_TDO) ? "TDO" : "");*/
	return (input[1] & READ_TDO) != 0;
}

void bitbang_close (void)
{
	/* Setup default value of signals. */
	bitbang_io (1, 1);
	ftdi_usb_close (&adapter);
	ftdi_deinit (&adapter);
}

/*
 * Bitbang adapter initialization
 */
void bitbang_init (void)
{
	if (ftdi_init (&adapter) < 0)
		exit (1);

	if (ftdi_usb_open (&adapter, BITBANG_VID, BITBANG_PID) < 0) {
		fprintf (stderr, "unable to open ftdi device with vid=%04x, pid=%04x\n",
			BITBANG_VID, BITBANG_PID);
		fprintf (stderr, "%s", adapter.error_str);
		exit (1);
	}

	if (ftdi_usb_reset (&adapter) < 0) {
		fprintf (stderr, "unable to reset ftdi device\n");
		exit (1);
	}

	unsigned char latency_timer;
	if (ftdi_set_latency_timer (&adapter, 2) < 0) {
		fprintf (stderr, "unable to set latency timer\n");
		exit (1);
	}

	if (ftdi_get_latency_timer (&adapter, &latency_timer) < 0) {
		fprintf (stderr, "unable to get latency timer\n");
		exit (1);
	}
	fprintf (stderr, "current latency timer: %u\n", latency_timer);
	adapter.usb_write_timeout = 1000;
	adapter.usb_read_timeout = 1000;

	/* Sync bit bang mode. */
	ftdi_set_bitmode (&adapter, TCK | TDI | TMS | NTRST | NSYSRST, BITMODE_RESET);
	ftdi_set_bitmode (&adapter, TCK | TDI | TMS | NTRST | NSYSRST, BITMODE_SYNCBB);

	int baud = BITBANG_SPEED / 16;
	fprintf (stderr, "speed %d x 16 bits/sec\n", baud);

	if (ftdi_set_baudrate (&adapter, baud) < 0) {
		fprintf (stderr, "Can't set baud rate: %s\n",
			ftdi_get_error_string(&adapter));
		exit (1);
	}
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
			bitbang_init ();
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			printf ("IDCODE = %08x\n", tap_read_idcode());
			bitbang_close ();
		} else
			goto usage;
		break;
	}
	return 0;
}
#endif /* STANDALONE */
