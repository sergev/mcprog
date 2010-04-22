/*
 * Интерфейс к адаптеру USB-JTAG фирмы Элвис.
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
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "adapter.h"
#include "oncd.h"

#if defined (__CYGWIN32__) || defined (MINGW32)
#   include "libusb-win32/usb.h"
#   include "usb-win32.c"
#else
#   include <libusb-1.0/libusb.h>
#endif

typedef struct {
	/* Общая часть. */
	adapter_t adapter;

	/* Доступ к устройству через libusb. */
	struct libusb_device_handle *usbdev;

	/* Версия аппаратной прошивки адаптера. */
	unsigned hw_version;
} usb_adapter_t;

/* Endpoints for USB-JTAG adapter. */
#define BULK_WRITE_ENDPOINT	2
#define BULK_CONTROL_ENDPOINT	4
#define BULK_READ_ENDPOINT	0x86

/* Commands for control endpoint. */
#define ADAPTER_PLL_12MHZ	0x01
#define ADAPTER_PLL_24MHZ	0x02
#define ADAPTER_PLL_48MHZ	0x03
#define ADAPTER_ACTIVE_RESET	0x04
#define ADAPTER_DEACTIVE_RESET	0x05

/*
 * Пакет, посылаемый адаптеру USB-JTAG, может содержать от одной до 40 команд.
 * Каждая команда занимает от 2 до 6 байт:
 * - код команды;
 * - значение регистра IRd, в том числе номер регистра OnCD;
 * - четыре или два байта данных, или пусто.
 *
 * Код команды имеет следующий формат:
 *                               Вид команды: 00 - обычное чтение/запись;
 *                               |   |        01 - блочная запись;
 *                               |   |        10 - блочное чтение;
 * биты  7   6   5   4   3   2   1   0        11 - запрос idcode или конец блочной операции.
 *       |   |   |   |   |   |
 *       |   |   |   |   |   SYS_RST, 0 - активное состояние
 *       |   |   |   |   TRST, 0 - активное состояние
 *       |   |   Длина данных: 00 - 32 бита;
 *       |   |                 01 - 16 бит;
 *       |   |                 10 - 12 бит;
 *       |   |                 11 - пусто.
 *       |   Debug request/Debug enable, 0 - активное состояние
 *       0 для Dr, 1 для Ir
 */
#define HDR(bits)	(0x7c ^ (bits))	/* Пакет Dr */
#define HIR(bits)	(0xfc ^ (bits))	/* Пакет Ir */

#define H_DEBUG		0x40		/* Debug request/Debug enable */
#define H_32		0x30		/* 32 бита данных */
#define H_TRST		0x08		/* TRST */
#define H_SYSRST	0x04		/* SYS_RST */
#if 1
#define H_BLKWR		0x01		/* блочная запись */
#define H_BLKRD		0x02		/* блочное чтение */
#define H_BLKEND	0x03		/* конец блочной операции */
#else
#define H_BLKWR		0x09		/* неблочная запись */
#define H_BLKRD		0x0a		/* неблочное чтение */
#define H_BLKEND	0x0b		/* конец неблочной операции */
#endif
#define H_IDCODE	0x03		/* запрос idcode */

#if 0
/*
 * Отладочная печать байтового массива.
 */
static void print_pkt (unsigned char *pkt, unsigned len)
{
	unsigned i;

	printf ("%02x", pkt[0]);
	for (i=1; i<len; ++i) {
		printf ("-%02x", pkt[i]);
	}
	printf ("\n");
}
#endif

/*
 * Записать через USB массив данных.
 */
static void bulk_write (struct libusb_device_handle *usbdev,
	const unsigned char *wb, unsigned wlen)
{
	int transferred;

	if (debug) {
		unsigned i;
		fprintf (stderr, "Bulk write: %02x", *wb);
		for (i=1; i<wlen; ++i)
			fprintf (stderr, "-%02x", wb[i]);
		fprintf (stderr, "\n");
	}
	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_WRITE_ENDPOINT,
	    (unsigned char*) wb, wlen, &transferred, 1000) != 0 ||
	    transferred != wlen) {
		fprintf (stderr, "Bulk write failed: %d bytes to endpoint %#x.\n",
			wlen, BULK_WRITE_ENDPOINT);
		_exit (-1);
	}
};

/*
 * Записать команду в Ctrl Pipe.
 */
static void bulk_cmd (struct libusb_device_handle *usbdev,
	unsigned char cmd)
{
	int transferred;

	if (debug)
		fprintf (stderr, "Bulk cmd: %02x\n", cmd);
	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_CONTROL_ENDPOINT,
	    &cmd, 1, &transferred, 1000) != 0 ||
	    transferred != 1) {
		fprintf (stderr, "Bulk cmd failed: command to endpoint %#x.\n",
			BULK_CONTROL_ENDPOINT);
		_exit (-1);
	}
};

/*
 * Прочитать из USB массив данных.
 */
static unsigned bulk_read (struct libusb_device_handle *usbdev,
	unsigned char *rb, unsigned rlen)
{
	int transferred;

	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_READ_ENDPOINT,
	    rb, rlen, &transferred, 1000) != 0) {
		fprintf (stderr, "Bulk read failed: %d/%d bytes from endpoint %#x.\n",
			transferred, rlen, BULK_READ_ENDPOINT);
		_exit (-1);
	}
	if (debug) {
		if (transferred) {
			unsigned i;
			fprintf (stderr, "Bulk read: %02x", *rb);
			for (i=1; i<transferred; ++i)
				fprintf (stderr, "-%02x", rb[i]);
			fprintf (stderr, "\n");
		} else
			fprintf (stderr, "Bulk read: empty\n");
	}
	return transferred;
}

/*
 * Записать и прочитать из USB массив данных.
 */
static unsigned bulk_write_read (struct libusb_device_handle *usbdev,
	const unsigned char *wb, unsigned wlen,
	unsigned char *rb, unsigned rlen)
{
	int transferred;

	if (debug) {
		unsigned i;
		fprintf (stderr, "Bulk write-read: %02x", *wb);
		for (i=1; i<wlen; ++i)
			fprintf (stderr, "-%02x", wb[i]);
		fprintf (stderr, " --> ");
		fflush (stderr);
	}
	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_WRITE_ENDPOINT,
	    (unsigned char*) wb, wlen, &transferred, 1000) != 0 ||
	    transferred != wlen) {
		fprintf (stderr, "Bulk write(-read) failed: %d bytes to endpoint %#x.\n",
			wlen, BULK_WRITE_ENDPOINT);
		_exit (-1);
	}
	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_READ_ENDPOINT,
	    rb, rlen, &transferred, 1000) != 0) {
		fprintf (stderr, "Bulk (write-)read failed: %d/%d bytes from endpoint %#x.\n",
			transferred, rlen, BULK_READ_ENDPOINT);
		_exit (-1);
	}
	if (debug) {
		if (transferred) {
			unsigned i;
			fprintf (stderr, "%02x", *rb);
			for (i=1; i<transferred; ++i)
				fprintf (stderr, "-%02x", rb[i]);
			fprintf (stderr, "\n");
		} else
			fprintf (stderr, "empty\n");
	}
	return transferred;
}

/*
 * Перевод кристалла в режим отладки путём манипуляций
 * регистрами данных JTAG.
 */
static void usb_stop_cpu (adapter_t *adapter)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char rb[8];
	unsigned retry;
	static const unsigned char pkt_debug_request_sysrst[8] = {
		HIR (H_DEBUG | H_SYSRST),
		IR_DEBUG_REQUEST,
	};
	static const unsigned char pkt_debug_enable[8] = {
		HIR (H_DEBUG),
		IR_DEBUG_ENABLE,
	};

	/* Запрос Debug request, сброс процессора. */
	for (retry=0; ; retry++) {
		if (bulk_write_read (a->usbdev, pkt_debug_request_sysrst, 2, rb, 8) != 2) {
			fprintf (stderr, "Failed debug request.\n");
			exit (-1);
		}
		if (rb[0] == 0x45)
			break;
		if (retry > 100) {
			fprintf (stderr, "No reply from debug request.\n");
			exit (-1);
		}
	}

	/* Разрешить отладочный режим. */
	for (retry=0; ; retry++) {
		if (bulk_write_read (a->usbdev, pkt_debug_enable, 2, rb, 8) != 2) {
			fprintf (stderr, "Failed debug enable.\n");
			exit (-1);
		}
		if (rb[0] == 0x55)
			break;
		if (retry > 100) {
			fprintf (stderr, "No reply from debug request.\n");
			exit (-1);
		}
	}
}

/*
 * Чтение регистра IDCODE.
 */
static unsigned usb_get_idcode (adapter_t *adapter)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char rb [8];
	unsigned idcode;
	static const unsigned char pkt_idcode[8] = {
		HDR (H_32 | H_SYSRST | H_IDCODE),
		0x03,
	};

	if (bulk_write_read (a->usbdev, pkt_idcode, 6, rb, 8) != 4) {
		fprintf (stderr, "Failed to get IDCODE.\n");
		exit (-1);
	}
	idcode = *(unsigned*) rb;
	return idcode;
}

/*
 * Заполнение пакета для блочного или неблочного обращения.
 */
static unsigned char *fill_pkt (unsigned char *ptr, unsigned cmd,
	unsigned reg, unsigned data)
{
	*ptr++ = cmd;
	*ptr++ = reg;
	*(unsigned*) ptr = data;
	return ptr + 4;
}

/*
 * Чтение регистра OnCD.
 */
static unsigned usb_oncd_read (adapter_t *adapter, int reg, int nbits)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt[6];
	unsigned val = 0;

	fill_pkt (pkt, HDR (H_32), reg | IRd_READ, 0);
	if (bulk_write_read (a->usbdev, pkt, 6, (unsigned char*) &val, 4) != 4) {
		fprintf (stderr, "Failed to read register.\n");
		exit (-1);
	}
//fprintf (stderr, "OnCD read %d -> %08x\n", reg, val);
	return val;
}

/*
 * Запись регистра OnCD.
 */
static void usb_oncd_write (adapter_t *adapter,
	unsigned val, int reg, int nbits)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt[6];

//fprintf (stderr, "OnCD write %d := %08x\n", reg, val);
	fill_pkt (pkt, HDR (H_32), reg, val);
	bulk_write (a->usbdev, pkt, 6);
}

static void usb_write_block (adapter_t *adapter,
	unsigned nwords, unsigned addr, unsigned *data)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6 + 6*nwords + 6], *ptr = pkt;
	unsigned oscr, i;

	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
	for (i=1; i<nwords; i++)
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMDR, *data++);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, *data);
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to write 4 words.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout writing N words, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
}

void usb_write_nwords (adapter_t *adapter, unsigned nwords, va_list args)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6*2*nwords + 6], *ptr = pkt;
	unsigned i, oscr, data, addr;

	for (i=0; i<nwords; i++) {
		data = va_arg (args, unsigned);
		addr = va_arg (args, unsigned);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, data);
	}
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to write %d words.\n", nwords);
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout writing %d words, aborted. OSCR=%#x\n", nwords, oscr);
		exit (1);
	}
}

void usb_read_block (adapter_t *adapter,
	unsigned nwords, unsigned addr, unsigned *data)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6 + 6*nwords + 6], *ptr = pkt;
	unsigned oscr, i;
#if 1
	/* Блочное чтение. */
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKRD), OnCD_OMAR, addr);
	for (i=1; i<nwords; i++)
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKRD), OnCD_OMDR | IRd_READ, 0);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR | IRd_READ, 0);
#else
	/* Неблочное чтение. */
	ptr = fill_pkt (ptr, HDR (H_32 | H_TRST | H_BLKRD), OnCD_OMAR, addr);
	for (i=1; i<nwords; i++)
		ptr = fill_pkt (ptr, HDR (H_32 | H_TRST | H_BLKRD), OnCD_OMDR | IRd_READ, 0);
	ptr = fill_pkt (ptr, HDR (H_32 | H_TRST | H_BLKEND), OnCD_OMDR | IRd_READ, 0);
#endif
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt,
	    (unsigned char*) data, 4*nwords) != 4*nwords) {
		fprintf (stderr, "Empty data reading memory, aborted.\n");
		exit (1);
	}
	if (bulk_read (a->usbdev, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to read N words.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout reading memory, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
}

void usb_program_block32 (adapter_t *adapter,
	unsigned nwords, unsigned base, unsigned addr, unsigned *data,
	unsigned addr_odd, unsigned addr_even,
	unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6*(8+1)*nwords + 6], *ptr = pkt;
	unsigned oscr, i;
//printf ("usb_program_block32 (nwords = %d, base = %x, addr = %x, cmd_aa = %08x, cmd_55 = %08x, cmd_a0 = %08x)\n",
//nwords, base, addr, cmd_aa, cmd_55, cmd_a0);
	for (i=0; i<nwords; i++) {
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_aa);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_even);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_55);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_a0);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, *data);
		/* delay */
		ptr = fill_pkt (ptr, HDR (H_32), OnCD_OMDR, 0);
		addr += 4;
		data++;
	}
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
}

void usb_program_block32_unprotect (adapter_t *adapter,
	unsigned nwords, unsigned base, unsigned addr, unsigned *data,
	unsigned addr_odd, unsigned addr_even,
	unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6*18 + 6*2*nwords + 6], *ptr = pkt;
	unsigned oscr, i;

	mdelay (10);
//printf ("usb_program_block32_unprotect (nwords = %d, base = %x, addr = %x)\n", nwords, base, addr);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_aa);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_even);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_55);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, 0x80808080);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_aa);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_even);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_55);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, 0x20202020);
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);
	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32 Atmel.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32 Atmel, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
	mdelay (10);

	ptr = pkt;
	for (i=0; i<nwords; i++) {
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, *data);
		addr += 4;
		data++;
	}
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32 Atmel.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32 Atmel, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
	mdelay (40);
}

void usb_program_block32_protect (adapter_t *adapter,
	unsigned nwords, unsigned base, unsigned addr, unsigned *data,
	unsigned addr_odd, unsigned addr_even,
	unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6*18 + 6*2*nwords + 6], *ptr = pkt;
	unsigned oscr, i;

	mdelay (10);
//printf ("usb_program_block32_protect (nwords = %d, base = %x, addr = %x)\n", nwords, base, addr);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_aa);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_even);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_55);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd);
	ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_a0);
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);
	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32 Atmel.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32 Atmel, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
	mdelay (10);

	ptr = pkt;
	for (i=0; i<nwords; i++) {
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, *data);
		addr += 4;
		data++;
	}
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32 Atmel.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32 Atmel, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
	mdelay (40);
}

void usb_program_block64 (adapter_t *adapter,
	unsigned nwords, unsigned base, unsigned addr, unsigned *data,
	unsigned addr_odd, unsigned addr_even,
	unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;
	unsigned char pkt [6*8*nwords + 6], *ptr = pkt;
	unsigned oscr, i;
//printf ("usb_program_block64 (nwords = %d, base = %x, cmd_a0 = %08x,  addr = %x)\n", nwords, base, cmd_a0, addr);
	for (i=0; i<nwords; i++) {
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd + (addr & 4));
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_aa);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_even + (addr & 4));
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_55);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, base + addr_odd + (addr & 4));
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, cmd_a0);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKWR), OnCD_OMAR, addr);
		ptr = fill_pkt (ptr, HDR (H_32 | H_BLKEND), OnCD_OMDR, *data);
		addr += 4;
		data++;
	}
	ptr = fill_pkt (ptr, HDR (H_32), OnCD_OSCR | IRd_READ, 0);

	if (bulk_write_read (a->usbdev, pkt, ptr - pkt, (unsigned char*) &oscr, 4) != 4) {
		fprintf (stderr, "Failed to program block32.\n");
		exit (-1);
	}
	if (! (oscr & OSCR_RDYm)) {
		fprintf (stderr, "Timeout programming block32, aborted. OSCR=%#x\n", oscr);
		exit (1);
	}
}

/*
 * Завершение работы с адаптером и освобождение памяти.
 */
static void usb_close (adapter_t *adapter)
{
	usb_adapter_t *a = (usb_adapter_t*) adapter;

	libusb_release_interface (a->usbdev, 0);
	libusb_close (a->usbdev);
	free (a);
}

/*
 * Инициализация адаптера USB-JTAG.
 * Возвращаем указатель на структуру данных, выделяемую динамически.
 * Если адаптер не обнаружен, возвращаем 0.
 */
adapter_t *adapter_open_usb (void)
{
	static const unsigned char pkt_reset[8] = {
		/* Посылаем команду чтения MEM, но с активным TRST. */
		HDR (H_DEBUG | H_TRST),
		OnCD_MEM | IRd_READ,
	};
	static const unsigned char pkt_getver[8] = {
		HIR (H_DEBUG | H_TRST | H_SYSRST),
		IR_BYPASS
	};
	usb_adapter_t *a;
	unsigned char rb[32];
	int cfg;

	if (libusb_init (0) < 0) {
		fprintf (stderr, "Failed to initialize libusb.\n");
		return 0;
	}

	a = calloc (1, sizeof (*a));
	if (! a) {
		fprintf (stderr, "Out of memory\n");
		return 0;
	}
	a->usbdev = libusb_open_device_with_vid_pid (0, 0x0547, 0x1002);
	if (! a->usbdev) {
/*		fprintf (stderr, "USB-JTAG Multicore adapter not found.\n");*/
		free (a);
		return 0;
	}
	if (libusb_get_configuration (a->usbdev, &cfg) != 0 || cfg != 1)
		libusb_set_configuration (a->usbdev, 1);
	libusb_claim_interface (a->usbdev, 0);

	bulk_cmd (a->usbdev, ADAPTER_PLL_12MHZ);
/*	bulk_cmd (a->usbdev, ADAPTER_PLL_48MHZ);*/
	mdelay (1);
	bulk_cmd (a->usbdev, ADAPTER_ACTIVE_RESET);
	mdelay (1);
	bulk_cmd (a->usbdev, ADAPTER_DEACTIVE_RESET);
	mdelay (1);

	/* Сброс OnCD. */
	bulk_write_read (a->usbdev, pkt_reset, 2, rb, 32);

	/* Получить версию прошивки. */
	if (bulk_write_read (a->usbdev, pkt_getver, 2, rb, 8) != 2) {
		fprintf (stderr, "Failed to get adapter version.\n");
		free (a);
		return 0;
	}
	a->hw_version = *(unsigned short*) rb >> 8;
	fprintf (stderr, "USB adapter version: %02x\n", a->hw_version);

	/* Обязательные функции. */
	a->adapter.name = "Elvees USB";
	a->adapter.close = usb_close;
	a->adapter.get_idcode = usb_get_idcode;
	a->adapter.stop_cpu = usb_stop_cpu;
	a->adapter.oncd_read = usb_oncd_read;
	a->adapter.oncd_write = usb_oncd_write;

	/* Расширенные возможности. */
	a->adapter.read_block = usb_read_block;
	a->adapter.write_block = usb_write_block;
	a->adapter.write_nwords = usb_write_nwords;
	a->adapter.program_block32 = usb_program_block32;
	a->adapter.program_block32_unprotect = usb_program_block32_unprotect;
	a->adapter.program_block32_protect = usb_program_block32_protect;
	a->adapter.program_block64 = usb_program_block64;

	return &a->adapter;
}
