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
};

static struct libusb_device_handle *usbdev;

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
#define BULK_CONTROL_ENDPOINT	4
#define BULK_READ_ENDPOINT	0x86

#define ADAPTER_ACTIVE_RESET 0x4
#define ADAPTER_DEACTIVE_RESET 0x5

#define ADAPTER_PLL_12MHZ 0x1
#define ADAPTER_PLL_24MHZ 0x2
#define ADAPTER_PLL_48MHZ 0x3

#define HDR_NORM        0x4c    // Заголовок обычного пакета (32-бита)
#define HDR_NORM_16     0x5c    // Заголовок обычного пакета (16-бит)
#define HDR_NORM_12     0x6c    // Заголовок обычного пакета (12-бит)

#define HDR_WR          0x45    // Заголовок неблочной записи
#define HDR_RD          0x46    // Заголовок неблочного чтения
#define HDR_END         0x47    // Терминирующий (для неблочных операций) заголовок

#define HDR_BLKWR       0x4d    // Заголовок блочной записи
#define HDR_BLKRD       0x4e    // Заголовок блочного чтения
#define HDR_BLKEND      0x4f    // Терминирующий (для блочных операций) заголовок

#if defined (__CYGWIN32__) || defined (MINGW32)
/*
 * Windows.
 */
#include <windows.h>

void jtag_usleep (unsigned usec)
{
	Sleep (usec / 1000);
}
#else
/*
 * Unix.
 */
void jtag_usleep (unsigned usec)
{
	usleep (usec);
}
#endif

/*
 * Записать через USB массив данных.
 */
static void bulk_write (const unsigned char *wb, unsigned wlen)
{
	int transferred;

	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_WRITE_ENDPOINT,
	    (unsigned char*) wb, wlen, &transferred, 1000) != 0 ||
	    transferred != wlen) {
		fprintf (stderr, "Bulk write failed: %d bytes to endpoint %#x.\n",
			wlen, BULK_WRITE_ENDPOINT);
		exit (-1);
	}
};

/*
 * Записать команду в Ctrl Pipe.
 */
static void bulk_cmd (unsigned char cmd)
{
	int transferred;

	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_CONTROL_ENDPOINT,
	    &cmd, 1, &transferred, 1000) != 0 ||
	    transferred != 1) {
		fprintf (stderr, "Bulk write failed: command to endpoint %#x.\n",
			BULK_CONTROL_ENDPOINT);
		exit (-1);
	}
};

/*
 * Записать и прочитать из USB массив данных.
 */
static unsigned bulk_write_read (const unsigned char *wb,
	unsigned wlen, unsigned char *rb, unsigned rlen)
{
	int transferred;

	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_WRITE_ENDPOINT,
	    (unsigned char*) wb, wlen, &transferred, 1000) != 0 ||
	    transferred != wlen) {
		fprintf (stderr, "Bulk write failed: %d bytes to endpoint %#x.\n",
			wlen, BULK_WRITE_ENDPOINT);
		exit (-1);
	}
	transferred = 0;
	if (libusb_bulk_transfer (usbdev, BULK_READ_ENDPOINT,
	    rb, rlen, &transferred, 1000) != 0) {
		fprintf (stderr, "Bulk read failed: %d/%d bytes from endpoint %#x.\n",
			transferred, rlen, BULK_READ_ENDPOINT);
		exit (-1);
	}
	return transferred;
};

/*
 * Приведение адаптера USB в исходное состояние.
 */
void jtag_start (void)
{
	const unsigned char pkt_reset[8] = { 0x34,0x4d };
	const unsigned char pkt_getver[8]= { 0xb0,0xff };
	unsigned char rb[32];

	usbdev = libusb_open_device_with_vid_pid (NULL, 0x0547, 0x1002);
	if (! usbdev) {
		fprintf (stderr, "USB-JTAG Multicore adapter not found.d\n");
		exit (-1);
	}
	bulk_cmd (ADAPTER_PLL_12MHZ);
	jtag_usleep (1000);
	bulk_cmd (ADAPTER_ACTIVE_RESET);
	jtag_usleep (1000);
	bulk_cmd (ADAPTER_DEACTIVE_RESET);
	jtag_usleep (1000);

	/* Сброс OnCD. */
	bulk_write_read (pkt_reset, 2, rb, 32);

	/* Получить версию прошивки. */
	if (bulk_write_read (pkt_getver, 2, rb, 8) != 2) {
		fprintf (stderr, "Failed to get adapter version.\n");
		exit (-1);
	}
	fprintf (stderr, "USB adapter version: %02x\n", *(unsigned short*) rb >> 8);
}

/*
 * Перевод кристалла в режим отладки путём манипуляций
 * регистрами данных JTAG.
 */
void jtag_reset ()
{
	const unsigned char pkt_debug_request_sysrst[8] = { 0xb8,0x44 };
	const unsigned char pkt_debug_enable[8] = { 0xbc,0x55 };
	unsigned char rb[8];
	unsigned retry;

	/* Запрос Debug request, сброс процессора. */
	for (retry=0; ; retry++) {
		if (bulk_write_read (pkt_debug_request_sysrst, 2, rb, 8) != 2) {
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
		if (bulk_write_read (pkt_debug_enable, 2, rb, 8) != 2) {
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
unsigned jtag_get_idcode (void)
{
	const unsigned char pkt_idcode[8] = { 0x4b, 0x03 };
	unsigned char rb [8];
	unsigned idcode;

	if (bulk_write_read (pkt_idcode, 6, rb, 8) != 4) {
		fprintf (stderr, "Failed to get IDCODE.\n");
		exit (-1);
	}
	idcode = *(unsigned*) rb;
	return idcode;
}

/*
 * Чтение 32-битного регистра OnCD.
 */
static unsigned oncd_read (int reg)
{
	static unsigned char pkt[8] = { HDR_NORM };
	unsigned val = 0;

	pkt [1] = reg | 0x40;
	if (bulk_write_read (pkt, 6, (unsigned char*) &val, 4) != 4) {
		fprintf (stderr, "Failed to read register.\n");
		exit (-1);
	}
//fprintf (stderr, "OnCD read %d -> %08x\n", reg, val);
	return val;
}

/*
 * Запись регистра OnCD.
 */
static void oncd_write (unsigned val, int reg)
{
	static unsigned char pkt[8] = { HDR_NORM };

//fprintf (stderr, "OnCD write %d := %08x\n", reg, val);
	pkt [1] = reg;
	memcpy (pkt+2, &val, 4);
	bulk_write (pkt, 6);
}

/*
 * Выполнение одной инструкции MIPS32.
 */
static void exec (unsigned instr)
{
	/* Restore PCfetch to right address or
	 * we can go in exception. */
	oncd_write (0xBFC00000, OnCD_PCfetch);

	/* Supply instruction to pipeline and do step */
	oncd_write (instr, OnCD_IRdec);
	oncd_write (0, OnCD_EXIT | 0xc0);
}

/*
 * Запись слова в память.
 */
void jtag_write_next (unsigned data, unsigned phys_addr)
{
	unsigned wait, oscr;

	if (phys_addr >= 0xA0000000)
		phys_addr -= 0xA0000000;
	else if (phys_addr >= 0x80000000)
		phys_addr -= 0x80000000;
//fprintf (stderr, "write %08x to %08x\n", data, phys_addr);
#if 1
	oncd_write (phys_addr, OnCD_OMAR);
	oncd_write (data, OnCD_OMDR);
	oncd_write (0, OnCD_MEM);
#else
	static unsigned char pkt[6*3];
	unsigned char *ptr = pkt;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMAR;
	*(unsigned*) ptr = phys_addr;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMDR;
	*(unsigned*) ptr = data;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_MEM;
	*(unsigned*) ptr = 0;
	*ptr += 4;

	bulk_write (pkt, ptr - pkt);
#endif
	for (wait = 100000; wait != 0; wait--) {
		oscr = oncd_read (OnCD_OSCR);
		if (oscr & OSCR_RDYm)
			break;
		jtag_usleep (10);
	}
	if (wait == 0) {
		fprintf (stderr, "Timeout writing memory, aborted.\n");
		exit (1);
	}
}

void jtag_write_word (unsigned data, unsigned phys_addr)
{
	unsigned oscr;

	/* Allow memory access */
	oscr = oncd_read (OnCD_OSCR);
	oscr |= OSCR_SlctMEM;
	oscr &= ~OSCR_RO;
	oncd_write (oscr, OnCD_OSCR);

	jtag_write_next (data, phys_addr);
}

void jtag_write_byte (unsigned data, unsigned phys_addr)
{
	jtag_write_word (cscon3 | MC_CSCON3_ADDR (phys_addr), MC_CSCON3);
	jtag_write_word (data, phys_addr);
}

void jtag_write_quad (unsigned data1, unsigned addr1,
	unsigned data2, unsigned addr2,
	unsigned data3, unsigned addr3,
	unsigned data4, unsigned addr4)
{
#if 0
	static unsigned char pkt[48];
	unsigned char *ptr = pkt;

	*ptr++ = HDR_BLKWR;
	*ptr++ = OnCD_OMAR;
	memcpy (ptr, &addr1, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKEND;
	*ptr++ = OnCD_OMDR;
	memcpy (ptr, &data1, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKWR;
	*ptr++ = OnCD_OMAR;
	memcpy (ptr, &addr2, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKEND;
	*ptr++ = OnCD_OMDR;
	memcpy (ptr, &data2, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKWR;
	*ptr++ = OnCD_OMAR;
	memcpy (ptr, &addr3, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKEND;
	*ptr++ = OnCD_OMDR;
	memcpy (ptr, &data3, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKWR;
	*ptr++ = OnCD_OMAR;
	memcpy (ptr, &addr4, 4);
	*ptr += 4;

	*ptr++ = HDR_BLKEND;
	*ptr++ = OnCD_OMDR;
	memcpy (ptr, &data4, 4);
	*ptr += 4;

	bulk_write (pkt, ptr - pkt);
#endif
#if 0
	static unsigned char pkt[6*3*4];
	unsigned char *ptr = pkt;

	/* Allow memory access */
	unsigned oscr = oncd_read (OnCD_OSCR);
	oscr |= OSCR_SlctMEM;
	oscr &= ~OSCR_RO;
	oncd_write (oscr, OnCD_OSCR);

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMAR;
	*(unsigned*) ptr = addr1;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMDR;
	*(unsigned*) ptr = data1;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_MEM;
	*(unsigned*) ptr = 0;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMAR;
	*(unsigned*) ptr = addr2;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMDR;
	*(unsigned*) ptr = data2;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_MEM;
	*(unsigned*) ptr = 0;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMAR;
	*(unsigned*) ptr = addr3;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMDR;
	*(unsigned*) ptr = data3;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_MEM;
	*(unsigned*) ptr = 0;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMAR;
	*(unsigned*) ptr = addr4;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_OMDR;
	*(unsigned*) ptr = data4;
	*ptr += 4;

	*ptr++ = HDR_NORM;
	*ptr++ = OnCD_MEM;
	*(unsigned*) ptr = 0;
	*ptr += 4;

	bulk_write (pkt, ptr - pkt);

	unsigned wait;
	for (wait = 100000; wait != 0; wait--) {
		oscr = oncd_read (OnCD_OSCR);
		if (oscr & OSCR_RDYm)
			break;
		jtag_usleep (10);
	}
	if (wait == 0) {
		fprintf (stderr, "Timeout writing memory, aborted.\n");
		exit (1);
	}
#endif
#if 1
	jtag_write_word (data1, addr1);
	jtag_write_next (data2, addr2);
	jtag_write_next (data3, addr3);
	jtag_write_next (data4, addr4);
#endif
}

/*
 * Чтение слова из памяти.
 */
void jtag_read_start ()
{
	unsigned oscr;

	/* Allow memory access */
	oscr = oncd_read (OnCD_OSCR);
	oscr |= OSCR_SlctMEM | OSCR_RO;
	oncd_write (oscr, OnCD_OSCR);
}

unsigned jtag_read_next (unsigned phys_addr)
{
	unsigned wait, oscr, data;

	if (phys_addr >= 0xA0000000)
		phys_addr -= 0xA0000000;
	else if (phys_addr >= 0x80000000)
		phys_addr -= 0x80000000;
//fprintf (stderr, "read %08x ", phys_addr); fflush (stderr);
	oncd_write (phys_addr, OnCD_OMAR);
	oncd_write (0, OnCD_MEM);
	for (wait = 100000; wait != 0; wait--) {
		oscr = oncd_read (OnCD_OSCR);
		if (oscr & OSCR_RDYm)
			break;
		jtag_usleep (10);
	}
	if (wait == 0) {
		fprintf (stderr, "Timeout reading memory, aborted.\n");
		exit (1);
	}
	data = oncd_read (OnCD_OMDR);
//fprintf (stderr, "-> %08x\n", data);
	return data;
}

unsigned jtag_read_word (unsigned phys_addr)
{
	jtag_read_start ();
	return jtag_read_next (phys_addr);
}

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

	mc = calloc (1, sizeof (multicore_t));
	if (! mc) {
		fprintf (stderr, "Out of memory\n");
		exit (-1);
	}
	mc->cpu_name = "Unknown";

	jtag_start ();

	/* For ARM7TDMI must be 0x1f0f0f0f. */
	mc->idcode = jtag_get_idcode();
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
		multicore_close (mc);
		exit (1);
	case MC12_ID:
		mc->cpu_name = "MC12";
		break;
	case MC12REV1_ID:
		mc->cpu_name = "MC12r1";
		break;
	}
	jtag_reset ();
	return mc;
}

/*
 * Close the device.
 */
void multicore_close (multicore_t *mc)
{
	unsigned oscr;
        int i;

	/* Clear processor state */
	for (i=1; i<32; i++) {
		/* add $i, $0, $0 */
		exec (0x20 | (i << 11));
	}
	/* Clear pipeline */
	for (i=0; i<3; i++) {
		/* add $0, $0, $0 */
		exec (0x20);
	}

	/* Setup IRdec and PCfetch */
	oncd_write (0xBFC00000, OnCD_PCfetch);
	oncd_write (0x20, OnCD_IRdec);

	/* Flush CPU pipeline at exit */
	oscr = oncd_read (OnCD_OSCR);
	oscr &= ~(OSCR_TME | OSCR_IME | OSCR_SlctMEM | OSCR_RDYm);
	oscr |= OSCR_MPE;
	oncd_write (oscr, OnCD_OSCR);

	/* Exit */
	oncd_write (0, OnCD_EXIT | 0x60);
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
	/* printf ("Unknown flash id = %08X\n", *dev); */
	return 0;
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
}

int multicore_erase (multicore_t *mc, unsigned addr)
{
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
	return 1;
}

void multicore_flash_write (multicore_t *mc, unsigned addr, unsigned word)
{
	unsigned base;

	base = compute_base (mc, addr);
	if (base >= 0xA0000000)
		base -= 0xA0000000;
	else if (base >= 0x80000000)
		base -= 0x80000000;

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
		jtag_write_quad (mc->flash_cmd_aa, base + mc->flash_addr_odd,
				mc->flash_cmd_55, base + mc->flash_addr_even,
				mc->flash_cmd_a0, base + mc->flash_addr_odd,
				word, addr);
	}
}

int multicore_flash_rewrite (multicore_t *mc, unsigned addr, unsigned word)
{
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
	return 1;
}

void multicore_read_start (multicore_t *mc)
{
	jtag_read_start ();
}

unsigned multicore_read_next (multicore_t *mc, unsigned addr)
{
	return jtag_read_next (addr);
}

void multicore_write_word (multicore_t *mc, unsigned addr, unsigned word)
{
	if (debug)
		fprintf (stderr, "write word %08x to %08x\n", word, addr);
	jtag_write_word (word, addr);
}

void multicore_write_next (multicore_t *mc, unsigned addr, unsigned word)
{
	jtag_write_next (word, addr);
}
