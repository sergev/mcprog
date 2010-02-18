typedef struct _multicore_t multicore_t;

void multicore_init (void);
multicore_t *multicore_open (void);
void multicore_close (multicore_t *mc);
unsigned multicore_idcode (multicore_t *mc);
char *multicore_cpu_name (multicore_t *mc);
unsigned multicore_flash_width (multicore_t *mc);
void multicore_flash_configure (multicore_t *mc, unsigned first, unsigned last);
unsigned multicore_flash_next (multicore_t *mc, unsigned prev, unsigned *last);
int multicore_flash_detect (multicore_t *mc, unsigned base,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width);
int multicore_erase (multicore_t *mc, unsigned addr);
void multicore_read_start (multicore_t *mc);
unsigned multicore_read_next (multicore_t *mc, unsigned addr);
void multicore_read_nwords (multicore_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
void multicore_flash_write (multicore_t *mc, unsigned addr, unsigned word);
int multicore_flash_rewrite (multicore_t *mc, unsigned addr, unsigned word);
void multicore_write_word (multicore_t *mc, unsigned addr, unsigned word);
void multicore_write_next (multicore_t *mc, unsigned addr, unsigned word);
