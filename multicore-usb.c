/*
 * Интерфейс через адаптер USB-JTAG к процессору Элвис Мультикор.
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
#include <libusb-1.0/libusb.h>

#include "multicore.h"

#define NFLASH		16	/* Max flash regions. */

struct _multicore_t {
	char		*cpu_name;
	unsigned 	idcode;
	unsigned	flash_width;
	unsigned	flash_bytes;
	unsigned	flash_addr_odd;
	unsigned	flash_addr_even;
	unsigned	flash_cmd_aa;
	unsigned	flash_cmd_55;
	unsigned	flash_cmd_10;
	unsigned	flash_cmd_80;
	unsigned	flash_cmd_90;
	unsigned	flash_cmd_a0;
	unsigned	flash_cmd_f0;
	unsigned	flash_devid_offset;
	unsigned	flash_base [NFLASH];
	unsigned	flash_last [NFLASH];

	struct libusb_device_handle *usbdev;
};

/*
 * Регистр конфигурации 3
 */
static unsigned cscon3;

#define MC_CSCON3		0x182F100C
#define MC_CSCON3_ADDR(addr)	((addr & 3) << 20)

/* Идентификатор производителя flash. */
#define ID_ALLIANCE		0x00520052
#define ID_AMD			0x00010001
#define ID_SST			0x00BF00BF
#define ID_MILANDR		0x01010101

/* Идентификатор микросхемы flash. */
#define ID_29LV800_B		0x225b225b
#define ID_29LV800_T		0x22da22da
#define ID_39VF800_A		0x27812781
#define ID_39VF6401_B		0x236d236d
#define ID_1636PP2Y		0xc8c8c8c8

/* Команды flash. */
#define FLASH_CMD16_AA		0x00AA00AA
#define FLASH_CMD8_AA		0xAAAAAAAA
#define FLASH_CMD16_55		0x00550055
#define FLASH_CMD8_55		0x55555555
#define FLASH_CMD16_10		0x00100010	/* Chip erase 2/2 */
#define FLASH_CMD8_10		0x10101010
#define FLASH_CMD16_80		0x00800080	/* Chip erase 1/2 */
#define FLASH_CMD8_80		0x80808080
#define FLASH_CMD16_90		0x00900090	/* Read ID */
#define FLASH_CMD8_90		0x90909090
#define FLASH_CMD16_A0		0x00A000A0	/* Program */
#define FLASH_CMD8_A0		0xA0A0A0A0
#define FLASH_CMD16_F0		0x00F000F0	/* Reset */
#define FLASH_CMD8_F0		0xF0F0F0F0

/* Идентификатор версии процессора. */
#define MC12_ID			0x20777001
#define MC12REV1_ID		0x30777001

/* OnCD Register Map */
#define	OnCD_OSCR	0x0	/* Control & State Register */
#define	OnCD_OMBC	0x1	/* BreakPoint Match Counter */
#define	OnCD_OMLR0	0x2	/* Address Boundary Register 0 */
#define	OnCD_OMLR1	0x3	/* Address Boundary Register 1 */
#define	OnCD_OBCR	0x4	/* BreakPoint Control Register */
#define	OnCD_IRdec	0x5	/* Last CPU Instruction, can be supplied. */
#define	OnCD_OTC	0x6	/* Trace Counter */
#define	OnCD_PCdec	0x7	/* Decoding Instruction(IRdec) Address */
#define	OnCD_PCexec	0x8	/* Executing Instruction Address */
#define	OnCD_PCmem	0x9	/* Memory Access Instruction Address */
#define	OnCD_PCfetch	0xA	/* PC (Fetching Instruction Address) */
#define	OnCD_OMAR	0xB	/* Memory Address Register */
#define	OnCD_OMDR	0xC	/* Memory Data Register */
#define	OnCD_MEM	0xD	/* Memory Access Register (EnMEM) */
#define	OnCD_PCwb	0xE	/* Address of instruction at write back stage */
#define	OnCD_MEMACK	0xE	/* Memory Operation Acknowlege (EnXX) */
#define	OnCD_EXIT	0xF	/* Exit From Debug Mode (EnGO) */

/* OSCR Register */
#define	OSCR_SlctMEM	0x0001	/* Allow Memory Access */
#define OSCR_RO		0x0002	/* 0: Write, 1: Read */
#define OSCR_TME	0x0004	/* Trace Mode Enable */
#define OSCR_IME	0x0008	/* Debug Interrupt Enable */
#define OSCR_MPE	0x0010	/* Flash CPU Pipeline At Debug Exit */
#define OSCR_RDYm	0x0020	/* RDY signal state */
#define OSCR_MBO	0x0040	/* BreakPoint triggered */
#define OSCR_TO		0x0080	/* Trace Counter triggered */
#define OSCR_SWO	0x0100	/* SoftWare enter into Debug mode */
#define OSCR_SO		0x0200	/* CPU Mode, 0: running, 1: Debug */
#define OSCR_DBM	0x0400	/* Debug Mode On, mc12 and above */
#define OSCR_NDS	0x0800	/* Do not stop in delay slot, mc12 and above */
#define OSCR_VBO	0x1000	/* Exception catched, mc12 and above */
#define OSCR_NFEXP	0x2000	/* Do not raise exception at pc fetch */
#define OSCR_WP0	0x4000	/* WP0 triggered, only for mc12 and above */
#define OSCR_WP1	0x8000	/* WP1 triggered, only for mc12 and above */

extern int debug;

#define BULK_WRITE_ENDPOINT	2
#define BULK_READ_ENDPOINT	0x86

/**
 * Записать и прочитать из USB массив данных
 * @param wb - буфер для записи
 * @param wlen - длина записываемых данных
 * @param rb - буфер для чтения
 * @param rlen - длина буфера чтения
 * @return - количество прочитанных байт
 */
static unsigned bulk_write_read (multicore_t *mc, const unsigned char *wb,
	unsigned wlen, unsigned char *rb, unsigned rlen)
{
	int transferred;

	transferred = 0;
	if (libusb_bulk_transfer (mc->usbdev, BULK_WRITE_ENDPOINT,
	    (unsigned char*) wb, wlen, &transferred, 1000) != 0 ||
	    transferred != wlen) {
		fprintf (stderr, "Bulk write failed: %d bytes to endpoint %d.\n",
			wlen, BULK_WRITE_ENDPOINT);
		exit (-1);
	}
	transferred = 0;
	if (libusb_bulk_transfer (mc->usbdev, BULK_READ_ENDPOINT,
	    rb, rlen, &transferred, 1000) != 0) {
		fprintf (stderr, "Bulk read failed: %d/%d bytes from endpoint %d.\n",
			transferred, rlen, BULK_READ_ENDPOINT);
		exit (-1);
	}
	return transferred;
};

/*
 * Установка доступа к аппаратным портам ввода-вывода.
 */
void multicore_init ()
{
	if (libusb_init (NULL) < 0) {
		fprintf (stderr, "Failed to initialize libusb.\n");
		exit (-1);
	}
}

/*
 * Open the device.
 */
multicore_t *multicore_open ()
{
	multicore_t *mc;
	const unsigned char pkt_idcode[8] = { 0x4b, 0x03 };
	unsigned char rb [8];

	mc = calloc (1, sizeof (multicore_t));
	if (! mc) {
		fprintf (stderr, "Out of memory\n");
		exit (-1);
	}
	mc->cpu_name = "Unknown";

	mc->usbdev = libusb_open_device_with_vid_pid (NULL, 0x0547, 0x1002);
	if (! mc->usbdev) {
		fprintf (stderr, "USB-JTAG Multicore adapter not found.d\n");
		exit (-1);
	}
	/* For ARM7TDMI must be 0x1f0f0f0f. */
	if (bulk_write_read (mc, pkt_idcode, 6, rb, 8) != 4) {
		fprintf (stderr, "Failed to get IDCODE.\n");
		exit (-1);
	}
	mc->idcode = *(unsigned*) rb;
	if (debug)
		fprintf (stderr, "idcode %08X\n", mc->idcode);
	switch (mc->idcode) {
	default:
		/* Device not detected. */
		if (mc->idcode == 0xffffffff || mc->idcode == 0)
			fprintf (stderr, "No response from device -- check power is on!\n");
		else
			fprintf (stderr, "No response from device -- unknown idcode 0x%08X!\n",
				mc->idcode);
		exit (1);
	case MC12_ID:
		mc->cpu_name = "MC12";
		break;
	case MC12REV1_ID:
		mc->cpu_name = "MC12r1";
		break;
	}
#if 0
	jtag_reset ();
#endif
	return mc;
}

/*
 * Close the device.
 */
void multicore_close (multicore_t *mc)
{
}

/*
 * Add a flash region.
 */
void multicore_flash_configure (multicore_t *mc, unsigned first, unsigned last)
{
	int i;

	for (i=0; i<NFLASH; ++i) {
		if (! mc->flash_last [i]) {
			mc->flash_base [i] = first;
			mc->flash_last [i] = last;
			return;
		}
	}
	fprintf (stderr, "multicore_flash_configure: too many flash regions.\n");
	exit (1);
}

/*
 * Iterate trough all flash regions.
 */
unsigned multicore_flash_next (multicore_t *mc, unsigned prev, unsigned *last)
{
	int i;

	if (prev == ~0 && mc->flash_base [0]) {
		*last = mc->flash_last [0];
		return mc->flash_base [0];
	}
	for (i=1; i<NFLASH && mc->flash_last[i]; ++i) {
		if (prev >= mc->flash_base [i-1] &&
		    prev <= mc->flash_last [i-1]) {
			*last = mc->flash_last [i];
			return mc->flash_base [i];
		}
	}
	return ~0;
}

char *multicore_cpu_name (multicore_t *mc)
{
	return mc->cpu_name;
}

unsigned multicore_idcode (multicore_t *mc)
{
	return mc->idcode;
}

/*
 * Вычисление базового адреса микросхемы flash-памяти.
 */
static unsigned compute_base (multicore_t *mc, unsigned addr)
{
	int i;

	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	for (i=0; i<NFLASH && mc->flash_last[i]; ++i) {
		if (addr >= mc->flash_base [i] &&
		    addr <= mc->flash_last [i])
			return mc->flash_base [i];
	}
	fprintf (stderr, "multicore: no flash region for address 0x%08X\n", addr);
	exit (1);
	return 0;
}

int multicore_flash_detect (multicore_t *mc, unsigned addr,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width)
{
#if 0
	int count;
	unsigned base;

	base = compute_base (mc, addr);
	for (count=0; count<16; ++count) {
		/* Try both 32 and 64 bus width.*/
		switch (count & 3) {
		case 0:
			/* Two 16-bit flash chips. */
			mc->flash_width = 32;
			mc->flash_addr_odd = 0x5555 << 2;
			mc->flash_addr_even = 0x2AAA << 2;
			mc->flash_cmd_aa = FLASH_CMD16_AA;
			mc->flash_cmd_55 = FLASH_CMD16_55;
			mc->flash_cmd_10 = FLASH_CMD16_10;
			mc->flash_cmd_80 = FLASH_CMD16_80;
			mc->flash_cmd_90 = FLASH_CMD16_90;
			mc->flash_cmd_a0 = FLASH_CMD16_A0;
			mc->flash_cmd_f0 = FLASH_CMD16_F0;
			mc->flash_devid_offset = 4;
			break;
		case 1:
			/* Four 16-bit flash chips. */
			mc->flash_width = 64;
			mc->flash_addr_odd = 0x5555 << 3;
			mc->flash_addr_even = 0x2AAA << 3;
			mc->flash_cmd_aa = FLASH_CMD16_AA;
			mc->flash_cmd_55 = FLASH_CMD16_55;
			mc->flash_cmd_10 = FLASH_CMD16_10;
			mc->flash_cmd_80 = FLASH_CMD16_80;
			mc->flash_cmd_90 = FLASH_CMD16_90;
			mc->flash_cmd_a0 = FLASH_CMD16_A0;
			mc->flash_cmd_f0 = FLASH_CMD16_F0;
			mc->flash_devid_offset = 8;
			break;
		case 2:
			/* Four 8-bit flash chips. */
			mc->flash_width = 32;
			mc->flash_addr_odd = 0x555 << 2;
			mc->flash_addr_even = 0x2AA << 2;
			mc->flash_cmd_aa = FLASH_CMD8_AA;
			mc->flash_cmd_55 = FLASH_CMD8_55;
			mc->flash_cmd_10 = FLASH_CMD8_10;
			mc->flash_cmd_80 = FLASH_CMD8_80;
			mc->flash_cmd_90 = FLASH_CMD8_90;
			mc->flash_cmd_a0 = FLASH_CMD8_A0;
			mc->flash_cmd_f0 = FLASH_CMD8_F0;
			mc->flash_devid_offset = 4;
			break;
		case 3:
			/* One 8-bit flash chip. */
			mc->flash_width = 8;
			mc->flash_addr_odd = 0x555;
			mc->flash_addr_even = 0x2AA;
			mc->flash_cmd_aa = FLASH_CMD8_AA;
			mc->flash_cmd_55 = FLASH_CMD8_55;
			mc->flash_cmd_10 = FLASH_CMD8_10;
			mc->flash_cmd_80 = FLASH_CMD8_80;
			mc->flash_cmd_90 = FLASH_CMD8_90;
			mc->flash_cmd_a0 = FLASH_CMD8_A0;
			mc->flash_cmd_f0 = FLASH_CMD8_F0;
			mc->flash_devid_offset = 0;
			break;
		}
		/* Read device code. */
		if (mc->flash_width == 8) {
			/* Byte-wide data bus. */
			cscon3 = jtag_read_word (MC_CSCON3) & ~MC_CSCON3_ADDR (3);
			jtag_write_byte (mc->flash_cmd_aa, base + mc->flash_addr_odd);
			jtag_write_byte (mc->flash_cmd_55, base + mc->flash_addr_even);
			jtag_write_byte (mc->flash_cmd_90, base + mc->flash_addr_odd);
			*mf = jtag_read_word (base);
			*dev = (unsigned char) (*mf >> 8);
			*mf = (unsigned char) *mf;

			/* Stop read ID mode. */
			jtag_write_byte (mc->flash_cmd_f0, base);
		} else {
			/* Word-wide data bus. */
			jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd);
			jtag_write_word (mc->flash_cmd_55, base + mc->flash_addr_even);
			jtag_write_word (mc->flash_cmd_90, base + mc->flash_addr_odd);
			*dev = jtag_read_word (base + mc->flash_devid_offset);
			*mf = jtag_read_word (base);

			/* Stop read ID mode. */
			jtag_write_word (mc->flash_cmd_f0, base);
		}

		if (debug > 1)
			fprintf (stderr, "flash id %08X\n", *dev);
		switch (*dev) {
		case ID_29LV800_B:
			strcpy (devname, "29LV800B");
			mc->flash_bytes = 2*1024*1024 * mc->flash_width / 32;
			goto success;
		case ID_29LV800_T:
			strcpy (devname, "29LV800T");
			mc->flash_bytes = 2*1024*1024 * mc->flash_width / 32;
			goto success;
		case ID_39VF800_A:
			strcpy (devname, "39VF800A");
			mc->flash_bytes = 2*1024*1024 * mc->flash_width / 32;
			goto success;
		case ID_39VF6401_B:
			strcpy (devname, "39VF6401B");
			mc->flash_bytes = 16*1024*1024 * mc->flash_width / 32;
			goto success;
		case ID_1636PP2Y:
			strcpy (devname, "1636PP2Y");
			mc->flash_bytes = 4*2*1024*1024;
			goto success;
		case (unsigned char) ID_1636PP2Y:
			if (mc->flash_width != 8)
				break;
			strcpy (devname, "1636PP2Y");
			mc->flash_bytes = 2*1024*1024;
			goto success;
		}
	}
#endif
	/* printf ("Unknown flash id = %08X\n", *dev); */
	return 0;
#if 0
success:
	/* Read MFR code. */
	switch (*mf) {
	case ID_ALLIANCE:
		strcpy (mfname, "Alliance");
		break;
	case ID_AMD:
		strcpy (mfname, "AMD");
		break;
	case ID_SST:
		strcpy (mfname, "SST");
		break;
	case ID_MILANDR:
		strcpy (mfname, "Milandr");
		break;
	case (unsigned char) ID_MILANDR:
		if (mc->flash_width != 8)
			goto unknown_mfr;
		strcpy (mfname, "Milandr");
		break;
	default:
unknown_mfr:	sprintf (mfname, "<%08X>", *mf);
		break;
	}

	*bytes = mc->flash_bytes;
	*width = mc->flash_width;
	return 1;
#endif
}

int multicore_erase (multicore_t *mc, unsigned addr)
{
#if 0
	unsigned word, base;

	/* Chip erase. */
	base = compute_base (mc, addr);
	printf ("Erase: %08X", base);
	if (mc->flash_width == 8) {
		/* 8-разрядная шина. */
		jtag_write_byte (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_byte (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_byte (mc->flash_cmd_80, base + mc->flash_addr_odd);
		jtag_write_byte (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_byte (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_byte (mc->flash_cmd_10, base + mc->flash_addr_odd);
	} else {
		jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_word (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_word (mc->flash_cmd_80, base + mc->flash_addr_odd);
		jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_word (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_word (mc->flash_cmd_10, base + mc->flash_addr_odd);
		if (mc->flash_width == 64) {
			/* Старшая половина 64-разрядной шины. */
			jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd + 4);
			jtag_write_word (mc->flash_cmd_55, base + mc->flash_addr_even + 4);
			jtag_write_word (mc->flash_cmd_80, base + mc->flash_addr_odd + 4);
			jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd + 4);
			jtag_write_word (mc->flash_cmd_55, base + mc->flash_addr_even + 4);
			jtag_write_word (mc->flash_cmd_10, base + mc->flash_addr_odd + 4);
		}
	}
	for (;;) {
		fflush (stdout);
		jtag_usleep (250000);
		word = jtag_read_word (base);
		if (word == 0xffffffff)
			break;
		printf (".");
	}
	jtag_usleep (250000);
	printf (" done\n");
#endif
	return 1;
}

void multicore_flash_write (multicore_t *mc, unsigned addr, unsigned word)
{
#if 0
	unsigned base;

	base = compute_base (mc, addr);
	if (mc->flash_width == 8) {
		/* 8-разрядная шина. */
		/* Unlock bypass. */
		jtag_write_byte (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_byte (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_byte (0x20, base + mc->flash_addr_odd);

		/* Program. */
		jtag_write_byte (mc->flash_cmd_a0, base);
		jtag_write_byte (word, addr);
		jtag_write_byte (mc->flash_cmd_a0, base);
		jtag_write_byte (word >> 8, addr + 1);
		jtag_write_byte (mc->flash_cmd_a0, base);
		jtag_write_byte (word >> 16, addr + 2);
		jtag_write_byte (mc->flash_cmd_a0, base);
		jtag_write_byte (word >> 24, addr + 3);

		/* Reset unlock bypass. */
		jtag_write_byte (mc->flash_cmd_90, base);
		jtag_write_byte (0x00, base);
	} else {
		if (mc->flash_width == 64 && (addr & 4)) {
			/* Старшая половина 64-разрядной шины. */
			base += 4;
		}
		jtag_write_word (mc->flash_cmd_aa, base + mc->flash_addr_odd);
		jtag_write_next (mc->flash_cmd_55, base + mc->flash_addr_even);
		jtag_write_next (mc->flash_cmd_a0, base + mc->flash_addr_odd);
		jtag_write_next (word, addr);
	}
#endif
}

int multicore_flash_rewrite (multicore_t *mc, unsigned addr, unsigned word)
{
#if 0
	unsigned bad, base;
	unsigned char byte;

	/* Повторная запись реализована только для 8-битной flash-памяти. */
	if (mc->flash_width != 8)
		return 0;

	/* Повтор записи возможен, только если не прописались нули. */
	jtag_read_start ();
	bad = jtag_read_next (addr);
	if ((bad & word) != word)
		return 0;

	/* Вычисляем нужный байт. */
	for (bad &= ~word; ! (bad & 0xFF); bad >>= 8) {
		addr++;
		word >>= 8;
	}
	byte = word;
/*fprintf (stderr, "write byte %02x to %08x\n", byte, addr);*/

	base = compute_base (mc, addr);
	jtag_write_byte (mc->flash_cmd_aa, base + mc->flash_addr_odd);
	jtag_write_byte (mc->flash_cmd_55, base + mc->flash_addr_even);
	jtag_write_byte (mc->flash_cmd_a0, base + mc->flash_addr_odd);
	jtag_write_byte (byte, addr);
	jtag_usleep (50000);
#endif
	return 1;
}

void multicore_read_start (multicore_t *mc)
{
#if 0
	jtag_read_start ();
#endif
}

unsigned multicore_read_next (multicore_t *mc, unsigned addr)
{
#if 0
	return jtag_read_next (addr);
#else
	return 0;
#endif
}

void multicore_write_word (multicore_t *mc, unsigned addr, unsigned word)
{
#if 0
	if (debug)
		fprintf (stderr, "write word %08x to %08x\n", word, addr);
	jtag_write_word (word, addr);
#endif
}

void multicore_write_next (multicore_t *mc, unsigned addr, unsigned word)
{
#if 0
	jtag_write_next (word, addr);
#endif
}
