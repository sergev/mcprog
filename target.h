typedef struct _target_t target_t;

target_t *target_open (void);
void target_close (target_t *mc);

unsigned target_idcode (target_t *mc);
char *target_cpu_name (target_t *mc);

unsigned target_flash_width (target_t *mc);
void target_flash_configure (target_t *mc, unsigned first, unsigned last);
int target_flash_detect (target_t *mc, unsigned base,
	unsigned *mf, unsigned *dev, char *mfname, char *devname,
	unsigned *bytes, unsigned *width);
unsigned target_flash_next (target_t *mc, unsigned prev, unsigned *last);

int target_erase (target_t *mc, unsigned addr);
void target_program_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
int target_flash_rewrite (target_t *mc, unsigned addr, unsigned word);

void target_read_start (target_t *t);
unsigned target_read_next (target_t *t, unsigned addr);
unsigned target_read_word (target_t *mc, unsigned addr);
void target_read_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);

void target_write_word (target_t *mc, unsigned addr, unsigned word);
void target_write_next (target_t *mc, unsigned addr, unsigned word);
void target_write_block (target_t *mc, unsigned addr,
	unsigned nwords, unsigned *data);
void target_write_nwords (target_t *t, unsigned nwords, ...);
void target_write_byte (target_t *t, unsigned addr, unsigned data);
void target_write_2bytes (target_t *t, unsigned addr1, unsigned data1,
	unsigned addr2, unsigned data2);
