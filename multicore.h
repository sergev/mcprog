typedef struct _multicore_t multicore_t;

void multicore_init (void);
multicore_t *multicore_open (void);
void multicore_close (multicore_t *mc);
unsigned multicore_idcode (multicore_t *mc);
void multicore_flash_id (multicore_t *mc, unsigned *mf, unsigned *dev);
char *multicore_cpu_name (multicore_t *mc);
char *multicore_flash_name (multicore_t *mc);
int multicore_erase (multicore_t *mc, unsigned long addr);
void multicore_read_start (multicore_t *mc);
unsigned long multicore_read_next (multicore_t *mc, unsigned long addr);
void multicore_write_word (multicore_t *mc, unsigned long addr, unsigned long word);
void multicore_write_8words (multicore_t *mc, unsigned long addr, unsigned long *data);
