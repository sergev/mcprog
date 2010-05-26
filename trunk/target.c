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
#include "mips.h"

#define NFLASH      16  /* Max flash regions. */

struct _target_t {
    adapter_t   *adapter;
    const char  *cpu_name;
    unsigned    idcode;
    unsigned    is_running;
    unsigned    cscon3;     /* Регистр конфигурации flash-памяти */
    unsigned    flash_width;
    unsigned    flash_bytes;
    unsigned    flash_addr_odd;
    unsigned    flash_addr_even;
    unsigned    flash_cmd_aa;
    unsigned    flash_cmd_55;
    unsigned    flash_cmd_10;
    unsigned    flash_cmd_20;
    unsigned    flash_cmd_80;
    unsigned    flash_cmd_90;
    unsigned    flash_cmd_a0;
    unsigned    flash_cmd_f0;
    unsigned    flash_devid_offset;
    unsigned    flash_base [NFLASH];
    unsigned    flash_last [NFLASH];
    unsigned    flash_delay;

    unsigned    pc_fetch, pc_dec, ir_dec, pc_exec;
//    unsigned    pc_mem, pc_wb;
    unsigned    mem0;
    unsigned    reg [32], valid [32];
    unsigned    reg_lo, valid_lo;
    unsigned    reg_hi, valid_hi;
    unsigned    reg_cp0 [32], valid_cp0 [32];
    unsigned    reg_fpu [32], valid_fpu [32];
    unsigned    reg_fcsr, valid_fcsr;
    unsigned    reg_fir, valid_fir;
    unsigned    exception;
#define NO_EXCEPTION 0xffffffff
};

/* Идентификатор производителя flash. */
#define ID_ALLIANCE     0x00520052
#define ID_AMD          0x00010001
#define ID_SST          0x00BF00BF
#define ID_MILANDR      0x01010101
#define ID_ANGSTREM     0xBFBFBFBF
#define ID_SPANSION     0x01010101

/* Идентификатор микросхемы flash. */
#define ID_29LV800_B    0x225b225b
#define ID_29LV800_T    0x22da22da
#define ID_39VF800_A    0x27812781
#define ID_39VF6401_B   0x236d236d
#define ID_1636PP2Y     0xc8c8c8c8
#define ID_1638PP1      0x07070707
#define ID_S29AL032D        0x000000f9

/* Команды flash. */
#define FLASH_CMD16_AA  0x00AA00AA
#define FLASH_CMD8_AA   0xAAAAAAAA
#define FLASH_CMD16_55  0x00550055
#define FLASH_CMD8_55   0x55555555
#define FLASH_CMD16_10  0x00100010  /* Chip erase 2/2 */
#define FLASH_CMD8_10   0x10101010
#define FLASH_CMD16_20  0x00200020  /* Unlock bypass */
#define FLASH_CMD8_20   0x20202020
#define FLASH_CMD16_80  0x00800080  /* Chip erase 1/2 */
#define FLASH_CMD8_80   0x80808080
#define FLASH_CMD16_90  0x00900090  /* Read ID */
#define FLASH_CMD8_90   0x90909090
#define FLASH_CMD16_A0  0x00A000A0  /* Program */
#define FLASH_CMD8_A0   0xA0A0A0A0
#define FLASH_CMD16_F0  0x00F000F0  /* Reset */
#define FLASH_CMD8_F0   0xF0F0F0F0

/* Идентификатор версии процессора. */
#define MC12_ID         0x20777001
#define MC12REV1_ID     0x30777001
#define MC12REV2_ID     0x40777001

#define MC_CSCON3               0x182F100C
#define MC_CSCON3_ADDR(addr)    ((addr & 3) << 20)

#define CRAM_ADDR   0xb8000000
#define BOOT_ADDR   0xbfc00000
#define GP          28
#define FP          30

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
    t->adapter->oncd_write (t->adapter, BOOT_ADDR, OnCD_PCfetch, 32);

    /* Supply instruction to pipeline and do step */
    t->adapter->oncd_write (t->adapter, instr, OnCD_IRdec, 32);
    t->adapter->oncd_write (t->adapter, 0,
        OnCD_GO | IRd_FLUSH_PIPE | IRd_STEP_1CLK, 0);
}

/*
 * Чтение регистра через запись в 0-е слово внутренней памяти.
 * Регистр GP используется как база.
 */
static unsigned target_read_reg (target_t *t, int regno)
{
    target_exec (t, MIPS_SW | (GP << 21) | (regno << 16));
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    return target_read_word (t, CRAM_ADDR);
}

/*
 * Установка значения регистра.
 */
static void target_write_reg (target_t *t, int regno, unsigned val)
{
    /* addiu rt, immed */
    target_exec (t, MIPS_ADDI | (regno << 16) | (val >> 16));

    /* sll rd, rt, 16 */
    target_exec (t, MIPS_SLL | (regno << 11) | (regno << 16) | (16 << 6));

    /* ori rt, rs, immed */
    target_exec (t, MIPS_ORI | (regno << 16) | (regno << 21) | (val & 0xffff));
}

/*
 * Чтение регистра сопроцессора 0.
 * В качестве промежуточного используется регистр FP.
 */
static unsigned target_read_cp0 (target_t *t, int regno)
{
    target_exec (t, MIPS_MFC0 | (FP << 16) | (regno << 11));
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    return target_read_reg (t, FP);
}

/*
 * Запись регистра сопроцессора 0.
 * В качестве промежуточного используется регистр FP.
 */
static void target_write_cp0 (target_t *t, int regno, unsigned val)
{
    target_write_reg (t, FP, val);

    target_exec (t, MIPS_MTC0 | (FP << 16) | (regno << 11));
    target_exec (t, MIPS_NOP);
}

/*
 * Cохранение состояния процессора:
 * - конвейер
 * - 0-е слово внутренней памяти
 * - регистры GP и FP
 */
static void target_save_state (target_t *t)
{
    int i;

    /* Сохраняем конвейер. */
    t->pc_fetch = t->adapter->oncd_read (t->adapter, OnCD_PCfetch, 32);
    t->pc_dec = t->adapter->oncd_read (t->adapter, OnCD_PCdec, 32);
    t->ir_dec = t->adapter->oncd_read (t->adapter, OnCD_IRdec, 32);
    t->pc_exec = t->adapter->oncd_read (t->adapter, OnCD_PCexec, 32);
//    t->pc_mem = t->adapter->oncd_read (t->adapter, OnCD_PCmem, 32);
//    t->pc_wb = t->adapter->oncd_read (t->adapter, OnCD_PCwb, 32);
//fprintf (stderr, "PC fetch = %08x\n", t->pc_fetch);
//fprintf (stderr, "PC dec   = %08x, ir = %08x\n", t->pc_dec, t->ir_dec);
//fprintf (stderr, "PC exec  = %08x\n", t->pc_exec);
//fprintf (stderr, "PC mem   = %08x\n", t->pc_mem);
//fprintf (stderr, "PC wb    = %08x\n", t->pc_wb);

    /* Снимаем запрет останова в Delay Slot. */
    t->adapter->oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
    t->adapter->oscr &= ~OSCR_NDS;

    /* Отменяем исключение по адресу PC. */
    t->adapter->oscr |= OSCR_NFEXP;
    t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);

    t->exception = NO_EXCEPTION;
    if (t->pc_exec) {
        /* Завершаем выполнение конвейера: стадии 'exec', 'mem' и 'wb'. */
        for (i=0; i<3; i++) {
            target_exec (t, MIPS_NOP);

            /* Проверяем, не произошло ли исключение.
             * Их может быть несколько, нам важно только первое. */
            if (t->exception == NO_EXCEPTION) {
                /* Поскольку target_exec() устанавливает PCfetch равным 0xbfc00000,
                 * после выполнения одного шага мы должны получить 0xbfc00004. */
                unsigned pc = t->adapter->oncd_read (t->adapter, OnCD_PCfetch, 32);
                if (pc != BOOT_ADDR + 4) {
                    /* Имеем исключение на стадиях 'exec' или 'mem'. */
                    t->exception = pc;
                }
            }
        }
    }
    /* Восстанавливаем исключение по адресу PC. */
    t->adapter->oscr &= ~OSCR_NFEXP;

    /* Ставим бит отладки.
     * Он останавливает счётчики, блокирует прерывания и исключения TLB,
     * и разрешает доступ к привилегированным ресурсам. */
    t->adapter->oscr |= OSCR_DBM;
    t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);

    /* Забываем старые значения регистров. */
    for (i=0; i<32; i++) {
        t->valid[i] = 0;
        t->valid_cp0[i] = 0;
        t->valid_fpu[i] = 0;
    }
    t->valid_lo = 0;
    t->valid_hi = 0;
    t->valid_fcsr = 0;
    t->valid_fir = 0;

    /* Сохраняем 0-е слово внутренней памяти. */
    t->mem0 = target_read_word (t, CRAM_ADDR);

    /* Сохраняем регистр GP. */
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_JR | (GP << 21));
    t->reg[GP] = t->adapter->oncd_read (t->adapter, OnCD_PCfetch, 32);
    t->valid[GP] = 1;

    /* Устанавливаем регистр GP равным адресу CRAM. */
    target_exec (t, MIPS_LUI | (GP << 16) | (CRAM_ADDR >> 16) | 0x8000);
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);

    /* Сохраняем регистр FP. */
    t->reg[FP] = target_read_reg (t, FP);
    t->valid[FP] = 1;
}

/*
 * Восстановление состояния процессора:
 * - регистры GP и FP
 * - 0-е слово внутренней памяти
 * - конвейер
 */
static void target_restore_state (target_t *t)
{
    int i;

    /* Восстанавливаем регистры FP и GP. */
    target_write_reg (t, FP, t->reg[FP]);
    target_write_reg (t, GP, t->reg[GP]);

    /* Восстанавливаем 0-е слово внутренней памяти. */
    target_write_word (t, CRAM_ADDR, t->mem0);

    /* Очищаем конвейер. */
    for (i=0; i<3; i++) {
        target_exec (t, MIPS_NOP);
    }

    if (t->exception == NO_EXCEPTION) {
        /* Если нет исключения - восстанавливаем стадию 'dec' конвейера. */
        t->adapter->oncd_write (t->adapter, t->pc_dec, OnCD_PCfetch, 32);
        t->adapter->oncd_write (t->adapter, MIPS_NOP, OnCD_IRdec, 32);
        t->adapter->oncd_write (t->adapter,
            0, OnCD_GO | IRd_FLUSH_PIPE | IRd_STEP_1CLK, 0);
        t->adapter->oncd_write (t->adapter, t->ir_dec, OnCD_IRdec, 32);

        /* Восстанавливаем стадию 'fetch'. */
        t->adapter->oncd_write (t->adapter, t->pc_fetch, OnCD_PCfetch, 32);
    } else {
        /* Переходим на обработчик исключения. */
        t->adapter->oncd_write (t->adapter, t->exception, OnCD_PCfetch, 32);
        t->adapter->oncd_write (t->adapter, MIPS_NOP, OnCD_IRdec, 32);
        t->adapter->oncd_write (t->adapter,
            0, OnCD_GO | IRd_FLUSH_PIPE | IRd_STEP_1CLK, 0);
    }

    /* Запрещаем останов в Delay Slot. */
    t->adapter->oscr |= OSCR_NDS;

    /* Снимаем бит отладки. */
    t->adapter->oscr &= ~OSCR_DBM;
    t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);
}

/*
 * Чтение регистра FPU через запись в 0-е слово внутренней памяти.
 * Регистр GP используется как база.
 */
static unsigned target_read_fpu (target_t *t, int regno)
{
    target_exec (t, MIPS_SWC1 | (GP << 21) | (regno << 16));
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    return target_read_word (t, CRAM_ADDR);
}

/*
 * Установка значения регистра FPU.
 * В качестве промежуточного используется регистр FP.
 */
static void target_write_fpu (target_t *t, int regno, unsigned val)
{
    target_write_reg (t, FP, val);
    target_exec (t, MIPS_MTC1 | (FP << 16) | (regno << 11));
    target_exec (t, MIPS_NOP);
}

/*
 * Чтение регистра сопроцессора 0.
 * В качестве промежуточного используется регистр FP.
 */
static unsigned target_read_fpuctl (target_t *t, int regno)
{
    target_exec (t, MIPS_CFC1 | (FP << 16) | (regno << 11));
    target_exec (t, MIPS_NOP);
    target_exec (t, MIPS_NOP);
    return target_read_reg (t, FP);
}

/*
 * Запись регистра сопроцессора 0.
 * В качестве промежуточного используется регистр FP.
 */
static void target_write_fpuctl (target_t *t, int regno, unsigned val)
{
    target_write_reg (t, FP, val);

    target_exec (t, MIPS_CTC1 | (FP << 16) | (regno << 11));
    target_exec (t, MIPS_NOP);
}

/*
 * Чтение произвольного регистра:
 *      0-31  - регистры процессора MIPS
 *      32    - CP0 status
 *      33    - регистр LO
 *      34    - регистр HI
 *      35    - CP0 badvaddr
 *      36    - CP0 cause
 *      37    - PC
 *      38-69 - регистры FPU
 *      70    - FCSR
 *      71    - FIR
 */
unsigned target_read_register (target_t *t, unsigned regno)
{
    switch (regno) {
    case 0 ... 31:              /* регистры процессора MIPS */
        if (! t->valid [regno]) {
            t->reg [regno] = target_read_reg (t, regno);
            t->valid [regno] = 1;
        }
        return t->reg [regno];

    case 32:                    /* CP0 status */
        if (! t->valid_cp0 [CP0_STATUS]) {
            t->reg_cp0 [CP0_STATUS] = target_read_cp0 (t, CP0_STATUS);
            t->valid_cp0 [CP0_STATUS] = 1;
        }
        return t->reg_cp0 [CP0_STATUS];

    case 33:                    /* регистр LO */
        if (! t->valid_lo) {
            target_exec (t, MIPS_MFLO | (FP << 11));
            t->reg_lo = target_read_reg (t, FP);
            t->valid_lo = 1;
        }
        return t->reg_lo;

    case 34:                    /* регистр HI */
        if (! t->valid_hi) {
            target_exec (t, MIPS_MFHI | (FP << 11));
            t->reg_hi = target_read_reg (t, FP);
            t->valid_hi = 1;
        }
        return t->reg_hi;

    case 35:                    /* CP0 badvaddr */
        if (! t->valid_cp0 [CP0_BADVADDR]) {
            t->reg_cp0 [CP0_BADVADDR] = target_read_cp0 (t, CP0_BADVADDR);
            t->valid_cp0 [CP0_BADVADDR] = 1;
        }
        return t->reg_cp0 [CP0_BADVADDR];

    case 36:                    /* CP0 cause */
        if (! t->valid_cp0 [CP0_CAUSE]) {
            t->reg_cp0 [CP0_CAUSE] = target_read_cp0 (t, CP0_CAUSE);
            t->valid_cp0 [CP0_CAUSE] = 1;
        }
        return t->reg_cp0 [CP0_CAUSE];

    case 37:                    /* PC */
        if (t->exception != NO_EXCEPTION)
            return t->exception;
        return t->pc_dec;

    case 38 ... 69:             /* регистры FPU */
        if (! t->valid_fpu [regno-38]) {
            t->reg_fpu [regno-38] = target_read_fpu (t, regno-38);
//fprintf (stderr, "f%d = %08x\n", regno-38, t->reg_fpu [regno-38]);
            t->valid_fpu [regno-38] = 1;
        }
        return t->reg_fpu [regno-38];

    case 70:                    /* FCSR */
        if (! t->valid_fcsr) {
            t->reg_fcsr = target_read_fpuctl (t, CP1_FCSR);
            t->valid_fcsr = 1;
        }
        return t->reg_fcsr;

    case 71:                    /* FIR */
        if (! t->valid_fir) {
            t->reg_fir = target_read_fpuctl (t, CP1_FIR);
            t->valid_fir = 1;
        }
        return t->reg_fir;
    }
    return 0;
}

/*
 * Установка значения произвольного регистра.
 * Регистры BADVADDR и FIR изменять нельзя.
 * Сразу заносим в устройство, кроме PC, FP и GP.
 */
void target_write_register (target_t *t, unsigned regno, unsigned val)
{
    switch (regno) {
    case 0 ... 31:              /* регистры процессора MIPS */
        target_write_reg (t, regno, val);
        t->reg [regno] = val;
        t->valid [regno] = 1;
        break;
    case 32:                    /* CP0 status */
        target_write_cp0 (t, CP0_STATUS, val);
        t->reg_cp0 [CP0_STATUS] = val;
        t->valid_cp0 [CP0_STATUS] = 1;
        break;
    case 33:                    /* регистр LO */
        target_write_reg (t, FP, val);
        target_exec (t, MIPS_NOP);
        target_exec (t, MIPS_MTLO | (FP << 21));
        target_exec (t, MIPS_NOP);
        t->reg_lo = val;
        t->valid_lo = 1;
        break;
    case 34:                    /* регистр HI */
        target_write_reg (t, FP, val);
        target_exec (t, MIPS_NOP);
        target_exec (t, MIPS_MTHI | (FP << 21));
        target_exec (t, MIPS_NOP);
        t->reg_hi = val;
        t->valid_hi = 1;
        break;
    case 36:                    /* CP0 cause */
        target_write_cp0 (t, CP0_CAUSE, val);
        t->reg_cp0 [CP0_CAUSE] = val;
        t->valid_cp0 [CP0_CAUSE] = 1;
        break;
    case 37:                    /* PC */
        /* Изменение адреса следующей команды реализуется
         * аналогично входу в отработчик исключения. */
        t->exception = val;
        break;
    case 38 ... 69:             /* регистры FPU */
        target_write_fpu (t, regno-38, val);
        t->reg_fpu [regno-38] = val;
        t->valid_fpu [regno-38] = 1;
        break;
    case 70:                    /* FCSR */
        target_write_fpuctl (t, CP1_FCSR, val);
        t->reg_fcsr = val;
        t->valid_fcsr = 1;
        break;
    }
}

/*
 * Запись слова в память.
 */
void target_write_next (target_t *t, unsigned phys_addr, unsigned data)
{
    unsigned count;

    if (phys_addr >= 0xA0000000)
        phys_addr -= 0xA0000000;
    else if (phys_addr >= 0x80000000)
        phys_addr -= 0x80000000;

    if (debug_level)
        fprintf (stderr, "write %08x to %08x\n", data, phys_addr);

    t->adapter->oncd_write (t->adapter, phys_addr, OnCD_OMAR, 32);
    t->adapter->oncd_write (t->adapter, data, OnCD_OMDR, 32);
    t->adapter->oncd_write (t->adapter, 0, OnCD_MEM, 0);

    if (t->is_running) {
        /* Если процессор запущен, обращение к памяти произойдёт не сразу.
         * Надо ждать появления бита RDYm в регистре OSCR. */
        for (count = 100; count != 0; count--) {
            t->adapter->oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
            if (t->adapter->oscr & OSCR_RDYm)
                break;
            mdelay (1);
        }
        if (count == 0) {
            fprintf (stderr, "Timeout writing memory, aborted. OSCR=%#x\n",
                t->adapter->oscr);
            exit (1);
        }
    }
}

void target_write_word (target_t *t, unsigned phys_addr, unsigned data)
{
    if (debug_level)
        fprintf (stderr, "write word %08x to %08x\n", data, phys_addr);

    /* Allow memory access */
    unsigned oscr_new = (t->adapter->oscr & ~OSCR_RO) | OSCR_SlctMEM;
    if (oscr_new != t->adapter->oscr) {
        t->adapter->oscr = oscr_new;
        t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);
    }

    target_write_next (t, phys_addr, data);
}

/*
 * Чтение слова из памяти.
 */
void target_read_start (target_t *t)
{
    /* Allow memory access */
    unsigned oscr_new = t->adapter->oscr | OSCR_SlctMEM | OSCR_RO;
    if (oscr_new != t->adapter->oscr) {
        t->adapter->oscr = oscr_new;
        t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);
    }
}

unsigned target_read_next (target_t *t, unsigned phys_addr)
{
    unsigned count, data;

    if (phys_addr >= 0xA0000000)
        phys_addr -= 0xA0000000;
    else if (phys_addr >= 0x80000000)
        phys_addr -= 0x80000000;

    t->adapter->oncd_write (t->adapter, phys_addr, OnCD_OMAR, 32);
    t->adapter->oncd_write (t->adapter, 0, OnCD_MEM, 0);
    if (t->is_running) {
        /* Если процессор запущен, обращение к памяти произойдёт не сразу.
         * Надо ждать появления бита RDYm в регистре OSCR. */
        for (count = 100; count != 0; count--) {
            t->adapter->oscr = t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32);
            if (t->adapter->oscr & OSCR_RDYm)
                break;
            mdelay (1);
        }
        if (count == 0) {
            fprintf (stderr, "Timeout reading memory, aborted. OSCR=%#x\n",
                t->adapter->oscr);
            exit (1);
        }
    }
    data = t->adapter->oncd_read (t->adapter, OnCD_OMDR, 32);

    if (debug_level)
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
 * Устанавливаем соединение с адаптером JTAG.
 * Не надо сбрасывать процессор!
 * Программа должна продолжать выполнение.
 */
target_t *target_open (int need_reset)
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
    t->adapter = adapter_open_usb (need_reset);
    if (! t->adapter)
	t->adapter = adapter_open_mpsse ();
    if (! t->adapter)
        t->adapter = adapter_open_bitbang ();
#ifndef __APPLE__
    if (! t->adapter)
        t->adapter = adapter_open_lpt ();
#endif
    if (! t->adapter) {
        fprintf (stderr, "No JTAG adapter found.\n");
        exit (-1);
    }

    /* Проверяем идентификатор процессора. */
    t->idcode = t->adapter->get_idcode (t->adapter);
    if (debug_level)
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
    t->is_running = 1;
    return t;
}

/*
 * Close the device.
 */
void target_close (target_t *t)
{
    if (! t->is_running)
        target_resume (t);
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

const char *target_cpu_name (target_t *t)
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

unsigned target_flash_bytes (target_t *t)
{
    return t->flash_bytes;
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
    unsigned *mf, unsigned *dev, char *mfname, char *chipname,
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
            if (count % 6 == 5) {
                *dev = (unsigned char) (*mf >> 16);
                *mf = (unsigned char) *mf;
            } else {
                *dev = (unsigned char) (*mf >> 8);
                *mf = (unsigned char) *mf;
            }

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
            //fprintf (stderr, "base = %08X, dev = %08X, mf = %08X\n", base, *dev, *mf);

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
#if 0
            // sinvv
            target_write_nwords (t, 3,
                base + t->flash_addr_odd, t->flash_cmd_aa,
                base + t->flash_addr_even, t->flash_cmd_55,
                base + t->flash_addr_odd, t->flash_cmd_f0);
#else
            /* Требуется для чипов Миландр 1636РР2У. */
            target_write_word (t, base, t->flash_cmd_f0);
#endif
        }
        if (debug_level > 1)
            fprintf (stderr, "flash id %08X\n", *dev);

        switch (*dev) {
        case ID_29LV800_B:
            strcpy (chipname, "29LV800B");
            t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
            goto success;
        case ID_29LV800_T:
            strcpy (chipname, "29LV800T");
            t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
            goto success;
        case ID_39VF800_A:
            strcpy (chipname, "39VF800A");
            t->flash_bytes = 2*1024*1024 * t->flash_width / 32;
            goto success;
        case ID_39VF6401_B:
            strcpy (chipname, "39VF6401B");
            t->flash_bytes = 16*1024*1024 * t->flash_width / 32;
            goto success;
        case ID_1636PP2Y:
            strcpy (chipname, "1636PP2Y");
            t->flash_bytes = 4*2*1024*1024;
            goto success;
        case ID_1638PP1:
            strcpy (chipname, "1638PP1");
            t->flash_bytes = 4*128*1024;
            goto success;
        case (unsigned char) ID_1636PP2Y:
            if (t->flash_width != 8)
                break;
            strcpy (chipname, "1636PP2Y");
            t->flash_bytes = 2*1024*1024;
            goto success;
        case ID_S29AL032D:
            strcpy (chipname, "S29AL032D");
            t->flash_bytes = 4*1024*1024;
            goto success;
        }
    }
    /* fprintf (stderr, "Unknown flash id = %08X\n", *dev); */
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
        if (*dev == ID_S29AL032D)
            strcpy (mfname, "Spansion");
        else
            strcpy (mfname, "Milandr");
        break;
    default:
unknown_mfr:
        sprintf (mfname, "<%08X>", *mf);
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
        fprintf (stderr, "target: cannot rewrite word at %x: good %08x bad %08x\n",
            addr, word, bad);
        exit (1);
    }

    switch (t->flash_width) {
    case 8:
        /* Вычисляем нужный байт. */
        for (bad &= ~word; bad && ! (bad & 0xFF); bad >>= 8) {
            addr++;
            word >>= 8;
//fprintf (stderr, "\nbad = %08x, word = %08x, addr=%08x ", bad, word, addr); fflush (stderr);
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

//fprintf (stderr, "target_read_block (addr = %x, nwords = %d)\n", addr, nwords);
    if (t->adapter->read_block) {
        while (nwords > 0) {
            unsigned n = nwords;
            if (n > t->adapter->block_words)
                n = t->adapter->block_words;
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
//fprintf (stderr, "    done (addr = %x)\n", addr);
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
            if (n > t->adapter->block_words)
                n = t->adapter->block_words;
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
//fprintf (stderr, "target_program_block8 (addr = %x, nwords = %d), flash_width = %d, base = %x\n", addr, nwords, t->flash_width, base);
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
//fprintf (stderr, "    done (addr = %x)\n", addr);
}

static void target_program_block32 (target_t *t, unsigned addr,
    unsigned base, unsigned nwords, unsigned *data)
{
    if (t->adapter->program_block32) {
        while (nwords > 0) {
            unsigned n = nwords;
            if (n > t->adapter->program_block_words)
                n = t->adapter->program_block_words;
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
    if (t->adapter->program_block64) {
        while (nwords > 0) {
            unsigned n = nwords;
            if (n > t->adapter->program_block_words)
                n = t->adapter->program_block_words;
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
//fprintf (stderr, "target_program_block (addr = %x, nwords = %d), flash_width = %d, base = %x\n", addr, nwords, t->flash_width, base);

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

/*
 * Проверка состояния процессора, не остановился ли.
 */
int target_is_stopped (target_t *t, int *is_aborted)
{
    *is_aborted = 0;

    /* Задержка на доли секунды. */
    mdelay (100);
    if (! t->adapter->cpu_stopped (t->adapter))
        return 0;

    /* Стоим. */
    if (t->is_running) {
        t->adapter->stop_cpu (t->adapter);
        t->is_running = 0;
        target_save_state (t);
    }

    /* Бит SWO означает, что останов произошёл по команде BREAKD
     * в выполняемой программе. */
    if (t->adapter->oscr & OSCR_SWO)
        *is_aborted = 1;
    return 1;
}

void target_stop (target_t *t)
{
    if (! t->is_running)
        return;
    t->adapter->stop_cpu (t->adapter);
    t->is_running = 0;
    target_save_state (t);
    t->cscon3 = target_read_word (t, MC_CSCON3) & ~MC_CSCON3_ADDR (3);
//fprintf (stderr, "target_stop(), CSCON3 = %08x, oscr = %08x\n", t->cscon3, t->adapter->oncd_read (t->adapter, OnCD_OSCR, 32));
}

void target_step (target_t *t)
{
    if (t->is_running)
        return;
    target_restore_state (t);
    if (t->adapter->step_cpu)
        t->adapter->step_cpu (t->adapter);
    else {
        t->adapter->oncd_write (t->adapter,
            0, OnCD_GO | IRd_FLUSH_PIPE | IRd_STEP_1CLK, 0);
    }
    target_save_state (t);
}

void target_resume (target_t *t)
{
    if (t->is_running)
        return;
    target_restore_state (t);
    t->is_running = 1;
    if (t->adapter->run_cpu)
        t->adapter->run_cpu (t->adapter);
    else {
        t->adapter->oncd_write (t->adapter,
            0, OnCD_GO | IRd_RESUME | IRd_FLUSH_PIPE, 0);
    }
//fprintf (stderr, "target_resume(), oscr = %08x\n", t->adapter->oscr);
}

void target_run (target_t *t, unsigned addr)
{
    if (t->is_running)
        return;

    /* Изменение адреса следующей команды реализуется
     * аналогично входу в отработчик исключения. */
    t->exception = addr;

    target_restore_state (t);
    t->is_running = 1;
#if 0
    /* Очистка конвейера процессора. */
    t->adapter->oscr &= ~(OSCR_TME | OSCR_IME | OSCR_SlctMEM | OSCR_RDYm);
    t->adapter->oscr |= OSCR_MPE;
    t->adapter->oncd_write (t->adapter, t->adapter->oscr, OnCD_OSCR, 32);
#endif
    if (t->adapter->run_cpu)
        t->adapter->run_cpu (t->adapter);
    else {
        t->adapter->oncd_write (t->adapter,
            0, OnCD_GO | IRd_RESUME | IRd_FLUSH_PIPE, 0);
    }
}

void target_restart (target_t *t)
{
    if (! t->is_running)
        target_restore_state (t);
    t->adapter->reset_cpu (t->adapter);
    t->is_running = 1;
}

void target_add_break (target_t *t, unsigned addr, int type)
{
    unsigned obcr, omlr0, omlr1;

    obcr = t->adapter->oncd_read (t->adapter, OnCD_OBCR, 12);
    if (obcr & OBCR_RW0_RW) {
        /* Если одна точка уже есть - перемещаем её на место второй. */
        obcr = (obcr << 1 & OBCR_MBS1) |
            (obcr << 4 & (OBCR_RW1_MASK | OBCR_CC1_MASK));
        omlr1 = t->adapter->oncd_read (t->adapter, OnCD_OMLR0, 32);
    } else {
        obcr = OBCR_ANY;
        omlr1 = 0;
    }

    switch (type) {
    case 'b': obcr |= OBCR_CC0_EQ | OBCR_RW0_READ;              break;
    case 'r': obcr |= OBCR_CC0_EQ | OBCR_RW0_READ  | OBCR_MBS0; break;
    case 'w': obcr |= OBCR_CC0_EQ | OBCR_RW0_WRITE | OBCR_MBS0; break;
    case 'a': obcr |= OBCR_CC0_EQ | OBCR_RW0_RW    | OBCR_MBS0; break;
    }
    omlr0 = addr;

    t->adapter->oncd_write (t->adapter, omlr0, OnCD_OMLR0, 32);
    t->adapter->oncd_write (t->adapter, omlr1, OnCD_OMLR1, 32);
    t->adapter->oncd_write (t->adapter, obcr, OnCD_OBCR, 12);
    t->adapter->oncd_write (t->adapter, 0, OnCD_OMBC, 16);
//fprintf (stderr, "target_add_break (%08x, '%c'), obcr = %03x, omlr0 = %08x, omlr1 = %08x\n", addr, type, obcr, omlr0, omlr1);
}

void target_remove_break (target_t *t, unsigned addr)
{
    unsigned obcr, omlr0, omlr1;

    obcr = t->adapter->oncd_read (t->adapter, OnCD_OBCR, 12);
    omlr0 = t->adapter->oncd_read (t->adapter, OnCD_OMLR0, 32);
    omlr1 = t->adapter->oncd_read (t->adapter, OnCD_OMLR1, 32);

    if ((obcr & OBCR_RW0_RW) && omlr0 == addr) {
        if (obcr & OBCR_RW1_RW) {
            /* Если вторая точка есть - перемещаем её на место первой. */
            obcr = (obcr >> 1 & OBCR_MBS0) |
                (obcr >> 4 & (OBCR_RW0_MASK | OBCR_CC0_MASK));
            omlr0 = omlr1;
            omlr1 = 0;
        } else {
            /* Обнуляем обе точки. */
            obcr = 0;
            omlr0 = 0;
            omlr1 = 0;
        }
    } else if ((obcr & OBCR_RW1_RW) && omlr1 == addr) {
        if (obcr & OBCR_RW0_RW) {
            /* Если первая точка есть - обнуляем вторую. */
            obcr &= ~(OBCR_MBS1 | OBCR_RW1_MASK | OBCR_CC1_MASK);
            omlr1 = 0;
        } else {
            /* Обнуляем обе точки. */
            obcr = 0;
            omlr0 = 0;
            omlr1 = 0;
        }
    } else {
//fprintf (stderr, "target_remove_break (%08x) failed: obcr = %03x, omlr0 = %08x, omlr1 = %08x\n", addr, obcr, omlr0, omlr1);
        return;
    }
    t->adapter->oncd_write (t->adapter, obcr, OnCD_OBCR, 12);
    t->adapter->oncd_write (t->adapter, omlr0, OnCD_OMLR0, 32);
    t->adapter->oncd_write (t->adapter, omlr1, OnCD_OMLR1, 32);
//fprintf (stderr, "target_remove_break (%08x), obcr = %03x, omlr0 = %08x, omlr1 = %08x\n", addr, obcr, omlr0, omlr1);
}
