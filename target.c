/*
 * Интерфейс через JTAG к процессору Элвис Мультикор.
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
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "target.h"
#include "adapter.h"
#include "oncd.h"

#define NFLASH		16	/* Max flash regions. */

struct _target_t {
	adapter_t	*adapter;
	char		*cpu_name;
	unsigned 	idcode;
	unsigned	cscon3;		/* Регистр конфигурации flash-памяти */
	unsigned	flash_width;
	unsigned	flash_bytes;
	unsigned	flash_addr_odd;
	unsigned	flash_addr_even;
	unsigned	flash_cmd_aa;
	unsigned	flash_cmd_55;
	unsigned	flash_cmd_10;
	unsigned	flash_cmd_20;
	unsigned	flash_cmd_80;
	unsigned	flash_cmd_90;
	unsigned	flash_cmd_a0;
	unsigned	flash_cmd_f0;
	unsigned	flash_devid_offset;
	unsigned	flash_base [NFLASH];
	unsigned	flash_last [NFLASH];
	unsigned	flash_delay;
};

/* Идентификатор производителя flash. */
#define ID_ALLIANCE		0x00520052
#define ID_AMD			0x00010001
#define ID_SST			0x00BF00BF
#define ID_MILANDR		0x01010101
#define ID_ANGSTREM		0xBFBFBFBF
#define ID_SPANSION		0x01010101

/* Идентификатор микросхемы flash. */
#define ID_29LV800_B		0x225b225b
#define ID_29LV800_T		0x22da22da
#define ID_39VF800_A		0x27812781
#define ID_39VF6401_B		0x236d236d
#define ID_1636PP2Y		0xc8c8c8c8
#define ID_1638PP1		0x07070707
#define ID_S29AL032D		0x000000f9

/* Команды flash. */
#define FLASH_CMD16_AA		0x00AA00AA
#define FLASH_CMD8_AA		0xAAAAAAAA
#define FLASH_CMD16_55		0x00550055
#define FLASH_CMD8_55		0x55555555
#define FLASH_CMD16_10		0x00100010	/* Chip erase 2/2 */
#define FLASH_CMD8_10		0x10101010
#define FLASH_CMD16_20		0x00200020	/* Unlock bypass */
#define FLASH_CMD8_20		0x20202020
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
#define MC12REV2_ID		0x40777001

#define MC_CSCON3		0x182F100C
#define MC_CSCON3_ADDR(addr)	((addr & 3) << 20)

#if defined (__CYGWIN32__) || defined (MINGW32)
/*
 * Задержка в миллисекундах: Windows.
 */
#include <windows.h>

void mdelay (unsigned msec)
{
	Sleep (msec);
}
#else
/*
 * Задержка в миллисекундах: Unix.
 */
void mdelay (unsigned msec)
{
	usleep (msec * 1000);
}
#endif

/*
 * Выполнение одной инструкции MIPS32.
 */
static void target_exec (target_t *t, unsigned instr)
{
	/* Restore PCfetch to right address or
	 * we can go in exception. */
	t->adapter->oncd_write (t->adapter,
		0xBFC00000, OnCD_PCfetch, 32);

	/* Supply instruction to pipeline and do step */
	t->adapter->oncd_write (t->adapter,
		instr, OnCD_IRdec, 32);
	t->adapter->oncd_write (t->adapter,
		0, OnCD_EXIT | IRd_FLUSH_PIPE | IRd_STEP_1CLK, 0);
}

/*
 * Запись слова в память.
 */
void target_write_next (target_t *t, unsigned phys_addr, unsigned data)
{
	unsigned count, oscr;

	if (phys_addr >= 0xA0000000)
		phys_addr -= 0xA0000000;
	else if (phys_addr >= 0x80000000)
		phys_addr -= 0x80000000;

	if (debug)
		fprintf (stderr, "write %08x to %08x\n", data, phys_addr);
	t->adapter->oncd_write (t->adapter, phys_addr, OnCD_OMAR, 32);
	t->adapter->oncd_write (t->adapter, data, OnCD_OMDR, 32);
	t->adapter->oncd_write (t->adapter, 0, OnCD_MEM, 0);

	for (count = 1000; count != 0; count--) {
		oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
		if (oscr & OSCR_RDYm)
			break;
		mdelay (1);
	}
	if (count == 0) {
		fprintf (stderr, "Timeout writing memory, aborted.\n");
		exit (1);
	}
}

void target_write_word (target_t *t, unsigned phys_addr, unsigned data)
{
	unsigned oscr;

	if (debug)
		fprintf (stderr, "write word %08x to %08x\n",
			data, phys_addr);

	/* Allow memory access */
	oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
	oscr |= OSCR_SlctMEM;
	oscr &= ~OSCR_RO;
	t->adapter->oncd_write (t->adapter, oscr, OnCD_OSCR, 32);

	target_write_next (t, phys_addr, data);
}

/*
 * Чтение слова из памяти.
 */
void target_read_start (target_t *t)
{
	unsigned oscr;

	/* Allow memory access */
	oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
	oscr |= OSCR_SlctMEM | OSCR_RO;
	t->adapter->oncd_write (t->adapter, oscr, OnCD_OSCR, 32);
}

unsigned target_read_next (target_t *t, unsigned phys_addr)
{
	unsigned count, oscr, data;

	if (phys_addr >= 0xA0000000)
		phys_addr -= 0xA0000000;
	else if (phys_addr >= 0x80000000)
		phys_addr -= 0x80000000;

	t->adapter->oncd_write (t->adapter, phys_addr, OnCD_OMAR, 32);
	t->adapter->oncd_write (t->adapter, 0, OnCD_MEM, 0);
	for (count = 100; count != 0; count--) {
		oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
		if (oscr & OSCR_RDYm)
			break;
		mdelay (1);
	}
	if (count == 0) {
		fprintf (stderr, "Timeout reading memory, aborted.\n");
		exit (1);
	}
	data = t->adapter->oncd_read (t->adapter, OnCD_OMDR, 32);
	if (debug)
		fprintf (stderr, "read %08x from     %08x\n", data, phys_addr);
	return data;
}

unsigned target_read_word (target_t *t, unsigned phys_addr)
{
	target_read_start (t);
	return target_read_next (t, phys_addr);
}

void target_write_nwords (target_t *t, unsigned nwords, ...)
{
	va_list args;
	unsigned addr, data, i;

	va_start (args, nwords);
	if (t->adapter->write_nwords) {
		t->adapter->write_nwords (t->adapter, nwords, args);
		va_end (args);
		return;
	}
	addr = va_arg (args, unsigned);
	data = va_arg (args, unsigned);
	target_write_word (t, addr, data);
	for (i=1; i<nwords; i++) {
		addr = va_arg (args, unsigned);
		data = va_arg (args, unsigned);
		target_write_next (t, addr, data);
	}
	va_end (args);
}

void target_write_byte (target_t *t, unsigned addr, unsigned data)
{
	if (t->adapter->write_nwords) {
		target_write_nwords (t, 2,
			MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr),
			addr, data);
		return;
	}
	target_write_word (t, MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr));
	target_write_next (t, addr, data);
}

void target_write_2bytes (target_t *t, unsigned addr1, unsigned data1,
	unsigned addr2, unsigned data2)
{
	if (t->adapter->write_nwords) {
		target_write_nwords (t, 4,
			MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr1),
			addr1, data1,
			MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr2),
			addr2, data2);
		return;
	}
	target_write_word (t, MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr1));
	target_write_next (t, addr1, data1);
	target_write_next (t, MC_CSCON3, t->cscon3 | MC_CSCON3_ADDR (addr2));
	target_write_next (t, addr2, data2);
}

/*
 * Open the device.
 */
target_t *target_open ()
{
	target_t *t;

	t = calloc (1, sizeof (target_t));
	if (! t) {
		fprintf (stderr, "Out of memory\n");
		exit (-1);
	}
	t->cpu_name = "Unknown";
	t->flash_base[0] = ~0;
	t->flash_last[0] = ~0;

	/* Ищем адаптер JTAG: USB, bitbang, MPSSE или LPT. */
	t->adapter = adapter_open_usb ();
	if (! t->adapter)
		t->adapter = adapter_open_mpsse ();
	if (! t->adapter)
		t->adapter = adapter_open_bitbang ();
	if (! t->adapter)
		t->adapter = adapter_open_lpt ();
	if (! t->adapter) {
		fprintf (stderr, "No JTAG adapter found.\n");
		exit (-1);
	}

	/* Проверяем идентификатор процессора. */
	t->idcode = t->adapter->get_idcode (t->adapter);
	if (debug)
		fprintf (stderr, "idcode %08X\n", t->idcode);
	switch (t->idcode) {
	default:
		/* Device not detected. */
		if (t->idcode == 0xffffffff || t->idcode == 0)
			fprintf (stderr, "No response from device -- check power is on!\n");
		else
			fprintf (stderr, "No response from device -- unknown idcode 0x%08X!\n",
				t->idcode);
		t->adapter->close (t->adapter);
		exit (1);
	case MC12_ID:
		t->cpu_name = "MC12";
		break;
	case MC12REV1_ID:
		t->cpu_name = "MC12r1";
		break;
	case MC12REV2_ID:
		t->cpu_name = "MC12r2";
		break;
	}
	t->adapter->stop_cpu (t->adapter);
	t->cscon3 = target_read_word (t, MC_CSCON3) & ~MC_CSCON3_ADDR (3);
	return t;
}

/*
 * Close the device.
 */
void target_close (target_t *t)
{
	unsigned oscr;
	int i;
#if 0
	/* Clear processor state */
	for (i=1; i<32; i++) {
		/* add $i, $0, $0 */
		target_exec (t, 0x20 | (i << 11));
	}
#endif
	/* Clear pipeline */
	for (i=0; i<3; i++) {
		/* add $0, $0, $0 */
		target_exec (t, 0x20);
	}

	/* Setup IRdec and PCfetch */
	t->adapter->oncd_write (t->adapter, 0xBFC00000, OnCD_PCfetch, 32);
	t->adapter->oncd_write (t->adapter, 0x20, OnCD_IRdec, 32);

	/* Flush CPU pipeline at exit */
	oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
	oscr &= ~(OSCR_TME | OSCR_IME | OSCR_SlctMEM | OSCR_RDYm);
	oscr |= OSCR_MPE;
	t->adapter->oncd_write (t->adapter, oscr, OnCD_OSCR, 32);

	/* Exit */
	t->adapter->oncd_write (t->adapter,
		0, OnCD_EXIT | IRd_FLUSH_PIPE | IRd_RUN, 0);

	t->adapter->close (t->adapter);
}

/*
 * Add a flash region.
 */
void target_flash_configure (target_t *t, unsigned first, unsigned last)
{
	int i;

	for (i=0; i<NFLASH-1; ++i) {
		if (t->flash_last [i] == ~0) {
			t->flash_base [i] = first;
			t->flash_last [i] = last;
			t->flash_base [i+1] = ~0;
			t->flash_last [i+1] = ~0;
			return;
		}
	}
	fprintf (stderr, "target_flash_configure: too many flash regions.\n");
	exit (1);
}

/*
 * Iterate through all flash regions.
 */
unsigned target_flash_next (target_t *t, unsigned prev, unsigned *last)
{
	int i;

	if (prev == ~0 && t->flash_base [0] != ~0) {
		*last = t->flash_last [0];
		return t->flash_base [0];
	}
	for (i=1; i<NFLASH-1 && t->flash_last[i] != ~0; ++i) {
		if (prev >= t->flash_base [i-1] &&
		    prev <= t->flash_last [i-1]) {
			*last = t->flash_last [i];
			return t->flash_base [i];
		}
	}
	return ~0;
}

char *target_cpu_name (target_t *t)
{
	return t->cpu_name;
}

unsigned target_idcode (target_t *t)
{
	return t->idcode;
}

unsigned target_flash_width (target_t *t)
{
	return t->flash_width;
}

/*
 * Вычисление базового адреса микросхемы flash-памяти.
 */
static unsigned compute_base (target_t *t, unsigned addr)
{
	int i;

	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	for (i=0; i<NFLASH && t->flash_last[i]; ++i) {
		if (addr >= t->flash_base [i] &&
		    addr <= t->flash_last [i])
			return t->flash_base [i];
	}
	fprintf (stderr, "target: no flash region for address 0x%08X\n", addr);
	exit (1);
	return 0;
}

int target_flash_detect (target_t *t, unsigned addr,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width)
{
	int count;
	unsigned base;

	base = compute_base (t, addr);
	for (count=0; count<4*6; ++count) {
		/* Try both 32 and 64 bus width.*/
		switch (count % 6) {
		case 0:
			/* Two 16-bit flash chips. */
			t->flash_width = 32;
			t->flash_addr_odd = 0x5555 << 2;
			t->flash_addr_even = 0x2AAA << 2;
			t->flash_cmd_aa = FLASH_CMD16_AA;
			t->flash_cmd_55 = FLASH_CMD16_55;
			t->flash_cmd_10 = FLASH_CMD16_10;
			t->flash_cmd_20 = FLASH_CMD16_20;
			t->flash_cmd_80 = FLASH_CMD16_80;
			t->flash_cmd_90 = FLASH_CMD16_90;
			t->flash_cmd_a0 = FLASH_CMD16_A0;
			t->flash_cmd_f0 = FLASH_CMD16_F0;
			t->flash_devid_offset = 4;
			break;
		case 1:
			/* Four 16-bit flash chips. */
			t->flash_width = 64;
			t->flash_addr_odd = 0x5555 << 3;
			t->flash_addr_even = 0x2AAA << 3;
			t->flash_cmd_aa = FLASH_CMD16_AA;
			t->flash_cmd_55 = FLASH_CMD16_55;
			t->flash_cmd_10 = FLASH_CMD16_10;
			t->flash_cmd_20 = FLASH_CMD16_20;
			t->flash_cmd_80 = FLASH_CMD16_80;
			t->flash_cmd_90 = FLASH_CMD16_90;
			t->flash_cmd_a0 = FLASH_CMD16_A0;
			t->flash_cmd_f0 = FLASH_CMD16_F0;
			t->flash_devid_offset = 8;
			break;
		case 2:
			/* Four 8-bit flash chips, SST/Milandr. */
			t->flash_width = 32;
			t->flash_addr_odd = 0x555 << 2;
			t->flash_addr_even = 0x2AA << 2;
			t->flash_cmd_aa = FLASH_CMD8_AA;
			t->flash_cmd_55 = FLASH_CMD8_55;
			t->flash_cmd_10 = FLASH_CMD8_10;
			t->flash_cmd_20 = FLASH_CMD8_20;
			t->flash_cmd_80 = FLASH_CMD8_80;
			t->flash_cmd_90 = FLASH_CMD8_90;
			t->flash_cmd_a0 = FLASH_CMD8_A0;
			t->flash_cmd_f0 = FLASH_CMD8_F0;
			t->flash_devid_offset = 4;
			break;
		case 3:
			/* One 8-bit flash chip. */
			t->flash_width = 8;
			t->flash_addr_odd = 0x555;
			t->flash_addr_even = 0x2AA;
			t->flash_cmd_aa = FLASH_CMD8_AA;
			t->flash_cmd_55 = FLASH_CMD8_55;
			t->flash_cmd_10 = FLASH_CMD8_10;
			t->flash_cmd_20 = FLASH_CMD8_20;
			t->flash_cmd_80 = FLASH_CMD8_80;
			t->flash_cmd_90 = FLASH_CMD8_90;
			t->flash_cmd_a0 = FLASH_CMD8_A0;
			t->flash_cmd_f0 = FLASH_CMD8_F0;
			t->flash_devid_offset = 0;
			break;
		case 4:
			/* Four 8-bit flash chips, Atmel/Angstrem. */
			t->flash_width = 32;
			t->flash_addr_odd = 0x5555 << 2;
			t->flash_addr_even = 0x2AAA << 2;
			t->flash_cmd_aa = FLASH_CMD8_AA;
			t->flash_cmd_55 = FLASH_CMD8_55;
			t->flash_cmd_10 = FLASH_CMD8_10;
			t->flash_cmd_20 = FLASH_CMD8_20;
			t->flash_cmd_80 = FLASH_CMD8_80;
			t->flash_cmd_90 = FLASH_CMD8_90;
			t->flash_cmd_a0 = FLASH_CMD8_A0;
			t->flash_cmd_f0 = FLASH_CMD8_F0;
			t->flash_devid_offset = 4;
			t->flash_delay = 20;
			break;
		case 5:
			/* One 8-bit flash chip. */
			t->flash_width = 8;
			t->flash_addr_odd = 0xAAA;
			t->flash_addr_even = 0x555;
			t->flash_cmd_aa = FLASH_CMD8_AA;
			t->flash_cmd_55 = FLASH_CMD8_55;
			t->flash_cmd_10 = FLASH_CMD8_10;
			t->flash_cmd_20 = FLASH_CMD8_20;
			t->flash_cmd_80 = FLASH_CMD8_80;
			t->flash_cmd_90 = FLASH_CMD8_90;
			t->flash_cmd_a0 = FLASH_CMD8_A0;
			t->flash_cmd_f0 = FLASH_CMD8_F0;
			t->flash_devid_offset = 0;
			break;
		default:
			continue;
		}
		/* Read device code. */
		if (t->flash_width == 8) {
			/* Byte-wide data bus. */
			target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_aa);
			target_write_byte (t, base + t->flash_addr_even, t->flash_cmd_55);
			target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_90);
			*mf = target_read_word (t, base);
			if ((count%6)==5) {
				*dev = (unsigned char) (*mf >> 16);
				*mf = (unsigned char) *mf;
			} else {
				*dev = (unsigned char) (*mf >> 8);
				*mf = (unsigned char) *mf;
			};
			/* Stop read ID mode. */
			target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_aa);
			target_write_byte (t, base + t->flash_addr_even, t->flash_cmd_55);
			target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_f0);

		} else if (t->flash_delay) {
			/* Word-wide data bus. */
			mdelay (t->flash_delay);
			target_write_nwords (t, 3,
				base + t->flash_addr_odd, t->flash_cmd_aa,
				base + t->flash_addr_even, t->flash_cmd_55,
				base + t->flash_addr_odd, t->flash_cmd_90);
			mdelay (t->flash_delay);
			*mf = target_read_word (t, base);
			*dev = target_read_word (t, base + t->flash_devid_offset);
			//printf ("base = %08X, dev = %08X, mf = %08X\n", base, *dev, *mf);

			/* Stop read ID mode. */
			target_write_nwords (t, 3,
				base + t->flash_addr_odd, t->flash_cmd_aa,
				base + t->flash_addr_even, t->flash_cmd_55,
				base + t->flash_addr_odd, t->flash_cmd_f0);
			mdelay (t->flash_delay);
		} else {
			/* Word-wide data bus. */
			target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_aa);
			target_write_word (t, base + t->flash_addr_even, t->flash_cmd_55);
			target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_90);
			*dev = target_read_word (t, base + t->flash_devid_offset);
			*mf = target_read_word (t, base);

			/* Stop read ID mode. */
			target_write_nwords (t, 3,
				base + t->flash_addr_odd, t->flash_cmd_aa,
				base + t->flash_addr_even, t->flash_cmd_55,
				base + t->flash_addr_odd, t->flash_cmd_f0);
		}

		if (debug > 1)
			fprintf (stderr, "flash id %08X\n", *dev);
		switch (*dev) {
		case ID_29LV800_B:
			strcpy (devname, "29LV800B");
			t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
			goto success;
		case ID_29LV800_T:
			strcpy (devname, "29LV800T");
			t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
			goto success;
		case ID_39VF800_A:
			strcpy (devname, "39VF800A");
			t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
			goto success;
		case ID_39VF6401_B:
			strcpy (devname, "39VF6401B");
			t->flash_bytes = 16*1024*1024 * t->flash_width / 32;
			goto success;
		case ID_1636PP2Y:
			strcpy (devname, "1636PP2Y");
			t->flash_bytes = 4*2*1024*1024;
			goto success;
		case ID_1638PP1:
			strcpy (devname, "1638PP1");
			t->flash_bytes = 4*128*1024;
			goto success;
		case (unsigned char) ID_1636PP2Y:
			if (t->flash_width != 8)
				break;
			strcpy (devname, "1636PP2Y");
			t->flash_bytes = 2*1024*1024;
			goto success;
		case ID_S29AL032D:
			strcpy (devname, "S29AL032D");
			t->flash_bytes = 4*1024*1024;
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
	case ID_ANGSTREM:
		strcpy (mfname, "Angstrem");
		break;
	/* Milandr & Spansion mf code =0x01 */
	case (unsigned char) ID_MILANDR:
		if (t->flash_width != 8)
			goto unknown_mfr;
		if (*dev==ID_S29AL032D) strcpy (mfname, "Spansion");
		else strcpy (mfname, "Milandr");
		break;
	default:
unknown_mfr:	sprintf (mfname, "<%08X>", *mf);
		break;
	}

	*bytes = t->flash_bytes;
	*width = t->flash_width;
	return 1;
}

int target_erase (target_t *t, unsigned addr)
{
	unsigned word, base;

	/* Chip erase. */
	base = compute_base (t, addr);
	printf ("Erase: %08X", base);
	if (t->flash_width == 8) {
		/* 8-разрядная шина. */
		target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_aa);
		target_write_byte (t, base + t->flash_addr_even, t->flash_cmd_55);
		target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_80);
		target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_aa);
		target_write_byte (t, base + t->flash_addr_even, t->flash_cmd_55);
		target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_10);

	} else if (t->flash_delay) {
		target_write_nwords (t, 6,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_80,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_10);
	} else {
		target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_aa);
		target_write_word (t, base + t->flash_addr_even, t->flash_cmd_55);
		target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_80);
		target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_aa);
		target_write_word (t, base + t->flash_addr_even, t->flash_cmd_55);
		target_write_word (t, base + t->flash_addr_odd, t->flash_cmd_10);
		if (t->flash_width == 64) {
			/* Старшая половина 64-разрядной шины. */
			target_write_word (t, base + t->flash_addr_odd + 4, t->flash_cmd_aa);
			target_write_word (t, base + t->flash_addr_even + 4, t->flash_cmd_55);
			target_write_word (t, base + t->flash_addr_odd + 4, t->flash_cmd_80);
			target_write_word (t, base + t->flash_addr_odd + 4, t->flash_cmd_aa);
			target_write_word (t, base + t->flash_addr_even + 4, t->flash_cmd_55);
			target_write_word (t, base + t->flash_addr_odd + 4, t->flash_cmd_10);
		}
	}
	for (;;) {
		fflush (stdout);
		mdelay (250);
		word = target_read_word (t, base);
		if (word == 0xffffffff)
			break;
		printf (".");
	}
	mdelay (250);
	printf (" done\n");
	return 1;
}

/*
 * Повторная запись реализована только для 8-битной flash-памяти.
 */
int target_flash_rewrite (target_t *t, unsigned addr, unsigned word)
{
	unsigned bad, base;
	unsigned char byte;

	base = compute_base (t, addr);
	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	/* Повтор записи возможен, только если не прописались нули. */
	bad = target_read_word (t, addr);
	if ((bad & word) != word) {
		fprintf (stderr, "target: cannot rewrite word at %x\n",
			addr);
//fprintf (stderr, "bad = %08x, expected = %08x\n", bad, word);
		exit (1);
	}

	switch (t->flash_width) {
	case 8:
		/* Вычисляем нужный байт. */
		for (bad &= ~word; ! (bad & 0xFF); bad >>= 8) {
			addr++;
			word >>= 8;
		}
		byte = word;
//fprintf (stderr, "\nrewrite byte %02x at %08x ", byte, addr); fflush (stderr);

		target_write_2bytes (t,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55);
		target_write_2bytes (t,
			base + t->flash_addr_odd, t->flash_cmd_a0,
			addr, byte);
		break;
	case 64:
		base += addr & 4;
		/* fall through...*/
	case 32:
		if (t->flash_delay)
			return 0;
fprintf (stderr, "\nrewrite word %02x at %08x ", word, addr); fflush (stderr);
		target_write_nwords (t, 4,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_a0,
			addr, word);
		break;
	}
	return 1;
}

void target_read_block (target_t *t, unsigned addr,
	unsigned nwords, unsigned *data)
{
	unsigned i;

	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	if (t->adapter->read_block) {
		while (nwords > 0) {
			unsigned n = nwords;
			if (n > 64)
				n = 64;
			t->adapter->read_block (t->adapter, n, addr, data);
			data += n;
			addr += n*4;
			nwords -= n;
		}
		return;
	}
	target_read_start (t);
	for (i=0; i<nwords; i++, addr+=4)
		*data++ = target_read_next (t, addr);
}

void target_write_block (target_t *t, unsigned addr,
	unsigned nwords, unsigned *data)
{
	unsigned i;

	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	if (t->adapter->write_block) {
		while (nwords > 0) {
			unsigned n = nwords;
			if (n > 64)
				n = 64;
			t->adapter->write_block (t->adapter, n, addr, data);
			data += n;
			addr += n*4;
			nwords -= n;
		}
		return;
	}
	target_write_word (t, addr, *data++);
	for (i=1; i<nwords; i++)
		target_write_next (t, addr += 4, *data++);
}

static void target_program_block8 (target_t *t, unsigned addr,
	unsigned base, unsigned nwords, unsigned *data)
{
	/* Unlock bypass. */
	target_write_2bytes (t, base + t->flash_addr_odd, t->flash_cmd_aa,
		base + t->flash_addr_even, t->flash_cmd_55);
	target_write_byte (t, base + t->flash_addr_odd, t->flash_cmd_20);
	while (nwords-- > 0) {
		unsigned word = *data++;
		target_write_2bytes (t, base, t->flash_cmd_a0, addr++, word);
		target_write_2bytes (t, base, t->flash_cmd_a0, addr++, word >> 8);
		target_write_2bytes (t, base, t->flash_cmd_a0, addr++, word >> 16);
		target_write_2bytes (t, base, t->flash_cmd_a0, addr++, word >> 24);
	}
	/* Reset unlock bypass. */
	target_write_2bytes (t, base, t->flash_cmd_90, base, 0);
}

static void target_program_block32 (target_t *t, unsigned addr,
	unsigned base, unsigned nwords, unsigned *data)
{
	if (t->adapter->program_block32) {
		while (nwords > 0) {
			unsigned n = nwords;
			if (n > 16)
				n = 16;
			t->adapter->program_block32 (t->adapter,
				n, base, addr, data,
				t->flash_addr_odd, t->flash_addr_even,
				t->flash_cmd_aa, t->flash_cmd_55, t->flash_cmd_a0);
			data += n;
			addr += n*4;
			nwords -= n;
		}
		return;
	}
	while (nwords-- > 0) {
		target_write_nwords (t, 4,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_a0,
			addr, *data++);
		addr += 4;
	}
}

static void target_program_block32_atmel (target_t *t, unsigned addr,
	unsigned base, unsigned nwords, unsigned *data)
{
	if (t->adapter->program_block32_protect) {
		while (nwords > 0) {
			t->adapter->program_block32_unprotect (t->adapter,
				128, base, addr, data,
				t->flash_addr_odd, t->flash_addr_even,
				t->flash_cmd_aa, t->flash_cmd_55, t->flash_cmd_a0);
			t->adapter->program_block32_protect (t->adapter,
				128, base, addr, data,
				t->flash_addr_odd, t->flash_addr_even,
				t->flash_cmd_aa, t->flash_cmd_55, t->flash_cmd_a0);
			if (nwords <= 128)
				break;
			data += 128;
			addr += 128*4;
			nwords -= 128;
		}
		return;
	}
	while (nwords > 0) {
		/* Unprotect. */
		target_write_nwords (t, 6,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, 0x80808080,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, 0x20202020);
		target_write_block (t, addr, 128, data);

		/* Protect. */
		target_write_nwords (t, 3,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_a0);
		target_write_block (t, addr, 128, data);

		data += 128;
		addr += 128*4;
		nwords -= 128;
	}
}

static void target_program_block64 (target_t *t, unsigned addr,
	unsigned base, unsigned nwords, unsigned *data)
{
	if (t->adapter->program_block32) {
		while (nwords > 0) {
			unsigned n = nwords;
			if (n > 16)
				n = 16;
			t->adapter->program_block64 (t->adapter,
				n, base, addr, data,
				t->flash_addr_odd, t->flash_addr_even,
				t->flash_cmd_aa, t->flash_cmd_55, t->flash_cmd_a0);
			data += n;
			addr += n*4;
			nwords -= n;
		}
		return;
	}
	if (addr & 4) {
		/* Старшая половина 64-разрядной шины. */
		base += 4;
	}
	while (nwords-- > 0) {
		target_write_nwords (t, 4,
			base + t->flash_addr_odd, t->flash_cmd_aa,
			base + t->flash_addr_even, t->flash_cmd_55,
			base + t->flash_addr_odd, t->flash_cmd_a0,
			addr, *data++);
		addr += 4;
	}
}

void target_program_block (target_t *t, unsigned addr,
	unsigned nwords, unsigned *data)
{
	unsigned base;

	base = compute_base (t, addr);
	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;
//printf ("target_program_block (addr = %x, nwords = %d), flash_width = %d, base = %x\n", addr, nwords, t->flash_width, base);

	switch (t->flash_width) {
	case 8:
		/* 8-разрядная шина. */
		target_program_block8 (t, addr, base, nwords, data);
		break;
	case 32:
		if (t->flash_delay) {
			target_program_block32_atmel (t,
				addr, base, nwords, data);
		} else {
			target_program_block32 (t, addr, base, nwords, data);
		}
		break;
	case 64:
		/* 64-разрядная шина. */
		target_program_block64 (t, addr, base, nwords, data);
		break;
	}
}

#define BLOCK_MEM	16*1024

int check_clean (target_t *t, unsigned addr)
{
	unsigned base, offset, i, sz, end, n;
	unsigned *mem;

	n=0;
	mem = malloc(sizeof(unsigned)*BLOCK_MEM);
	/* Check chip clean. */
	base = compute_base (t, addr);
	end = base + t->flash_bytes;
	for (offset=0;(base+offset)<end;offset+=sizeof(unsigned)*BLOCK_MEM) {
		if ((sizeof(unsigned)*BLOCK_MEM+offset)<end) sz = BLOCK_MEM;
		else sz = (end-offset)/sizeof(unsigned);
		target_read_block(t, base+offset, sz, mem);
		for (i=0;i<sz;i++) {
			if (mem[i] != 0xffffffff) {
				free(mem);
				return(0);
			};
		};
	};
	free(mem);
	printf ("Clean flash: %08X\n", base);
	return(1);
};
