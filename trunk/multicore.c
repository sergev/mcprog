/*
 * Интерфейс через адаптер LPT-JTAG к процессору Элвис Мультикор.
 * Разработано в ИТМиВТ, 2006-2008.
 * Авторы: А.Ступаченко, С.Вакуленко.
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

#include "multicore.h"

struct _multicore_t {
	char		*cpu_name;
	unsigned 	idcode;
	unsigned	flash_width;
	unsigned	flash_bytes;
	unsigned	flash_addr_5555;
	unsigned	flash_addr_2aaa;
	unsigned	flash_devid_offset;
};

/* Идентификатор производителя flash. */
#define ID_ALLIANCE		0x00520052
#define ID_AMD			0x00010001
#define ID_SST			0x00BF00BF

/* Идентификатор микросхемы flash. */
#define ID_29LV800_B		0x225b225b
#define ID_29LV800_T		0x22da22da
#define ID_39VF800_A		0x27812781

/* Команды flash. */
#define FLASH_ADDR32_5555	(0x5555 << 2)
#define FLASH_ADDR32_2AAA	(0x2AAA << 2)
#define FLASH_ADDR64_5555	(0x5555 << 3)
#define FLASH_ADDR64_2AAA	(0x2AAA << 3)
#define FLASH_CMD_AA		0x00AA00AA
#define FLASH_CMD_55		0x00550055
#define FLASH_CMD_10		0x00100010
#define FLASH_CMD_80		0x00800080
#define FLASH_CMD_90		0x00900090
#define FLASH_CMD_A0		0x00A000A0
#define FLASH_CMD_F0		0x00F000F0

/* Идентификатор версии процессора. */
#define MC12_ID			0x20777001
#define MC12REV1_ID		0x30777001

/* JTAG TAP Commands */
#define	EXTEST		0x0
#define	SAMPLE		0x1
#define DB_REQUEST	0x4
#define	DB_ENABLE	0x5
#define	BYPASS		0xF

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

/*
 * LPT port definitions
 */
#ifndef LPT_BASE
#   define LPT_BASE 	0x378
#endif
#define SPP_DATA	(LPT_BASE + 0)
#define SPP_STATUS	(LPT_BASE + 1)
#define SPP_CONTROL	(LPT_BASE + 2)
#define EPP_ADDR	(LPT_BASE + 3)
#define EPP_DATA	(LPT_BASE + 4)
#define ECP_ECR		(LPT_BASE + 0x402)	/* ECP Extended Control Register */

/* Status Register */
#define EPP_STATUS_TMOUT	0x01	/* EPP TimeOut, only for EPP mode */
#define	SPP_STATUS_nIRQ		0x04	/* 0 - IRQ occured */
#define	SPP_STATUS_nERROR	0x08	/* pin 15 - Error bit */
#define	SPP_STATUS_SELECT	0x10	/* pin 13 - Select In */
#define	SPP_STATUS_POUT		0x20	/* pin 12 - Out of Paper */
#define	SPP_STATUS_nACK		0x40	/* pin 10 - Ack */
#define	SPP_STATUS_BUSY		0x80	/* pin 11 - Busy, hw inv */

/* Control Register */
#define	SPP_CONTROL_nSTROBE	0x01	/* pin 1  - Strobe, hw inv */
#define	SPP_CONTROL_nAUTOLF	0x02	/* pin 14 - Auto LineFeed, hw inv */
#define SPP_CONTROL_nRESET	0x04	/* pin 16 - Initialize Printer */
#define	SPP_CONTROL_nSELECT	0x08	/* pin 17 - Select Printer, hw inv */
#define	SPP_CONTROL_IRQEN	0x10	/* IRQ Enable */
#define	SPP_CONTROL_BI		0x20	/* Enable Bi-directional mode */

/* ECP Extended Control Register */
#define ECR_MODE_EPP		0x80	/* Select EPP mode */

/*
 * EPP <-> JTAG Interface Adapter
 */

/* Adapter commands */
#define MCIF_START	0x02	/* Change JTAG TAP state from TLR to RTI */
#define MCIF_FINISH	0x04	/* Change JTAG TAP state from RTI to TLR */
#define MCIF_PREWRITE	0x08	/* Prepare data for writing */
#define MCIF_PREREAD	0x10	/* Prepare data for reading */
#define MCIF_WRITE_DR	0x20	/* Write data from adapter into TAP DR */
#define MCIF_WRITE_IR	0x80	/* Write data from adapter into TAP IR */
#define MCIF_START_WRDR	0x40	/* Write first portion of data into DR */
#define MCIF_CONT_WRDR	0x41	/* Write remaining data into DR */

/* Adapter version >= 0x1 */
#define MCIF_RTI_PAUSE	0x40	/* RTI -> Capture -> Exit1 -> Pause */
#define MCIF_SHIFT	0x41	/* Pause-DR -> Shift-DR -> Pause-DR */
#define MCIF_PAUSE_RTI	0x01	/* Pause -> Exit2 -> Update -> RTI */

#define MCIF_RESET	MCIF_PREREAD

/* Adapter status register */
#define MCIF_STATUS_IDMASK	0x07	/* Adapter version */
#   define MCIF_STATUS_VER1	0x01	/* Non-block IO enabled */
#   define MCIF_STATUS_VER2	0x02	/* Block IO enabled */
#define MCIF_STATUS_BLKIO	0x08	/* Block IO state */
#define MCIF_STATUS_WRIR	0x10	/* Writing IR */
#define MCIF_STATUS_WRDR	0x20	/* Writing DR */
#define MCIF_STATUS_RTI		0x40	/* Run-Test-Idle */
#define MCIF_STATUS_PAUSEDR	0x80	/* State machine in Pause-DR state */

/*
 * Процедуры обращения к портам ввода-вывода.
 */
#ifdef __linux__
#   include <sys/io.h>
#else
#   ifndef inb
inline unsigned char inb (unsigned short port)
{
	unsigned char val;

	__asm__ __volatile__ ("inb %w1, %0" : "=a" (val) : "Nd" (port));
	/*printf ("input from %04x is %02x\n", port, value & 0xff);*/
	return val;
}
#   endif

#   ifndef outb
inline void outb (unsigned char value, unsigned short port)
{
	/*printf ("output to %04x is %02x\n", port, value);*/
	__asm__ __volatile__ ("outb %b0, %w1" : : "a" (value), "Nd" (port));
}
#   endif
#endif

/*
 * Windows: подключение к драйверу GIVEIO.
 */
#if defined (__CYGWIN32__) || defined (MINGW32)
#include <windows.h>
/*#include <winioctl.h>*/

static int install_lpt_service()
{
	SC_HANDLE	SchSCManager;
	SC_HANDLE	schService;
	CHAR		DriverFileName[MAX_PATH];
	CHAR		gvnm[MAX_PATH];

	if (! GetSystemDirectory (DriverFileName, MAX_PATH))
		return -1;

	lstrcat (DriverFileName,"\\Drivers\\giveio.sys");
	sprintf (gvnm, "giveio.sys");

	if (CopyFile (gvnm, DriverFileName, FALSE) == 0)
		return -1;

	SchSCManager = OpenSCManager (NULL, NULL, SC_MANAGER_ALL_ACCESS);
	schService = CreateService (SchSCManager, "giveio", "giveio",
		SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
		"System32\\Drivers\\giveio.sys",
		NULL, NULL, NULL, NULL, NULL);

	if (schService == NULL && GetLastError() != ERROR_SERVICE_EXISTS)
			return -1;

	CloseServiceHandle (schService);
	return 0;
}

static int ensure_lpt_service_running()
{
	SC_HANDLE  SchSCManager, prpCManager;
	SC_HANDLE  schService, prpService;
	SERVICE_STATUS sst;
	QUERY_SERVICE_CONFIG qsc;
	int b;

	qsc.lpBinaryPathName =	 malloc (MAX_PATH);
	qsc.lpDependencies =	 malloc (MAX_PATH);
	qsc.lpDisplayName =	 malloc (MAX_PATH);
	qsc.lpLoadOrderGroup =	 malloc (MAX_PATH);
	qsc.lpServiceStartName = malloc (MAX_PATH);

	SchSCManager = OpenSCManager (NULL, NULL, SC_MANAGER_ALL_ACCESS);

	prpCManager = OpenSCManager (NULL, NULL, SC_MANAGER_ALL_ACCESS);
	prpService = OpenService (prpCManager, "parport", SERVICE_ALL_ACCESS);
	b = QueryServiceStatus (prpService, &sst);
	b = ChangeServiceConfig (prpService, SERVICE_NO_CHANGE,
		SERVICE_DISABLED, SERVICE_NO_CHANGE, NULL, NULL, NULL,
		NULL, NULL, NULL, "Parallel port driver");
	CloseServiceHandle (prpService);
	CloseServiceHandle (prpCManager);

	if (SchSCManager == NULL && GetLastError() == ERROR_ACCESS_DENIED)
		return -1;
	do {
		schService = OpenService (SchSCManager, "giveio", SERVICE_ALL_ACCESS);
		if (schService == NULL)
			switch (GetLastError()) {
			case ERROR_ACCESS_DENIED:
			case ERROR_INVALID_NAME:
				return -1;
			case ERROR_SERVICE_DOES_NOT_EXIST:
				if (install_lpt_service() != 0) {
					fprintf(stderr, "Failed to start GiveIO.SYS driver.\n");
					return -1;
				}
			}
	} while (schService == NULL);

	if (StartService (schService, 0, NULL) == 0 &&
	    GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
		return -1;

	CloseServiceHandle (schService);
	CloseServiceHandle (SchSCManager);
	return 0;
}

static int is_winnt()
{
	OSVERSIONINFO osv;

	osv.dwOSVersionInfoSize = sizeof (osv);
	GetVersionEx (&osv);
	return (osv.dwPlatformId == VER_PLATFORM_WIN32_NT);
}

/*
 * Run virtual device for access some ports (LPT).
 */
static void get_winnt_lpt_access()
{
	HANDLE h;

	ensure_lpt_service_running ();
	h = CreateFile ("\\\\.\\giveio", GENERIC_READ, 0, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fprintf (stderr, "Failed to open GiveIO.SYS device.\n");
		exit (1);
	}
	CloseHandle (h);
}

void jtag_usleep (unsigned long usec)
{
	Sleep (usec / 1000);
}
#else
/* Unix. */
void jtag_usleep (unsigned long usec)
{
	usleep (usec);
}
#endif

/*
 * Переключение направления порта данных LPT.
 */
static void direction_reverse (int reverse)
{
	int ctrl;
	static int cur_mode = -1;

	/* Epp reliability is sensitive to setting the same mode twice.
	 * So, just return if we are already in the requested mode. */
	if (cur_mode == reverse)
		return;

	ctrl = inb (SPP_CONTROL);
	if (reverse) {
		/* Enable bi-directional mode */
		ctrl |= SPP_CONTROL_BI;
	} else {
		/* Make sure port is in Forward Direction */
		ctrl &= ~SPP_CONTROL_BI;
	}
	outb (ctrl, SPP_CONTROL);
	cur_mode = reverse;
}

/*
 * Выдача команды адаптеру EPP<->JTAG.
 */
static void putcmd (char cmd)
{
	direction_reverse (0);
	outb (cmd, EPP_ADDR);
}

/*
 * Ожидание статуса адаптера EPP<->JTAG.
 * Возвращаем 0 при достижении требуемого значения.
 */
static int wait_status (int expected)
{
	int status;

	direction_reverse (1);
	status = inb (EPP_ADDR) & 0xf0;
	if (status == expected)
		return 0;
	return -1;
}

/*
 * Сброс.
 * bitno = 0 - CPU reset
 * bitno = 1 - OnCD reset (JTAG nTRST)
 */
static void reset (int bitno, int on)
{
	static unsigned char val = 0xff;

	if (on)
		val &= ~(0x01 << bitno); /* Assert RST */
	else
		val |= (0x01 << bitno); /* Deassert RST */
	putcmd (MCIF_RESET);
	outb (val, EPP_DATA);
}

/*
 * Загрузка значения в регистр команды IR.
 */
static int set_ir (int new)
{
	int old;
	time_t t0;

	putcmd (MCIF_PREWRITE);
	outb (4, EPP_DATA);		/* MC IR length */
	outb ((char) new, EPP_DATA);
	putcmd (MCIF_WRITE_IR);
	t0 = time (0);
	while (wait_status (MCIF_STATUS_RTI)) {
		if (time (0) > t0 + 1) {
			fprintf (stderr, "set_ir: timeout\n");
			exit (1);
		}
	}
	putcmd (MCIF_PREREAD);
	direction_reverse (1);
	old = inb (EPP_DATA);
	return old;
}

/*
 * Обмен данными через регистр DR.
 */
static void set_dr (char *data, int len)
{
	int i, full_len;
	unsigned out;
	time_t t0;

	putcmd (MCIF_RTI_PAUSE);
#if 1
	t0 = time (0);
	while (wait_status (MCIF_STATUS_PAUSEDR)) {
		if (time (0) > t0 + 1) {
			fprintf (stderr, "Timeout, writing JTAG DR\n");
			exit (1);
		}
	}
#endif
	for (full_len = len; full_len > 0; full_len -= len) {
		/* Adapter has 32 bit data buffer */
		len = (full_len > 32) ? 32 : full_len;

		putcmd (MCIF_PREWRITE);
		outb ((char)len, EPP_DATA);
		for (i = 0; i < (len + 7) / 8; i++)
			outb (data[i], EPP_DATA);
		putcmd (MCIF_SHIFT);
		t0 = time (0);
		while (wait_status (MCIF_STATUS_PAUSEDR)) {
			if (time (0) > t0 + 1) {
				fprintf (stderr, "Timeout, writing JTAG DR\n");
				exit (1);
			}
		}
		direction_reverse (1);
		out = 0;
		for (i = 0; i < 4; i++)
			out |= inb (EPP_DATA) << (8 * i);
		out >>= 32 - len;
		for (i = 0; i < (len + 7) / 8; i++) {
			data[i] = out;
			out >>= 8;
		}
		data += (len + 7) / 8;
	}
	putcmd (MCIF_PAUSE_RTI);
#if 1
	t0 = time (0);
	while (wait_status (MCIF_STATUS_RTI)) {
		if (time (0) > t0 + 1) {
			fprintf(stderr, "Timeout, writing JTAG DR\n");
			exit(1);
		}
	}
#endif
}

/*
 * Обмен данными с блоком OnCD.
 */
static void oncd_io (char *data, int len)
{
	int i;
	time_t t0;

	putcmd (MCIF_PREWRITE);
	outb (len + 1, EPP_DATA);
	for (i = 0; i < (len + 7) / 8; i++)
		outb (data[i], EPP_DATA);
	putcmd (MCIF_WRITE_DR);
	t0 = time (0);
	while (wait_status (MCIF_STATUS_RTI)) {
		if (time (0) > t0 + 1) {
			fprintf (stderr, "oncd_io: timeout\n");
			exit (1);
		}
	}
	putcmd (MCIF_PREREAD);
	direction_reverse (1);
	for (i = 0; i < (len + 7) / 8; i++)
		data[i] = inb (EPP_DATA);
}

/*
 * Чтение регистра OnCD.
 */
static unsigned oncd_read (int reg, int reglen)
{
	unsigned data[2];

	data[0] = (reg | 0x40) << 24;
	data[1] = 0;
	oncd_io ((char*)data + 3, 8 + reglen);
	return data[1];
}

/*
 * Запись регистра OnCD.
 */
static void oncd_write (unsigned int value, int reg, int reglen)
{
	unsigned int data[2];

	data[0] = reg << 24;
	data[1] = value;
	oncd_io ((char*)data + 3, 8 + reglen);
}

/*
 * Выполнение одной инструкции MIPS32.
 */
static void exec (unsigned instr)
{
	/* Restore PCfetch to right address or
	 * we can go in exception. */
	oncd_write (0xBFC00000, OnCD_PCfetch, 32);

	/* Supply instruction to pipeline and do step */
	oncd_write (instr, OnCD_IRdec, 32);
	oncd_write (0, OnCD_EXIT | 0xc0, 0);
}

/*
 * Приведение порта LPT в исходное состояние.
 */
void jtag_start (void)
{
	unsigned char ctrl, status;

	/* LPT port probably in ECP mode, try to select EPP mode */
	outb (ECR_MODE_EPP, ECP_ECR);
	ctrl = inb (SPP_CONTROL);

	/* Initialize port */
	ctrl = SPP_CONTROL_BI | SPP_CONTROL_nRESET;
	outb (ctrl, SPP_CONTROL);

	/* Set SPP_CONTROL_RESET low for a while */
	ctrl &= ~SPP_CONTROL_nRESET;
	outb (ctrl, SPP_CONTROL);
	jtag_usleep (10000); /* we should hold nInit at least 50 usec. */
	ctrl |= SPP_CONTROL_nRESET;

	outb (ctrl, SPP_CONTROL);
	/* clear timeout bit if set */
	status = inb (SPP_STATUS);
	if (status & EPP_STATUS_TMOUT) {
		outb (status, SPP_STATUS);
	}
	if (! (status & SPP_STATUS_BUSY)) {
		/* SPP BUSY high after EPP port reset */
		fprintf (stderr, "\nNo device detected.\nCheck power!\n");
		exit (1);
	}
	putcmd (MCIF_START);
}

/*
 * Перевод кристалла в режим отладки путём манипуляций
 * регистрами данных JTAG.
 */
void jtag_reset ()
{
	int old_ir, i;
	unsigned oscr;

	/* Сброс OnCD. */
	reset (1, 1);
	jtag_usleep (50000);
	reset (1, 0);

	/* Сброс процессора. */
	reset (0, 1);
	jtag_usleep (50000);

	/* Debug request. */
	set_ir (DB_REQUEST);

	/* Wait while processor enters debug mode. */
	i = 0;
	for (;;) {
		old_ir = set_ir (DB_ENABLE);
		if (old_ir & 0x4)
			break;
		jtag_usleep (10000);
		if (++i >= 50) {
			fprintf (stderr, "Timeout while entering debug mode\n");
			exit (1);
		}
	}
	oncd_write (0, OnCD_OBCR, 12);
	oscr = oncd_read (OnCD_OSCR, 32);
	oscr |= OSCR_TME;
	oncd_write (oscr, OnCD_OSCR, 32);
	reset (0, 0);
}

/*
 * Чтение регистра IDCODE.
 */
unsigned jtag_get_idcode (void)
{
	unsigned idcode = 0;

	putcmd (MCIF_FINISH);		/* Enter TLR state */
	putcmd (MCIF_START);		/* Change state TLR->RTI */
	set_dr ((char*) &idcode, 32);
	return idcode;
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
	oncd_write (phys_addr, OnCD_OMAR, 32);
	oncd_write (data, OnCD_OMDR, 32);
	oncd_write (0, OnCD_MEM, 0);
	for (wait = 5; wait != 0; wait--) {
		oscr = oncd_read (OnCD_OSCR, 32);
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
	oscr = oncd_read (OnCD_OSCR, 32);
	oscr |= OSCR_SlctMEM;
	oscr &= ~OSCR_RO;
	oncd_write (oscr, OnCD_OSCR, 32);

	jtag_write_next (data, phys_addr);
}

/*
 * Чтение слова из памяти.
 */
void jtag_read_start ()
{
	unsigned oscr;

	/* Allow memory access */
	oscr = oncd_read (OnCD_OSCR, 32);
	oscr |= OSCR_SlctMEM | OSCR_RO;
	oncd_write (oscr, OnCD_OSCR, 32);
}

unsigned jtag_read_next (unsigned phys_addr)
{
	unsigned wait, oscr, data;

	if (phys_addr >= 0xA0000000)
		phys_addr -= 0xA0000000;
	else if (phys_addr >= 0x80000000)
		phys_addr -= 0x80000000;
	oncd_write (phys_addr, OnCD_OMAR, 32);
	oncd_write (0, OnCD_MEM, 0);
	for (wait = 5; wait != 0; wait--) {
		oscr = oncd_read (OnCD_OSCR, 32);
		if (oscr & OSCR_RDYm)
			break;
		jtag_usleep (10);
	}
	if (wait == 0) {
		fprintf (stderr, "Timeout reading memory, aborted.\n");
		exit (1);
	}
	data = oncd_read (OnCD_OMDR, 32);
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
	/* Set IDT to allow access to EPP port */
#if defined (__CYGWIN32__) || defined (MINGW32)
	if (is_winnt ())
		get_winnt_lpt_access ();
#else
	/* Unfortunately we can permit access only to all ports, because
	 * we need access to ECP_ECR port and it beyond of 1024 bits. */
#if defined (__linux__)
	if (iopl(3) == -1) {
		if (errno == EPERM)
			fprintf (stderr, "This program must be run with superuser privileges.\n");
		else
			perror ("iopl");
		exit (1);
	}
#endif
#if defined (__OpenBSD__) || defined (__NetBSD__)
	if (i386_iopl (3) != 0) {
		perror ("i386_iopl");
		exit (1);
	}
#endif
	/* Drop privilegies.
	 * Yes, there are should be two setuid() calls.
	 * If you don't understand why, then you don't
	 * understand unix security. */
	setuid (getuid());
	setuid (getuid());
#endif
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
	/* if (debug > 1)
		fprintf (stderr, "idcode %08x\n", mc->idcode); */
	switch (mc->idcode) {
	default:
		/* Device not detected. */
		if (mc->idcode == 0xffffffff || mc->idcode == 0)
			fprintf (stderr, "No response from device -- check power is on!\n");
		else
			fprintf (stderr, "No response from device -- unknown idcode 0x%08x!\n",
				mc->idcode);
		exit (1);
	case MC12_ID:
		mc->cpu_name = "MC12";
		break;
	case MC12REV1_ID:
		mc->cpu_name = "MC12r1";
		break;
	}
	jtag_reset ();

	/* CSR: fixed mapping, clock multiply by 5 (from 16 MHz to 80 MHz). */
	jtag_write_word (0x00010051, 0x182F4008);

        /* CSCON3: 5 wait states (really 4 is ok). */
	jtag_write_word (0x00050000, 0x182F100C);

        /* CSCON1: 5 wait states, address 02000000, 64 bits. */
	jtag_write_word (0x009502FE, 0x182F1004);

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
	oncd_write (0xBFC00000, OnCD_PCfetch, 32);
	oncd_write (0x20, OnCD_IRdec, 32);

	/* Flush CPU pipeline at exit */
	oscr = oncd_read (OnCD_OSCR, 32);
	oscr &= ~(OSCR_TME | OSCR_IME | OSCR_SlctMEM | OSCR_RDYm);
	oscr |= OSCR_MPE;
	oncd_write (oscr, OnCD_OSCR, 32);

	/* Exit */
	oncd_write (0, OnCD_EXIT | 0x60, 0);
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
 * На вычислителе ИТМиВТ имеем три банка:
 * - 2 мегабайта с адреса 1FA00000
 * - 2 мегабайта с адреса 1FC00000
 * - 4 мегабайта с адреса 02000000, 64-битный режим.
 */
static unsigned long compute_base (unsigned long addr)
{
	if (addr >= 0xA0000000)
		addr -= 0xA0000000;
	else if (addr >= 0x80000000)
		addr -= 0x80000000;

	if (addr >= 0x1FC00000 && addr < 0x1FE00000)
		return 0x1FC00000;
	if (addr >= 0x1FA00000 && addr < 0x1FC00000)
		return 0x1FA00000;
	if (addr >= 0x02000000 && addr < 0x02400000)
		return 0x02000000;
	fprintf (stderr, "Invalid address 0x%08lx!\n", addr);
	exit (1);
	return 0;
}

int multicore_flash_detect (multicore_t *mc, unsigned addr,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width)
{
	int count;
	unsigned long base;

	base = compute_base (addr);
	for (count=0; count<10; ++count) {
		/* Try both 32 and 64 bus width.*/
		if (count & 1) {
			mc->flash_width = 64;
			mc->flash_addr_5555 = FLASH_ADDR64_5555;
			mc->flash_addr_2aaa = FLASH_ADDR64_2AAA;
			mc->flash_devid_offset = 8;
		} else {
			mc->flash_width = 32;
			mc->flash_addr_5555 = FLASH_ADDR32_5555;
			mc->flash_addr_2aaa = FLASH_ADDR32_2AAA;
			mc->flash_devid_offset = 4;
		}
		/* Read device code. */
		jtag_write_word (FLASH_CMD_AA, base + mc->flash_addr_5555);
		jtag_write_word (FLASH_CMD_55, base + mc->flash_addr_2aaa);
		jtag_write_word (FLASH_CMD_90, base + mc->flash_addr_5555);
		*dev = jtag_read_word (base + mc->flash_devid_offset);
		/* if (debug > 1)
			fprintf (stderr, "flash id %08x\n", *dev); */
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
		}
	}
	/*printf ("Bad flash id = %08x, must be %08x, %08x or %08x\n",
		*dev, ID_29LV800_B, ID_29LV800_T, ID_39VF800_A);*/
	return 0;

success:
	/* Read MFR code. */
	*mf = jtag_read_word (base);
	switch (*mf) {
	case ID_ALLIANCE: strcpy (mfname, "Alliance");	   break;
	case ID_AMD:	  strcpy (mfname, "AMD");	   break;
	case ID_SST:	  strcpy (mfname, "SST");	   break;
	default:	  sprintf (mfname, "<%08x>", *mf); break;
	}

	/* Stop read ID mode. */
	jtag_write_word (FLASH_CMD_F0, base);

	*bytes = mc->flash_bytes;
	*width = mc->flash_width;
	return 1;
}

int multicore_erase (multicore_t *mc, unsigned long addr)
{
	unsigned long word, base;

	/* Chip erase. */
	base = compute_base (addr);
	printf ("Erase: %08lx", base);
	jtag_write_word (FLASH_CMD_AA, base + mc->flash_addr_5555);
	jtag_write_word (FLASH_CMD_55, base + mc->flash_addr_2aaa);
	jtag_write_word (FLASH_CMD_80, base + mc->flash_addr_5555);
	jtag_write_word (FLASH_CMD_AA, base + mc->flash_addr_5555);
	jtag_write_word (FLASH_CMD_55, base + mc->flash_addr_2aaa);
	jtag_write_word (FLASH_CMD_10, base + mc->flash_addr_5555);
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

void multicore_write_word (multicore_t *mc, unsigned long addr, unsigned long word)
{
	unsigned long base;

	base = compute_base (addr);
	jtag_write_word (FLASH_CMD_AA, base + mc->flash_addr_5555);
	jtag_write_next (FLASH_CMD_55, base + mc->flash_addr_2aaa);
	jtag_write_next (FLASH_CMD_A0, base + mc->flash_addr_5555);
	jtag_write_next (word, addr);
	/* TODO: запись в 64-битную flash-память пока не реализована. */
}

void multicore_read_start (multicore_t *mc)
{
	jtag_read_start ();
}

unsigned long multicore_read_next (multicore_t *mc, unsigned long addr)
{
	return jtag_read_next (addr);
}
