/*
 * Обобщённый JTAG-адаптер.
 */
#include <stdarg.h>

typedef struct _adapter_t adapter_t;

struct _adapter_t {
    const char *name;

    /* Регистр управления блоком отладки. */
    unsigned oscr;

    /*
     * Обязательные функции.
     */
    void (*close) (adapter_t *a);
    unsigned (*get_idcode) (adapter_t *a);
    int (*cpu_stopped) (adapter_t *a);
    void (*stop_cpu) (adapter_t *a);
    void (*reset_cpu) (adapter_t *a);
    void (*oncd_write) (adapter_t *a, unsigned val, int reg, int nbits);
    unsigned (*oncd_read) (adapter_t *a, int reg, int nbits);

    /*
     * Расширенные возможности.
     */
    unsigned block_words;
    unsigned program_block_words;

    void (*step_cpu) (adapter_t *a);
    void (*run_cpu) (adapter_t *a);
    void (*read_block) (adapter_t *adapter,
        unsigned nwords, unsigned addr, unsigned *data);
    void (*write_block) (adapter_t *adapter,
        unsigned nwords, unsigned addr, unsigned *data);
    void (*write_nwords) (adapter_t *adapter, unsigned nwords, va_list args);
    void (*program_block32) (adapter_t *adapter,
        unsigned nwords, unsigned base, unsigned addr, unsigned *data,
        unsigned addr_odd, unsigned addr_even,
        unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0);
    void (*program_block32_unprotect) (adapter_t *adapter,
        unsigned nwords, unsigned base, unsigned addr, unsigned *data,
        unsigned addr_odd, unsigned addr_even,
        unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0);
    void (*program_block32_protect) (adapter_t *adapter,
        unsigned nwords, unsigned base, unsigned addr, unsigned *data,
        unsigned addr_odd, unsigned addr_even,
        unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0);
    void (*program_block64) (adapter_t *adapter,
        unsigned nwords, unsigned base, unsigned addr, unsigned *data,
        unsigned addr_odd, unsigned addr_even,
        unsigned cmd_aa, unsigned cmd_55, unsigned cmd_a0);
};

adapter_t *adapter_open_usb (void);
adapter_t *adapter_open_lpt (void);
adapter_t *adapter_open_bitbang (void);
adapter_t *adapter_open_mpsse (void);

void mdelay (unsigned msec);
extern int debug_level;
