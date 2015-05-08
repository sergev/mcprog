 /*
 * Copyright (C) 2010 Serge Vakulenko
 *
 * Реализация интерфейса к процессорам Элвис MIPS32
 * через адаптер JTAG-USB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <getopt.h>
#include <usb.h>

#include "gdbproxy.h"
#include "target.h"

/*
 * Описание архитектуры MIPS32.
 * Нумерация регистров должна совпадать с отладчиком GDB.
 */
#define RP_ELVEES_MIN_ADDRESS           0x0U
#define RP_ELVEES_MAX_ADDRESS           0xFFFFFFFF
#define RP_ELVEES_NUM_REGS              72
#define RP_ELVEES_REG_BYTES             (RP_ELVEES_NUM_REGS*sizeof(unsigned))
#define RP_ELVEES_REGNUM_SP             29 /* Stack pointer */
#define RP_ELVEES_REGNUM_FP             30 /* Frame pointer */
#define RP_ELVEES_REGNUM_STATUS         32 /* Processor status */
#define RP_ELVEES_REGNUM_LO             33
#define RP_ELVEES_REGNUM_HI             34
#define RP_ELVEES_REGNUM_BADVADDR       35
#define RP_ELVEES_REGNUM_CAUSE          36
#define RP_ELVEES_REGNUM_PC             37 /* Program counter */
#define RP_ELVEES_REGNUM_FP0            38 /* FPU registers */
#define RP_ELVEES_REGNUM_FCSR           70 /* FPU status */
#define RP_ELVEES_REGNUM_FIR            71 /* FPU implementation information */

/*
 * Методы целевой платформы.
 */
static int  elvees_open (int argc, char * const argv[],
                        const char *prog_name, log_func log_fn);
static void elvees_close (void);
static int  elvees_connect (char *status_string,
                        size_t status_string_size, int *can_restart);
static int  elvees_disconnect (void);
static void elvees_kill (void);
static int  elvees_restart (void);
static void elvees_stop (void);
static int  elvees_set_gen_thread (rp_thread_ref *thread);
static int  elvees_set_ctrl_thread (rp_thread_ref *thread);
static int  elvees_is_thread_alive (rp_thread_ref *thread, int *alive);
static int  elvees_read_registers (uint8_t *data_buf, uint8_t *avail_buf,
                        size_t buf_size, size_t *read_size);
static int  elvees_write_registers (uint8_t *data_buf, size_t write_size);
static int  elvees_read_single_register (unsigned int reg_no,
                        uint8_t *data_buf, uint8_t *avail_buf,
                        size_t buf_size, size_t *read_size);
static int  elvees_write_single_register (unsigned int reg_no,
                        uint8_t *data_buf, size_t write_size);
static int  elvees_read_mem (uint64_t addr, uint8_t *data_buf,
                        size_t req_size, size_t *actual_size);
static int  elvees_write_mem (uint64_t addr, uint8_t *data_buf,
                        size_t req_sise);
static int  elvees_resume_from_current (int step, int sig);
static int  elvees_resume_from_addr (int step, int sig, uint64_t addr);
static int  elvees_go_waiting (int sig);
static int  elvees_wait_partial (int first, char *status_string,
                        size_t status_string_len, out_func out,
                        int *implemented, int *more);
static int  elvees_wait (char *status_string, size_t status_string_len,
                        out_func out, int *implemented);
static int  elvees_process_query (unsigned int *mask,
                        rp_thread_ref *arg, rp_thread_info *info);
static int  elvees_list_query (int first, rp_thread_ref *arg,
                        rp_thread_ref *result, size_t max_num,
                        size_t *num, int *done);
static int  elvees_current_thread_query (rp_thread_ref *thread);
static int  elvees_offsets_query (uint64_t *text,
                        uint64_t *data, uint64_t *bss);
static int  elvees_crc_query (uint64_t addr, size_t len, uint32_t *val);
static int  elvees_raw_query (char *in_buf, char *out_buf,
                        size_t out_buf_size);
static int  elvees_remcmd (char *in_buf, out_func of, data_func df);
static int  elvees_add_break (int type, uint64_t addr, unsigned int len);
static int  elvees_remove_break (int type, uint64_t addr, unsigned int len);

/*
 * Удалённые команды, специфические для конкретной платформы.
 */
static int elvees_rcmd_help (int argc, char *argv[], out_func of, data_func df);

#define RCMD(name, hlp) {#name, elvees_rcmd_##name, hlp}  //table entry generation

/*
 * Структура цаблицы удалённых команд.
 */
typedef struct {
    const char *name;                                   // command name
    int (*function)(int, char**, out_func, data_func);  // function to call
    const char *help;                                   // one line of help text
} RCMD_TABLE;

/*
 * Глобальный описатель целевой платформы.
 */
rp_target elvees_target = {
    NULL,       /* next */
    "elvees",
    "Elvees MIPS32 processor",
    NULL,       /* help*/
    elvees_open,
    elvees_close,
    elvees_connect,
    elvees_disconnect,
    elvees_kill,
    elvees_restart,
    elvees_stop,
    elvees_set_gen_thread,
    elvees_set_ctrl_thread,
    elvees_is_thread_alive,
    elvees_read_registers,
    elvees_write_registers,
    elvees_read_single_register,
    elvees_write_single_register,
    elvees_read_mem,
    elvees_write_mem,
    elvees_resume_from_current,
    elvees_resume_from_addr,
    elvees_go_waiting,
    elvees_wait_partial,
    elvees_wait,
    elvees_process_query,
    elvees_list_query,
    elvees_current_thread_query,
    elvees_offsets_query,
    elvees_crc_query,
    elvees_raw_query,
    elvees_remcmd,
    elvees_add_break,
    elvees_remove_break
};

static struct {
    /* Start up parameters, set by elvees_open */
    log_func    log;
    target_t    *device;
} target;

/* Local functions */
static char *elvees_out_treg (char *in, unsigned int reg_no);

#ifdef NDEBUG
#define DEBUG_OUT(...)
#else
static void DEBUG_OUT(const char *string,...)
{
    va_list args;
    va_start (args, string);
    fprintf (stderr, "debug: ");
    vfprintf (stderr, string, args);
    fprintf (stderr, "\n");
    va_end (args);
}
#endif

/*
 * Target method.
 * Установление соединения с JTAG-адаптером.
 * Вызывается при старте mcremote и каждый раз после отсоединения отладчика.
 */
static int elvees_open(int argc,
                         char * const argv[],
                         const char *prog_name,
                         log_func log_fn)
{
    /* Using USB JTAG adapter. */
    /* const char *port ="usb0"; */

    /* Option descriptors */
    static struct option long_options[] =
    {
        /* Options setting flag */
        {NULL, 0, 0, 0}
    };

    assert (prog_name != NULL);
    assert (log_fn != NULL);

    /* Set log */
    target.log = log_fn;

    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_open()",
                        elvees_target.name);

    /* Process options */
    for (;;) {
        int c;
        int option_index;

        c = getopt_long(argc, argv, "+", long_options, &option_index);
        if (c == EOF)
            break;
        switch (c) {
        case 0:
            /* Long option which just sets a flag */
            break;
        default:
            target.log(RP_VAL_LOGLEVEL_NOTICE,
                                "%s: Use `%s --help' to see a complete list of options",
                                elvees_target.name,
                                prog_name);
            return RP_VAL_TARGETRET_ERR;
        }
    }

    /*
    if (optind == (argc - 1)) {
        port = argv[optind];
    }
    */

    if (! target.device) {
        /* Если первый раз - соединяемся с адаптером.
         * Не надо давать SYSRST процессору! */
        target.device = target_open (0, 0);
        if (! target.device) {
            target.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: failed to initialize JTAG adapter",
                            elvees_target.name);
            return RP_VAL_TARGETRET_ERR;
        }
    }
    return RP_VAL_TARGETRET_OK;
}

/*
 * Target method.
 * Вызывается при каждом отключении отладчика от mcremote.
 * Надо пустить процессор.
 */
static void elvees_close(void)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_close()",
                        elvees_target.name);
    assert (target.device != 0);

    target_resume (target.device);
}

/*
 * Target method.
 * Вызывается при каждом подключении отладчика к mcremote.
 * Надо остановить процессор.
 */
static int elvees_connect(char *status_string,
                            size_t status_string_len,
                            int *can_restart)
{
    char *cp;

    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_connect()",
                        elvees_target.name);

    assert (target.device != 0);

    assert (status_string != NULL);
    assert (status_string_len >= 34);
    assert (can_restart != NULL);

    *can_restart = TRUE;

    target_stop (target.device);

    /* Fill out the the status string */
    sprintf(status_string, "T%02d", RP_SIGNAL_BREAKPOINT);

    cp = elvees_out_treg(&status_string[3], RP_ELVEES_REGNUM_PC);
    cp = elvees_out_treg(cp, RP_ELVEES_REGNUM_FP);
    return RP_VAL_TARGETRET_OK;
}

/*
 * Target method.
 * Отсоединение от адаптера.
 * Можно ничего не делать.
 */
static int elvees_disconnect(void)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_disconnect()",
                        elvees_target.name);
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static void elvees_kill(void)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_kill()",
                        elvees_target.name);

    /* Kill the target debug session. */
}

static int elvees_restart(void)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_restart()",
                        elvees_target.name);
    assert (target.device != 0);

    target_restart (target.device);
    return RP_VAL_TARGETRET_OK;
}

/*
 * Target method: Stop (i,e, break) the target program.
 */
static void elvees_stop(void)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_stop()",
                        elvees_target.name);
    assert (target.device != 0);

    target_stop (target.device);
}

static int elvees_set_gen_thread(rp_thread_ref *thread)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_set_gen_thread()",
                        elvees_target.name);

    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_set_ctrl_thread(rp_thread_ref *thread)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_set_ctrl_thread()",
                        elvees_target.name);

    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_is_thread_alive(rp_thread_ref *thread, int *alive)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_is_thread_alive()",
                        elvees_target.name);

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_read_registers(uint8_t *data_buf,
                                 uint8_t *avail_buf,
                                 size_t buf_size,
                                 size_t *read_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_read_registers()",
                        elvees_target.name);
    assert (target.device != 0);
    assert (data_buf != NULL);
    assert (avail_buf != NULL);
    assert (buf_size >= RP_ELVEES_REG_BYTES);
    assert (read_size != NULL);

    /* Выдаём только регистры CPU и CP0.
     * Регистры FPU отладчик запросит отдельно, при необхоимости. */
    int i;
    for (i=0; i<RP_ELVEES_REGNUM_FP0; i++) {
        unsigned val = target_read_register (target.device, i);
        memcpy (data_buf + i*sizeof(unsigned), &val, sizeof(unsigned));
        memset (avail_buf + i*sizeof(unsigned), 1, sizeof(unsigned));
    }
    *read_size = RP_ELVEES_REG_BYTES;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_write_registers(uint8_t *buf, size_t write_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_write_registers()",
                        elvees_target.name);
    assert (target.device != 0);
    assert (buf != NULL);
    assert (write_size > 0);
    assert (write_size <= RP_ELVEES_REG_BYTES);

    /* Write the registers to the target. */
    int i;
    for (i=0; i<RP_ELVEES_NUM_REGS; i++) {
        unsigned offset = i * sizeof(unsigned);
        if (offset + sizeof(unsigned) > write_size)
            break;
        unsigned val = *(unsigned*) (buf + offset);
        target_write_register (target.device, i, val);
    }
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_read_single_register(unsigned int reg_no,
                                         uint8_t *data_buf,
                                         uint8_t *avail_buf,
                                         size_t buf_size,
                                         size_t *read_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_read_single_register (%d)",
                        elvees_target.name, reg_no);
    assert (target.device != 0);
    assert (data_buf != NULL);
    assert (avail_buf != NULL);
    assert (buf_size >= RP_ELVEES_REG_BYTES);
    assert (read_size != NULL);

    if (reg_no < 0 || reg_no >= RP_ELVEES_NUM_REGS)
        return RP_VAL_TARGETRET_ERR;

    unsigned val = target_read_register (target.device, reg_no);

    memcpy (data_buf, &val, sizeof(unsigned));
    memset (avail_buf, 1, sizeof(unsigned));
    *read_size = sizeof(unsigned);
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_write_single_register(unsigned int reg_no,
                                          uint8_t *buf,
                                          size_t write_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_write_single_register (%d, 0x%X)",
                        elvees_target.name,
                        reg_no, *(unsigned*) buf);
    assert (target.device != 0);
    assert (buf != NULL);
    assert (write_size == 4);

    if (reg_no < 0 || reg_no >= RP_ELVEES_NUM_REGS)
        return RP_VAL_TARGETRET_ERR;

    unsigned val = *(unsigned*) buf;
//target.log(RP_VAL_LOGLEVEL_DEBUG, "  write 0x%X to reg%d\n", val, reg_no);
    target_write_register (target.device, reg_no, val);
    return RP_VAL_TARGETRET_OK;
}

/*
 * Target method.
 * Чтение требуемого количества байтов из памяти устройства.
 */
static int elvees_read_mem(uint64_t addr,
                             uint8_t *buf,
                             size_t req_size,
                             size_t *actual_size)
{
//    target.log(RP_VAL_LOGLEVEL_DEBUG,
//        "%s: elvees_read_mem(0x%llX, ptr, %d, ptr)",
//        elvees_target.name, addr, req_size);
    assert (target.device != 0);
    assert (buf != NULL);
    assert (req_size > 0);
    assert (actual_size != NULL);

    if (addr > RP_ELVEES_MAX_ADDRESS) {
        target.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            elvees_target.name,
                            addr);

        return RP_VAL_TARGETRET_ERR;
    }

    if (addr + req_size > RP_ELVEES_MAX_ADDRESS + 1ULL)
        req_size = RP_ELVEES_MAX_ADDRESS + 1ULL - addr;
    *actual_size = req_size;

    unsigned offset = (addr & 3);
    if (offset != 0) {
        /* Адрес не на границе слова. */
        unsigned data;
        unsigned nbytes = 4 - offset;
        if (nbytes > req_size)
            nbytes = req_size;
        data = target_read_word (target.device, addr & ~3);
        switch (offset) {
        case 1:
            buf[0] = data >> 8;
            if (nbytes > 1)
                buf[1] = data >> 16;
            if (nbytes > 2)
                buf[2] = data >> 24;
            break;
        case 2:
            buf[0] = data >> 16;
            if (nbytes > 1)
                buf[1] = data >> 24;
            break;
        case 3:
            buf[0] = data >> 24;
            break;
        }
        req_size -= nbytes;
        if (req_size <= 0)
            return RP_VAL_TARGETRET_OK;
        addr += nbytes;
        buf += nbytes;
    }
    if (req_size >= 4) {
        /* Массив слов. */
        unsigned nwords = req_size/4;
        target_read_block (target.device, addr, nwords, (unsigned*) buf);
        req_size &= 3;
        if (req_size <= 0)
            return RP_VAL_TARGETRET_OK;
        addr += nwords * 4;
        buf += nwords * 4;
    }
    /* Последнее нецелое слово. */
    unsigned data = target_read_word (target.device, addr);
    memcpy (buf, (unsigned char*) &data, req_size);
    return RP_VAL_TARGETRET_OK;
}

/*
 * Target method.
 * Запись требуемого количества байтов в память устройства.
 */
static int elvees_write_mem(uint64_t addr,
                              uint8_t *buf,
                              size_t write_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_write_mem(0x%llX, ptr, %d)",
                        elvees_target.name,
                        addr,
                        write_size);
    assert (target.device != 0);
    assert (buf != NULL);

    /* GDB does zero length writes for some reason. Treat them harmlessly. */
    if (write_size == 0)
        return RP_VAL_TARGETRET_OK;

    if (addr > RP_ELVEES_MAX_ADDRESS)
    {
        target.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            elvees_target.name,
                            addr);
        return RP_VAL_TARGETRET_ERR;
    }

    if ((addr + write_size - 1) > RP_ELVEES_MAX_ADDRESS)
    {
        target.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address/write_size 0x%llx/0x%x",
                            elvees_target.name,
                            addr,
                            write_size);
        return RP_VAL_TARGETRET_ERR;
    }

    unsigned offset = (addr & 3);
    if (offset != 0) {
        /* Адрес не на границе слова.
         * Читаем слово и компонуем новое значение. */
        unsigned data;
        unsigned nbytes = 4 - offset;
        if (nbytes > write_size)
            nbytes = write_size;
        data = target_read_word (target.device, addr & ~3);
        switch (offset) {
        case 1:
            data &= ~0x0000ff00;
            data |= buf[0] << 8;
            if (nbytes > 1) {
                data &= ~0x00ff0000;
                data |= buf[1] << 16;
            }
            if (nbytes > 2) {
                data &= ~0xff000000;
                data |= buf[2] << 24;
            }
            break;
        case 2:
            data &= ~0x00ff0000;
            data |= buf[0] << 16;
            if (nbytes > 1) {
                data &= ~0xff000000;
                data |= buf[1] << 24;
            }
            break;
        case 3:
            data &= ~0xff000000;
            data |= buf[0] << 24;
            break;
        }
        target_write_word (target.device, addr & ~3, data);
        write_size -= nbytes;
        if (write_size <= 0)
            return RP_VAL_TARGETRET_OK;
        addr += nbytes;
        buf += nbytes;
    }
    if (write_size >= 4) {
        /* Массив слов. */
        unsigned nwords = write_size/4;
        target_write_block (target.device, addr, nwords, (unsigned*) buf);
        write_size &= 3;
        if (write_size <= 0)
            return RP_VAL_TARGETRET_OK;
        addr += nwords * 4;
        buf += nwords * 4;
    }
    /* Последнее нецелое слово. */
    unsigned data = target_read_word (target.device, addr);
    memcpy ((unsigned char*) &data, buf, write_size);
    target_write_word (target.device, addr, data);

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_resume_from_current(int step, int sig)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_resume_from_current(%s, %d)",
                        elvees_target.name,
                        (step)  ?  "step"  :  "run",
                        sig);
    assert (target.device != 0);

    if (step) {
        /* Single step the target */
        target_step (target.device);
    } else {
        /* Run the target to a breakpoint, or until we stop it. */
        target_resume (target.device);
    }
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_resume_from_addr(int step, int sig, uint64_t addr)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_resume_from_addr(%s, %d, 0x%llX)",
                        elvees_target.name,
                        (step)  ?  "step"  :  "run",
                        sig,
                        addr);
    assert (target.device != 0);

    /* Run the target from the new PC address. */
    target_run (target.device, addr);
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_go_waiting(int sig)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_go_waiting()",
                        elvees_target.name);
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_wait_partial(int first, char *status_string,
    size_t status_string_len, out_func of,
    int *implemented, int *more)
{
    char *cp;
    int sig;

    assert (target.device != 0);
    assert (status_string != NULL);
    assert (status_string_len >= 34);
    assert (of != NULL);
    assert (implemented != NULL);
    assert (more != NULL);

    *implemented = TRUE;

    /* Test the target state (i.e. running/stopped) without blocking */
    int is_aborted;
    if (! target_is_stopped (target.device, &is_aborted)) {
        *more = TRUE;
//        target.log(RP_VAL_LOGLEVEL_DEBUG,
//                        "%s: elvees_wait_partial() cpu is running",
//                        elvees_target.name);
//fprintf (stderr, "."); fflush (stderr);
        return RP_VAL_TARGETRET_OK;
    }

    if (is_aborted) {
        sig = RP_SIGNAL_ABORTED;
        target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_wait_partial() cpu is aborted",
                        elvees_target.name);
    } else {
        sig = RP_SIGNAL_BREAKPOINT;
        target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_wait_partial() cpu stopped on breakpoint",
                        elvees_target.name);
    }

    /* Fill out the status string */
    sprintf(status_string, "T%02d", sig);

    cp = elvees_out_treg(&status_string[3], RP_ELVEES_REGNUM_PC);
    cp = elvees_out_treg(cp, RP_ELVEES_REGNUM_FP);
    *more = FALSE;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_wait(char *status_string,
                         size_t status_string_len,
                         out_func of,
                         int *implemented)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_wait() - not implemented",
                        elvees_target.name);

    /* Блокирующее ожидание не нужно. */
    *implemented = FALSE;
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_process_query(unsigned int *mask,
                                  rp_thread_ref *arg,
                                  rp_thread_info *info)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_process_query()",
                        elvees_target.name);

    /* Does your target support threads? Is so, implement this function.
       Otherwise just return no support. */
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_list_query(int first,
                               rp_thread_ref *arg,
                               rp_thread_ref *result,
                               size_t max_num,
                               size_t *num,
                               int *done)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_list_query()",
                        elvees_target.name);

    /* Does your target support threads? Is so, implement this function.
       Otherwise just return no support. */
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_current_thread_query(rp_thread_ref *thread)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_current_thread_query()",
                        elvees_target.name);

    /* Does your target support threads? Is so, implement this function.
       Otherwise just return no support. */
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int elvees_offsets_query(uint64_t *text, uint64_t *data, uint64_t *bss)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_offsets_query()",
                        elvees_target.name);
    assert (target.device != 0);
    assert (text != NULL);
    assert (data != NULL);
    assert (bss != NULL);

    /* Is this what *your* target really needs? */
    *text = 0;
    *data = 0;
    *bss = 0;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int elvees_crc_query(uint64_t addr, size_t len, uint32_t *val)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_crc_query()",
                        elvees_target.name);
    assert (target.device != 0);

    if (addr > RP_ELVEES_MAX_ADDRESS ||
        addr + len > RP_ELVEES_MAX_ADDRESS + 1) {
        target.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            elvees_target.name,
                            addr);
        return RP_VAL_TARGETRET_ERR;
    }

    target.log(RP_VAL_LOGLEVEL_ERR,
                        "%s: crc not implemented", elvees_target.name);
    return RP_VAL_TARGETRET_ERR;
}

/* Target method */
static int elvees_raw_query(char *in_buf, char *out_buf, size_t out_buf_size)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_raw_query(\"%s\")",
                        elvees_target.name, in_buf);

    return RP_VAL_TARGETRET_NOSUPP;
}

static int tohex(char *s, const char *t)
{
    int i;
    static char hex[] = "0123456789abcdef";

    i = 0;
    while (*t)
    {
        *s++ = hex[(*t >> 4) & 0x0f];
        *s++ = hex[*t & 0x0f];
        t++;
        i++;
    }
    *s = '\0';
    return i;
}

/* command: erase flash */
static int elvees_rcmd_erase(int argc, char *argv[], out_func of, data_func df)
{
    char buf[1000 + 1];

    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_rcmd_erase()",
                        elvees_target.name);
    tohex(buf, "Erasing target flash - ");
    of(buf);

    /* TODO: perform the erase. */

    tohex(buf, " Erased OK\n");
    of(buf);
    return RP_VAL_TARGETRET_OK;
}

/* Table of commands */
static const RCMD_TABLE remote_commands[] =
{
    RCMD(help,      "This help text"),

    RCMD(erase,     "Erase target flash memory"),
    {0,0,0}     //sentinel, end of table marker
};

/* Help function, generate help text from command table */
static int elvees_rcmd_help(int argc, char *argv[], out_func of, data_func df)
{
    char buf[1000 + 1];
    char buf2[1000 + 1];
    int i = 0;

    tohex(buf, "Remote command help:\n");
    of(buf);
    for (i = 0;  remote_commands[i].name;  i++)
    {
#ifdef WIN32
        sprintf(buf2, "%-10s %s\n", remote_commands[i].name, remote_commands[i].help);
#else
        snprintf(buf2, 1000, "%-10s %s\n", remote_commands[i].name, remote_commands[i].help);
#endif
        tohex(buf, buf2);
        of(buf);
    }
    return RP_VAL_TARGETRET_OK;
}

/* Decode nibble */
static int remote_decode_nibble(const char *in, unsigned int *nibble)
{
    unsigned int nib;

    if ((nib = rp_hex_nibble(*in)) >= 0)
    {
        *nibble = nib;
        return  TRUE;
    }

    return  FALSE;
}


/* Decode byte */
static int remote_decode_byte(const char *in, unsigned int *byte)
{
    unsigned int ls_nibble;
    unsigned int ms_nibble;

    if (!remote_decode_nibble(in, &ms_nibble))
        return  FALSE;

    if (!remote_decode_nibble(in + 1, &ls_nibble))
        return  FALSE;

    *byte = (ms_nibble << 4) + ls_nibble;

    return  TRUE;
}


/* Target method */
#define MAXARGS 4
static int elvees_remcmd(char *in_buf, out_func of, data_func df)
{
    int count = 0;
    int i;
    char *args[MAXARGS];
    char *ptr;
    unsigned int ch;
    char buf[1000 + 1];
    char *s;

    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_remcmd()",
                        elvees_target.name);
    DEBUG_OUT("command '%s'", in_buf);

    if (strlen(in_buf))
    {
        /* There is something to process */
        /* TODO: Handle target specific commands, such as flash erase, JTAG
                 control, etc. */
        /* A single example "flash erase" command is partially implemented
           here as an example. */

        /* Turn the hex into ASCII */
        ptr = in_buf;
        s = buf;
        while (*ptr)
        {
            if (remote_decode_byte(ptr, &ch) == 0)
                return RP_VAL_TARGETRET_ERR;
            *s++ = ch;
            ptr += 2;
        }
        *s = '\0';
        DEBUG_OUT("command '%s'", buf);

        /* Split string into separate arguments */
        ptr = buf;
        args[count++] = ptr;
        while (*ptr)
        {
            /* Search to the end of the string */
            if (*ptr == ' ')
            {
                /* Space is the delimiter */
                *ptr = 0;
                if (count >= MAXARGS)
                    return RP_VAL_TARGETRET_ERR;
                args[count++] = ptr + 1;
            }
            ptr++;
        }
        /* Search the command table, and execute the function if found */
        DEBUG_OUT("executing target dependant command '%s'", args[0]);

        for (i = 0;  remote_commands[i].name;  i++)
        {
            if (strcmp(args[0], remote_commands[i].name) == 0)
                return remote_commands[i].function(count, args, of, df);
        }
        return RP_VAL_TARGETRET_NOSUPP;
    }
    return RP_VAL_TARGETRET_ERR;
}


/* Target method */
static int elvees_add_break(int type, uint64_t addr, unsigned int len)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
        "%s: elvees_add_break(%d, 0x%llx, %d)",
        elvees_target.name, type, addr, len);
    assert (target.device != 0);
    switch (type) {
    case 1:             /* hardware-breakpoint */
        target_add_break (target.device, addr, 'b');
        return RP_VAL_TARGETRET_OK;
    case 2:             /* write watchpoint */
        target_add_break (target.device, addr, 'w');
        return RP_VAL_TARGETRET_OK;
    case 3:             /* read watchpoint */
        target_add_break (target.device, addr, 'r');
        return RP_VAL_TARGETRET_OK;
    case 4:             /* access watchpoint */
        target_add_break (target.device, addr, 'a');
        return RP_VAL_TARGETRET_OK;
    default:
        return RP_VAL_TARGETRET_NOSUPP;
    }
}

/* Target method */
static int elvees_remove_break(int type, uint64_t addr, unsigned int len)
{
    target.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: elvees_remove_break(%d, 0x%llx, %d)",
                        elvees_target.name,
                        type,
                        addr,
                        len);
    assert (target.device != 0);
    target_remove_break (target.device, addr);
    return RP_VAL_TARGETRET_OK;
}

/* Output registers in the format suitable
   for TAAn:r...;n:r...;  format */
static char *elvees_out_treg(char *in, unsigned int reg_no)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t reg_val;

    if (in == NULL)
        return NULL;

    assert (reg_no < RP_ELVEES_NUM_REGS);

    *in++ = hex[(reg_no >> 4) & 0x0f];
    *in++ = hex[reg_no & 0x0f];
    *in++ = ':';

    reg_val = target_read_register (target.device, reg_no);
//fprintf (stderr, "reg%d = %08x\n", reg_no, reg_val);

    /* The register goes into the buffer in little-endian order */
    *in++ = hex[(reg_val >> 4) & 0x0f];  *in++ = hex[reg_val & 0x0f];
    *in++ = hex[(reg_val >> 12) & 0x0f]; *in++ = hex[(reg_val >> 8) & 0x0f];
    *in++ = hex[(reg_val >> 20) & 0x0f]; *in++ = hex[(reg_val >> 16) & 0x0f];
    *in++ = hex[(reg_val >> 28) & 0x0f]; *in++ = hex[(reg_val >> 24) & 0x0f];
    *in++ = ';';
    *in   = '\0';

    return in;
}
