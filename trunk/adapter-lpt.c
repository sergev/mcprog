/*
 * Интерфейс к адаптеру LPT-JTAG фирмы Элвис.
 * Разработано в ИТМиВТ, 2006-2009.
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

#include "adapter.h"
#include "oncd.h"
#include "localize.h"

typedef struct {
    /* Общая часть. */
    adapter_t adapter;
} lpt_adapter_t;

/*
 * LPT port definitions
 */
//#define LPT_BASE  0xe800          /* LPT on PCI */
//#define ECP_ECR   0xe482          /* ECP Extended Control Register */

#ifndef LPT_BASE
#   define LPT_BASE 0x378
#   define ECP_ECR  (LPT_BASE + 0x402)  /* ECP Extended Control Register */
#endif
#define SPP_DATA    (LPT_BASE + 0)
#define SPP_STATUS  (LPT_BASE + 1)
#define SPP_CONTROL (LPT_BASE + 2)
#define EPP_ADDR    (LPT_BASE + 3)
#define EPP_DATA    (LPT_BASE + 4)

/* Status Register */
#define EPP_STATUS_TMOUT    0x01    /* EPP TimeOut, only for EPP mode */
#define SPP_STATUS_nERROR   0x08    /* pin 15 - Error bit */
#define SPP_STATUS_SELECT   0x10    /* pin 13 - Select In */
#define SPP_STATUS_POUT     0x20    /* pin 12 - Out of Paper */
#define SPP_STATUS_nACK     0x40    /* pin 10 - Ack */
#define SPP_STATUS_nBUSY    0x80    /* pin 11 - Busy, hw inv */

/* Control Register */
#define SPP_CONTROL_nSTROBE 0x01    /* pin 1  - Strobe, hw inv */
#define SPP_CONTROL_nAUTOLF 0x02    /* pin 14 - Auto LineFeed, hw inv */
#define SPP_CONTROL_nRESET  0x04    /* pin 16 - Initialize Printer */
#define SPP_CONTROL_nSELECT 0x08    /* pin 17 - Select Printer, hw inv */
#define SPP_CONTROL_IRQEN   0x10    /* IRQ Enable */
#define SPP_CONTROL_DIRIN   0x20    /* Enable input mode */

/* ECP Extended Control Register */
#define ECR_MODE_EPP        0x80    /* Select EPP mode */

/*
 * EPP <-> JTAG Interface Adapter
 */

/* Adapter commands */
#define MCIF_START          0x02    /* Change JTAG TAP state from TLR to RTI */
#define MCIF_FINISH         0x04    /* Change JTAG TAP state from RTI to TLR */
#define MCIF_PREWRITE       0x08    /* Prepare data for writing */
#define MCIF_PREREAD        0x10    /* Prepare data for reading */
#define MCIF_WRITE_DR       0x20    /* Write data from adapter into TAP DR */
#define MCIF_WRITE_IR       0x80    /* Write data from adapter into TAP IR */

/* Adapter version >= 0x1 */
#define MCIF_RTI_PAUSE      0x40    /* RTI -> Capture -> Exit1 -> Pause */
#define MCIF_SHIFT          0x41    /* Pause-DR -> Shift-DR -> Pause-DR */
#define MCIF_PAUSE_RTI      0x01    /* Pause -> Exit2 -> Update -> RTI */

#define MCIF_RESET          MCIF_PREREAD

/* Adapter status register */
#define MCIF_STATUS_IDMASK  0x07    /* Adapter version */
#   define MCIF_STATUS_VER1 0x01    /* Non-block IO enabled */
#   define MCIF_STATUS_VER2 0x02    /* Block IO enabled */
#define MCIF_STATUS_BLKIO   0x08    /* Block IO state */
#define MCIF_STATUS_WRIR    0x10    /* Writing IR */
#define MCIF_STATUS_WRDR    0x20    /* Writing DR */
#define MCIF_STATUS_RTI     0x40    /* Run-Test-Idle */
#define MCIF_STATUS_PAUSEDR 0x80    /* State machine in Pause-DR state */

/*
 * Процедуры обращения к портам ввода-вывода.
 */
#ifdef __linux__
#include <sys/io.h>
#endif

#ifndef inb
#define inb myinb
static inline unsigned char inb (unsigned port)
{
    unsigned char value;

    __asm__ __volatile__ ("inb %w1, %0" : "=a" (value) : "Nd" (port));
    if (debug_level > 1) {
        if (port == SPP_CONTROL)     printf ("        ctrl");
        else if (port == SPP_STATUS) printf ("        stat");
        else if (port == EPP_ADDR)   printf ("        addr");
        else if (port == EPP_DATA)   printf ("        data");
        else                 printf ("        %04x", port);
        printf (" -> %02x\n", value);
    }
    return value;
}
#endif

#ifndef outb
#define outb myoutb
static inline void outb (unsigned char value, unsigned port)
{
    if (debug_level > 1) {
        printf ("%02x -> ", value);
        if (port == SPP_CONTROL)     printf ("ctrl\n");
        else if (port == SPP_STATUS) printf ("stat\n");
        else if (port == EPP_ADDR)   printf ("addr\n");
        else if (port == EPP_DATA)   printf ("data\n");
        else                 printf ("%04x\n", port);
    }
    __asm__ __volatile__ ("outb %b0, %w1" : : "a" (value), "Nd" (port));
}
#endif

/*
 * Windows: подключение к драйверу GIVEIO.
 */
#if defined (__CYGWIN32__) || defined (MINGW32)
#include <windows.h>
/*#include <winioctl.h>*/

static int install_lpt_service()
{
    SC_HANDLE   SchSCManager;
    SC_HANDLE   schService;
    CHAR        DriverFileName[MAX_PATH];
    CHAR        gvnm[MAX_PATH];

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

    qsc.lpBinaryPathName =   malloc (MAX_PATH);
    qsc.lpDependencies =     malloc (MAX_PATH);
    qsc.lpDisplayName =  malloc (MAX_PATH);
    qsc.lpLoadOrderGroup =   malloc (MAX_PATH);
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
        /* Enable input mode */
        ctrl |= SPP_CONTROL_DIRIN;
    } else {
        /* Make sure port is in Forward Direction */
        ctrl &= ~SPP_CONTROL_DIRIN;
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

#if 0
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
#endif

/*
 * Загрузка значения в регистр команды IR.
 */
static int set_ir (int new)
{
    int old;
    time_t t0;

    putcmd (MCIF_PREWRITE);
    outb (4, EPP_DATA);     /* MC IR length */
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
            goto failed;
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
failed:         fprintf (stderr, _("No device on LPT port -- check cable!\n"));
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
            goto failed;
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
static unsigned lpt_oncd_read (adapter_t *adapter, int reg, int nbits)
{
    unsigned data[2];

    data[0] = (reg | 0x40) << 24;
    data[1] = 0;
    oncd_io ((char*)data + 3, 8 + nbits);
    return data[1];
}

/*
 * Запись регистра OnCD.
 */
static void lpt_oncd_write (adapter_t *adapter,
    unsigned val, int reg, int nbits)
{
    unsigned int data[2];

    data[0] = reg << 24;
    data[1] = val;
    oncd_io ((char*)data + 3, 8 + nbits);
}

/*
 * Перевод кристалла в режим отладки путём манипуляций
 * регистрами данных JTAG.
 */
static void lpt_stop_cpu (adapter_t *adapter)
{
    int old_ir, i;
#if 0
    /* Сброс OnCD. */
    reset (1, 1);
    mdelay (50);
    reset (1, 0);

    /* Сброс процессора. */
    reset (0, 1);
    mdelay (50);
#endif
    /* Debug request. */
    set_ir (TAP_DEBUG_REQUEST);

    /* Wait while processor enters debug mode. */
    i = 0;
    for (;;) {
        old_ir = set_ir (TAP_DEBUG_ENABLE);
        if (old_ir & 0x4)
            break;
        mdelay (10);
        if (++i >= 50) {
            fprintf (stderr, "Timeout while entering debug mode\n");
            exit (1);
        }
    }
}

/*
 * Чтение регистра IDCODE.
 */
static unsigned lpt_get_idcode (adapter_t *adapter)
{
    unsigned idcode = 0;

    putcmd (MCIF_FINISH);       /* Enter TLR state */
    putcmd (MCIF_START);        /* Change state TLR->RTI */
    set_dr ((char*) &idcode, 32);
    return idcode;
}

/*
 * Аппаратный сброс процессора.
 */
static void lpt_reset_cpu (adapter_t *adapter)
{
    putcmd (MCIF_RESET);
    outb (0xfe, EPP_DATA);      /* Активация /SYSRST */
    mdelay (10);
    putcmd (MCIF_RESET);
    outb (0xff, EPP_DATA);
}

/*
 * Выяснение, остановлен ли процессор.
 */
static int lpt_cpu_stopped (adapter_t *adapter)
{
    unsigned ir = set_ir (TAP_DEBUG_ENABLE);
    return ir & 4;
}

/*
 * Close the device.
 */
static void lpt_close (adapter_t *adapter)
{
    lpt_adapter_t *a = (lpt_adapter_t*) adapter;

    free (a);
}

/*
 * Инициализация адаптера LPT-JTAG.
 * Возвращаем указатель на структуру данных, выделяемую динамически.
 * Если адаптер не обнаружен, возвращаем 0.
 */
adapter_t *adapter_open_lpt (void)
{
    lpt_adapter_t *a;
    unsigned char ctrl, status;

    /*
     * Установка доступа к аппаратным портам ввода-вывода.
     */
#if defined (__CYGWIN32__) || defined (MINGW32)
    if (is_winnt ())
        get_winnt_lpt_access ();
#else
    /* Unfortunately we can permit access only to all ports, because
     * we need access to ECP_ECR port and it beyond of 1024 bits. */
#if defined (__linux__)
    if (iopl(3) == -1) {
        if (errno == EPERM)
            fprintf (stderr, "LPT adapter: superuser privileges needed.\n");
        else
            perror ("LPT adapter: iopl failed");
        return 0;
    }
#endif
#if defined (__OpenBSD__) || defined (__NetBSD__)
    if (i386_iopl (3) != 0) {
        perror ("LPT adapter: i386_iopl failed");
        return 0;
    }
#endif
    /* Drop privilegies.
     * Yes, there are should be two setuid() calls.
     * If you don't understand why, then you don't
     * understand unix security. */
    setuid (getuid());
    setuid (getuid());
#endif
    /*
     * Приведение порта LPT в исходное состояние.
     */
    /* LPT port probably in ECP mode, try to select EPP mode */
    outb (ECR_MODE_EPP, ECP_ECR);
    ctrl = inb (SPP_CONTROL);
#if 0
    unsigned char dsr = inb (SPP_STATUS);
    unsigned char ecr = inb (ECP_ECR);
    outb (1, ECP_ECR);
    outb (0xe0, ECP_ECR);
    unsigned char confa = inb (ECP_ECR - 2);
    unsigned char confb = inb (ECP_ECR - 1);
    fprintf (stderr, "dsr = %02x, dcr = %02x, conf-a = %02x, conf-b = %02x, ecr = %02x\n",
        dsr, ctrl, confa, confb, ecr);
    if (confa != 0x94) {
        fprintf (stderr, "Unknown parallel port controller detected: conf-a = %02x.\n", confa);
        fprintf (stderr, "Correct value is 0x94 for NetMos PCI 9835 Multi-I/O Controller\n");
    }
    outb (ECR_MODE_EPP, ECP_ECR);
#endif
    /* Initialize port */
    ctrl = SPP_CONTROL_DIRIN | SPP_CONTROL_nRESET;
    outb (ctrl, SPP_CONTROL);

    /* Set SPP_CONTROL_RESET low for a while */
    ctrl &= ~SPP_CONTROL_nRESET;
    outb (ctrl, SPP_CONTROL);
    mdelay (10); /* we should hold nInit at least 50 usec. */
    ctrl |= SPP_CONTROL_nRESET;
    outb (ctrl, SPP_CONTROL);

    /* clear timeout bit if set */
    status = inb (SPP_STATUS);
    if (status & EPP_STATUS_TMOUT) {
        outb (status, SPP_STATUS);
    }
    if (! (status & SPP_STATUS_nBUSY)) {
        /* SPP BUSY high after EPP port reset */
/*      fprintf (stderr, "\nNo Elvees device detected on LPT adapter.\nCheck power!\n");*/
        return 0;
    }
    putcmd (MCIF_START);

    a = calloc (1, sizeof (*a));
    if (! a) {
        fprintf (stderr, "Out of memory\n");
        return 0;
    }

    /* Обязательные функции. */
    a->adapter.name = "Elvees LPT";
    a->adapter.close = lpt_close;
    a->adapter.get_idcode = lpt_get_idcode;
    a->adapter.cpu_stopped = lpt_cpu_stopped;
    a->adapter.stop_cpu = lpt_stop_cpu;
    a->adapter.reset_cpu = lpt_reset_cpu;
    a->adapter.oncd_read = lpt_oncd_read;
    a->adapter.oncd_write = lpt_oncd_write;
    return &a->adapter;
}
