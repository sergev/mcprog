typedef struct _multicore_t multicore_t;

void multicore_init (void);
multicore_t *multicore_open (void);
void multicore_close (multicore_t *mc);
unsigned multicore_idcode (multicore_t *mc);
char *multicore_cpu_name (multicore_t *mc);
int multicore_flash_detect (multicore_t *mc, unsigned base,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width);
int multicore_erase (multicore_t *mc, unsigned long addr);
void multicore_read_start (multicore_t *mc);
unsigned long multicore_read_next (multicore_t *mc, unsigned long addr);
void multicore_flash_write (multicore_t *mc, unsigned long addr, unsigned long word);
void multicore_write_word (multicore_t *mc, unsigned long addr, unsigned long word);
void multicore_write_next (multicore_t *mc, unsigned long addr, unsigned long word);
