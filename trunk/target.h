typedef struct _target_t target_t;

void target_init (void);
target_t *target_open (void);
void target_close (target_t *mc);
unsigned target_idcode (target_t *mc);
char *target_cpu_name (target_t *mc);
unsigned target_flash_width (target_t *mc);
void target_flash_configure (target_t *mc, unsigned first, unsigned last);
unsigned target_flash_next (target_t *mc, unsigned prev, unsigned *last);
int target_flash_detect (target_t *mc, unsigned base,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width);
int target_erase (target_t *mc, unsigned addr);
unsigned target_read_word (target_t *mc, unsigned addr);
void target_read_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
void target_write_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
void target_program_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
int target_flash_rewrite (target_t *mc, unsigned addr, unsigned word);
void target_write_word (target_t *mc, unsigned addr, unsigned word);
void target_write_next (target_t *mc, unsigned addr, unsigned word);
