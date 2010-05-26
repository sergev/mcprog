/* Copyright (C) 2002 Chris Liechti and Steve Underwood
   Copyright (C) 2010 Aurelien VALADE

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

     1. Redistributions of source code must retain the above copyright notice,
        this list of conditions and the following disclaimer.
     2. Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
     3. The name of the author may not be used to endorse or promote products
        derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
   EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
   OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
   ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


   Implementation of a dummy `skeleton' target for the GDB proxy server.

   Exported Data:
     skeletone_target            - target descriptor of the `skeleton' target

   Imported Data:
     None

   Static Data:
     msp430_XXXX               - static data representing status and
                                   parameters of the target

   Global Functions:
     None

   Static Functions:
     msp430_XXXX              - methods comprising the `msp430' target.
                                  A description is in file gdbproxy.h

     msp430_                  - local finctions
     msp430_command

   $Id: target_msp430.c,v 1.0 2010/04/05 15:00:00 wolvi.lataniere Exp $ */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>

#include "gdbproxy.h"
#include "libmsp430_interface.h"

#if WIN32 || defined(__DARWIN__)
#include "getopt.h"
#endif

#if defined(__FreeBSD__)
#include "getopt.h"
#endif


/* Note: we are using prefix 'msp430' for static stuff in
   order to simplify debugging of the target code itself */

#define RP_MSP430_MIN_ADDRESS             0x0U
#define RP_MSP430_MAX_ADDRESS             0xFFFFU
#define RP_MSP430_REG_DATATYPE            uint16_t
#define RP_MSP430_REG_BYTES               (16*sizeof(uint16_t))
#define RP_MSP430_NUM_REGS                16
#define RP_MSP430_REGNUM_PC               0  /* Program counter reg. number */
#define RP_MSP430_REGNUM_SP               1  /* Stack pointer reg. number */
#define RP_MSP430_REGNUM_FP               4  /* Frame pointer reg. number */

#define RP_MSP430_MAX_BREAKPOINTS         8

/* Some example states a real target might support. */
#define RP_MSP430_TARGET_STATE_RUNNING                    1
#define RP_MSP430_TARGET_STATE_STOPPED                    0
#define RP_MSP430_TARGET_STATE_SINGLE_STEP_COMPLETE       2
#define RP_MSP430_TARGET_STATE_BREAKPOINT_HIT             3

/*
 * Target methods, static
 */
static void  msp430_help(const char *prog_name);
static int   msp430_open(int argc,
                           char * const argv[],
                           const char *prog_name,
                           log_func log_fn);
static void  msp430_close(void);
static int   msp430_connect(char *status_string,
                              size_t status_string_size,
                              int *can_restart);
static int   msp430_disconnect(void);
static void  msp430_kill(void);
static int   msp430_restart(void);
static void  msp430_stop(void);
static int   msp430_set_gen_thread(rp_thread_ref *thread);
static int   msp430_set_ctrl_thread(rp_thread_ref *thread);
static int   msp430_is_thread_alive(rp_thread_ref *thread, int *alive);
static int   msp430_read_registers(uint8_t *data_buf,
                                     uint8_t *avail_buf,
                                     size_t buf_size,
                                     size_t *read_size);
static int   msp430_write_registers(uint8_t *data_buf, size_t write_size);
static int   msp430_read_single_register(unsigned int reg_no,
                                           uint8_t *data_buf,
                                           uint8_t *avail_buf,
                                           size_t buf_size,
                                           size_t *read_size);
static int   msp430_write_single_register(unsigned int reg_no,
                                            uint8_t *data_buf,
                                            size_t write_size);
static int   msp430_read_mem(uint64_t addr,
                               uint8_t *data_buf,
                               size_t req_size,
                               size_t *actual_size);
static int   msp430_write_mem(uint64_t addr,
                                uint8_t *data_buf,
                                size_t req_sise);
static int   msp430_resume_from_current(int step, int sig);
static int   msp430_resume_from_addr(int step,
                                       int sig,
                                       uint64_t addr);
static int   msp430_go_waiting(int sig);
static int   msp430_wait_partial(int first,
                                   char *status_string,
                                   size_t status_string_len,
                                   out_func out,
                                   int *implemented,
                                   int *more);
static int   msp430_wait(char *status_string,
                           size_t status_string_len,
                           out_func out,
                           int *implemented);
static int   msp430_process_query(unsigned int *mask,
                                    rp_thread_ref *arg,
                                    rp_thread_info *info);
static int   msp430_list_query(int first,
                                 rp_thread_ref *arg,
                                 rp_thread_ref *result,
                                 size_t max_num,
                                 size_t *num,
                                 int *done);
static int   msp430_current_thread_query(rp_thread_ref *thread);
static int   msp430_offsets_query(uint64_t *text,
                                    uint64_t *data,
                                    uint64_t *bss);
static int   msp430_crc_query(uint64_t addr,
                                size_t len,
                                uint32_t *val);
static int   msp430_raw_query(char *in_buf,
                                char *out_buf,
                                size_t out_buf_size);
static int   msp430_remcmd(char *in_buf, out_func of, data_func df);
static int   msp430_add_break(int type, uint64_t addr, unsigned int len);
static int   msp430_remove_break(int type, uint64_t addr, unsigned int len);

//Table of remote commands following
static int msp430_rcmd_help(int argc, char *argv[], out_func of, data_func df);  //proto for table entry

static uint32_t crc32(uint8_t *buf, size_t len, uint32_t crc);

#define RCMD(name, hlp) {#name, msp430_rcmd_##name, hlp}  //table entry generation

//Table entry definition
typedef struct
{
    const char *name;                                   // command name
    int (*function)(int, char**, out_func, data_func);  // function to call
    const char *help;                                   // one line of help text
} RCMD_TABLE;


static MSP430_DEVICE deviceID;

/*
 * Global target descriptor
 */
rp_target msp430_target =
{
    NULL,      /* next */
    "msp430",
    "msp430 target using FreeMSP430 DebuggerLink Library",
    msp430_help,
    msp430_open,
    msp430_close,
    msp430_connect,
    msp430_disconnect,
    msp430_kill,
    msp430_restart,
    msp430_stop,
    msp430_set_gen_thread,
    msp430_set_ctrl_thread,
    msp430_is_thread_alive,
    msp430_read_registers,
    msp430_write_registers,
    msp430_read_single_register,
    msp430_write_single_register,
    msp430_read_mem,
    msp430_write_mem,
    msp430_resume_from_current,
    msp430_resume_from_addr,
    msp430_go_waiting,
    msp430_wait_partial,
    msp430_wait,
    msp430_process_query,
    msp430_list_query,
    msp430_current_thread_query,
    msp430_offsets_query,
    msp430_crc_query,
    msp430_raw_query,
    msp430_remcmd,
    msp430_add_break,
    msp430_remove_break
};

struct msp430_status_s
{
    /* Start up parameters, set by msp430_open */
    log_func    log;
    int         is_open;

    /* Tell wait_xxx method the notion of whether or not
       previously called resume is supported */
    int         target_running;
    int         target_interrupted;
    RP_MSP430_REG_DATATYPE    registers[RP_MSP430_NUM_REGS];
    uint64_t                    breakpoints[RP_MSP430_MAX_BREAKPOINTS];
};

static struct msp430_status_s msp430_status =
{
    NULL,
    FALSE,
    FALSE,
    FALSE,
    {0},
    {0}
};

/* Local functions */
static char *msp430_out_treg(char *in, unsigned int reg_no);
static int refresh_registers(void);

/* Target method */

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

static void msp430_help(const char *prog_name)
{
    printf("This is the msp430 target for the GDB proxy server. Usage:\n\n");
    printf("  %s [options] %s [msp430-options] [port]\n",
           prog_name,
           msp430_target.name);
    printf("\nOptions:\n\n");
    printf("  --debug              run %s in debug mode\n", prog_name);
    printf("  --help               `%s --help %s'  prints this message\n",
           prog_name,
           msp430_target.name);
    printf("  --port=PORT          use the specified TCP port\n");
    printf("\nmsp430-options:\n\n");

    printf("\n");

    return;
}

/* Target method */
static int msp430_open(int argc,
                         char * const argv[],
                         const char *prog_name,
                         log_func log_fn)
{
  char *port = "USB";

    /* Option descriptors */
    static struct option long_options[] =
    {
        /* Options setting flag */
        {NULL, 0, 0, 0}
    };

    assert(!msp430_status.is_open);
    assert(prog_name != NULL);
    assert(log_fn != NULL);

    /* Set log */
    msp430_status.log = log_fn;

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_open()",
                        msp430_target.name);

    /* Process options */
    for (;;)
    {
        int c;
        int option_index;

        c = getopt_long(argc, argv, "+", long_options, &option_index);
        if (c == EOF)
            break;
        switch (c)
        {
        case 0:
            /* Long option which just sets a flag */
            break;
        default:
            msp430_status.log(RP_VAL_LOGLEVEL_NOTICE,
                                "%s: Use `%s --help %s' to see a complete list of options",
                                msp430_target.name,
                                prog_name,
                                msp430_target.name);
            return RP_VAL_TARGETRET_ERR;
        }
    }

    if (optind == (argc - 1))
    {
        port = argv[optind];
    }
    else if (optind != argc)
    {
        /* Bad number of arguments */
        msp430_status.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad number of arguments",
                            msp430_target.name);
        msp430_target.help(prog_name);

        return RP_VAL_TARGETRET_ERR;
    }

    if (!msp430_status.is_open)
    {
      guint32 guVersion;
      /* Initialize the MSP430 Debugger */
      if (MSP430_Initialize(port, &guVersion) != 0)
	{
	  guint32 error_no = MSP430_Error_Number();
	  msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			    "%s: MSP430 debugger initialization failed",
			    msp430_target.name);
	  msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			    "%s: error (%d) : %s",
			    msp430_target.name, error_no, MSP430_Error_String(error_no));

	  return RP_VAL_TARGETRET_ERR;
	}
    }

    /* Setup the Target VCC */
    if (MSP430_VCC(3000) != 0)
      {
	guint32 error_no = MSP430_Error_Number();
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: MSP430 debugger Vcc Setup failed",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: error (%d) : %s",
			  msp430_target.name, error_no, MSP430_Error_String(error_no));

	return RP_VAL_TARGETRET_ERR;

      }

    if (MSP430_Configure(8,0)!=0)
      {
	guint32 error_no = MSP430_Error_Number();
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: MSP430 debugger Configuration failed",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: error (%d) : %s",
			  msp430_target.name, error_no, MSP430_Error_String(error_no));

	return RP_VAL_TARGETRET_ERR;   /* configure the Debugger */
      }


    if (MSP430_Identify(&deviceID, sizeof(deviceID), 0)!=0)
      {
	guint32 error_no = MSP430_Error_Number();
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: MSP430 target identification failed",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: error (%d) : %s",
			  msp430_target.name, error_no, MSP430_Error_String(error_no));

	return RP_VAL_TARGETRET_ERR;   /* Get the MSP430 Device */
      }



    if (MSP430_Configure(0,1)!=0)
      {
	guint32 error_no = MSP430_Error_Number();
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: MSP430 Debugger configuration failed",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: error (%d) : %s",
			  msp430_target.name, error_no, MSP430_Error_String(error_no));

	return RP_VAL_TARGETRET_ERR;
      }

    /* Set up initial default values */
    msp430_status.target_running = FALSE;
    msp430_status.target_interrupted = FALSE;
    memset (msp430_status.registers, 0, sizeof(msp430_status.registers));
    memset (msp430_status.breakpoints, 0, sizeof(msp430_status.breakpoints));

    msp430_status.is_open = TRUE;

    msp430_write_single_register(0, &(deviceID.MainStart), 2);

    return RP_VAL_TARGETRET_OK;
}


/* Target method */
static void msp430_close(void)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_close()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    if (MSP430_Close(3000) != 0)
      {
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error closing the debugger connection.",
			  msp430_target.name);
	return;
      }

    msp430_status.is_open = FALSE;
}

/* Target method */
static int msp430_connect(char *status_string,
                            size_t status_string_len,
                            int *can_restart)
{
    char *cp;

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_connect()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(status_string != NULL);
    assert(status_string_len >= 34);
    assert(can_restart != NULL);

    *can_restart = TRUE;

    /* Fill out the the status string */
    sprintf(status_string, "T%02d", RP_SIGNAL_ABORTED);

    if (refresh_registers())
        return RP_VAL_TARGETRET_ERR;

    cp = msp430_out_treg(&status_string[3], RP_MSP430_REGNUM_PC);
    cp = msp430_out_treg(cp, RP_MSP430_REGNUM_FP);

    return (cp != NULL)  ?  RP_VAL_TARGETRET_OK  :  RP_VAL_TARGETRET_ERR;
}

/* Target method */
static int msp430_disconnect(void)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_disconnect()",
                        msp430_target.name);

    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static void msp430_kill(void)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_kill()",
                        msp430_target.name);

    /* TODO: Kill the target debug session. */
}

static int msp430_restart(void)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_restart()",
                        msp430_target.name);

    /* Just stop it. The actual restart will be done
       when connect is called again */
    msp430_stop();

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static void msp430_stop(void)
{
  int state;
  int read;
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_stop()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    MSP430_State(&state,1,&read);

    msp430_status.target_interrupted = TRUE;
}

static int msp430_set_gen_thread(rp_thread_ref *thread)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_set_gen_thread()",
                        msp430_target.name);

    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_set_ctrl_thread(rp_thread_ref *thread)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_set_ctrl_thread()",
                        msp430_target.name);

    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_is_thread_alive(rp_thread_ref *thread, int *alive)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_is_thread_alive()",
                        msp430_target.name);

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_read_registers(uint8_t *data_buf,
                                 uint8_t *avail_buf,
                                 size_t buf_size,
                                 size_t *read_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_read_registers()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(data_buf != NULL);
    assert(avail_buf != NULL);
    assert(buf_size >= RP_MSP430_REG_BYTES);
    assert(read_size != NULL);

    if (refresh_registers())
        return RP_VAL_TARGETRET_ERR;

    memcpy(data_buf, msp430_status.registers, RP_MSP430_REG_BYTES);
    memset(avail_buf, 1, RP_MSP430_REG_BYTES);
    *read_size = RP_MSP430_REG_BYTES;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_write_registers(uint8_t *buf, size_t write_size)
{
  int i;
  int regs[RP_MSP430_NUM_REGS];

  msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_write_registers()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(buf != NULL);
    assert(write_size > 0);
    assert(write_size <= RP_MSP430_REG_BYTES);

    memcpy(msp430_status.registers, buf, write_size);

    for (i=0; i<RP_MSP430_NUM_REGS; i++)
      {
	regs[i] = (int)msp430_status.registers[i];
      }

    if(MSP430_Registers((char*)regs, 0xFFFF, 0)!=0)
      {
	int errno = MSP430_Error_Number();
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Regisers Write KO",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));

	return RP_VAL_TARGETRET_ERR;
      }

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_read_single_register(unsigned int reg_no,
                                         uint8_t *data_buf,
                                         uint8_t *avail_buf,
                                         size_t buf_size,
                                         size_t *read_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_read_single_register()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(data_buf != NULL);
    assert(avail_buf != NULL);
    assert(buf_size >= RP_MSP430_REG_BYTES);
    assert(read_size != NULL);

    if (reg_no < 0  ||  reg_no > RP_MSP430_NUM_REGS)
        return RP_VAL_TARGETRET_ERR;

    if (refresh_registers())
        return RP_VAL_TARGETRET_ERR;

    memcpy(data_buf, &msp430_status.registers[reg_no], sizeof(msp430_status.registers[reg_no]));
    memset(avail_buf, 1, sizeof(msp430_status.registers[reg_no]));
    *read_size = sizeof(msp430_status.registers[reg_no]);
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_write_single_register(unsigned int reg_no,
                                          uint8_t *buf,
                                          size_t write_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_write_single_register(%d, 0x%X)",
                        msp430_target.name,
                        reg_no,
                        ((RP_MSP430_REG_DATATYPE *) buf)[0]);

    assert(msp430_status.is_open);

    assert(buf != NULL);
    assert(write_size == 2);

    if (reg_no < 0  ||  reg_no > RP_MSP430_NUM_REGS)
        return RP_VAL_TARGETRET_ERR;

    msp430_status.registers[reg_no] = ((RP_MSP430_REG_DATATYPE *) buf)[0];

    return msp430_write_registers(msp430_status.registers, RP_MSP430_REG_BYTES);
}

/* Target method */
static int msp430_read_mem(uint64_t addr,
                             uint8_t *buf,
                             size_t req_size,
                             size_t *actual_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_read_mem(0x%llX, ptr, %d, ptr)",
                        msp430_target.name,
                        addr,
                        req_size);

    assert(msp430_status.is_open);

    assert(buf != NULL);
    assert(req_size > 0);
    assert(actual_size != NULL);

    if (addr > RP_MSP430_MAX_ADDRESS)
    {
        msp430_status.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            msp430_target.name,
                            addr);

        return RP_VAL_TARGETRET_ERR;
    }

    if (addr + req_size > RP_MSP430_MAX_ADDRESS + 1)
        *actual_size = RP_MSP430_MAX_ADDRESS + 1 - addr;
    else
        *actual_size = req_size;

    if (MSP430_Memory(addr, buf, req_size, 1)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Memory Read error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));

	return RP_VAL_TARGETRET_ERR;
      }

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_write_mem(uint64_t addr,
                              uint8_t *buf,
                              size_t write_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_write_mem(0x%llX, ptr, %d)",
                        msp430_target.name,
                        addr,
                        write_size);

    assert(msp430_status.is_open);
    assert(buf != NULL);

    /* GDB does zero length writes for some reason. Treat them harmlessly. */
    if (write_size == 0)
        return RP_VAL_TARGETRET_OK;

    if (addr > RP_MSP430_MAX_ADDRESS)
    {
        msp430_status.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            msp430_target.name,
                            addr);
        return RP_VAL_TARGETRET_ERR;
    }

    if ((addr + write_size - 1) > RP_MSP430_MAX_ADDRESS)
    {
        msp430_status.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address/write_size 0x%llx/0x%x",
                            msp430_target.name,
                            addr,
                            write_size);
        return RP_VAL_TARGETRET_ERR;
    }

    if (MSP430_Memory(addr, buf, write_size, 0)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Memory Write error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));
	return RP_VAL_TARGETRET_ERR;
      }

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_resume_from_current(int step, int sig)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_resume_from_current(%s, %d)",
                        msp430_target.name,
                        (step)  ?  "step"  :  "run",
                        sig);

    assert(msp430_status.is_open);

    if (step)
      {
	/* Single step the target */
	if (MSP430_Run(2,0)!=0)
	  {
	    int errno = MSP430_Error_Number();

	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Single Step error",
			      msp430_target.name);
	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Error (%d) : %s", errno,
			      MSP430_Error_String(errno));
	    return RP_VAL_TARGETRET_ERR;
	  }
      }
    else
      {
	/* Run the target to a breakpoint, or until we stop it. */;
	if (MSP430_Run(3,0)!=0)
	  {
	    int errno = MSP430_Error_Number();

	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Target resume error",
			      msp430_target.name);
	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Error (%d) : %s", errno,
			      MSP430_Error_String(errno));
	    return RP_VAL_TARGETRET_ERR;
	  }
      }

    msp430_status.target_running = TRUE;
    msp430_status.target_interrupted = FALSE;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */

static int msp430_resume_from_addr(int step, int sig, uint64_t addr)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_resume_from_addr(%s, %d, 0x%llX)",
                        msp430_target.name,
                        (step)  ?  "step"  :  "run",
                        sig,
                        addr);

    assert(msp430_status.is_open);

    msp430_status.registers[RP_MSP430_REGNUM_PC] = addr;

    msp430_write_registers(msp430_status.registers, RP_MSP430_REG_BYTES);

    if (MSP430_Run(3,0)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Target resume error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));
	return RP_VAL_TARGETRET_ERR;
      }

    msp430_status.target_running = TRUE;
    msp430_status.target_interrupted = FALSE;
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_go_waiting(int sig)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_go_waiting()",
                        msp430_target.name);
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_wait_partial(int first,
                                 char *status_string,
                                 size_t status_string_len,
                                 out_func of,
                                 int *implemented,
                                 int *more)
{
    int state;
    int read;
    char *cp;
    int sig;

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_wait_partial()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(status_string != NULL);
    assert(status_string_len >= 34);
    assert(of != NULL);
    assert(implemented != NULL);
    assert(more != NULL);

    *implemented = TRUE;

    if (!msp430_status.target_running)
        return RP_VAL_TARGETRET_NOSUPP;

#ifdef WIN32
    Sleep((first)  ?  500  :  100);
#else
    usleep((first)  ?  500000  :  100000);
#endif
    if (MSP430_State(&state, 0, &read)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Read state error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));
	return RP_VAL_TARGETRET_ERR;
      }

    if (state == RP_MSP430_TARGET_STATE_RUNNING)
    {
        *more = TRUE;
        return RP_VAL_TARGETRET_OK;
    }

    switch (state)
    {
    case RP_MSP430_TARGET_STATE_STOPPED:
        if (msp430_status.target_interrupted)
            sig = RP_SIGNAL_INTERRUPT;
        else
            sig = RP_SIGNAL_ABORTED;
        break;
    case RP_MSP430_TARGET_STATE_RUNNING:
        *more = TRUE;
        return RP_VAL_TARGETRET_OK;
    case RP_MSP430_TARGET_STATE_SINGLE_STEP_COMPLETE:
        sig = RP_SIGNAL_BREAKPOINT;
        break;
    case RP_MSP430_TARGET_STATE_BREAKPOINT_HIT:
        sig = RP_SIGNAL_BREAKPOINT;
        break;
    default:
        msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                            "%s: unexpected state %d for the MSP430",
                            msp430_target.name,
                            state);
        sig = RP_SIGNAL_ABORTED;
        break;
    }
    /* Fill out the status string */
    sprintf(status_string, "T%02d", sig);

    if (refresh_registers())
        return RP_VAL_TARGETRET_ERR;

    cp = msp430_out_treg(&status_string[3], RP_MSP430_REGNUM_PC);
    cp = msp430_out_treg(cp, RP_MSP430_REGNUM_FP);

    *more = FALSE;

    return (cp != NULL)  ?  RP_VAL_TARGETRET_OK  :  RP_VAL_TARGETRET_ERR;
}

/* Target method */
static int msp430_wait(char *status_string,
                         size_t status_string_len,
                         out_func of,
                         int *implemented)
{
    int state;
    int read;
    char *cp;
    int sig;

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_wait()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(status_string != NULL);
    assert(status_string_len >= 34);
    assert(of != NULL);
    assert(implemented != NULL);

    *implemented = TRUE;

    if (!msp430_status.target_running)
        return RP_VAL_TARGETRET_NOSUPP;

    do {
      if (MSP430_State(&state, 0, &read)!=0)
	{
	  int errno = MSP430_Error_Number();

	  msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			    "%s: State read error",
			    msp430_target.name);
	  msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			    "%s: Error (%d) : %s", errno,
			    MSP430_Error_String(errno));
	  return RP_VAL_TARGETRET_ERR;
	}
#if WIN32
      Sleep(1000);
#else
      sleep(1);
#endif
    } while (state>0);

    //state = RP_MSP430_TARGET_STATE_STOPPED;

    switch (state)
    {
    case RP_MSP430_TARGET_STATE_STOPPED:
        sig = RP_SIGNAL_ABORTED;
        break;
    case RP_MSP430_TARGET_STATE_SINGLE_STEP_COMPLETE:
        sig = RP_SIGNAL_BREAKPOINT;
        break;
    case RP_MSP430_TARGET_STATE_BREAKPOINT_HIT:
        sig = RP_SIGNAL_BREAKPOINT;
        break;
    default:
        msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                            "%s: unexpected state %d for the MSP430",
                            msp430_target.name,
                            state);
        sig = RP_SIGNAL_ABORTED;
        break;
    }
    /* Fill out the status string */
    sprintf(status_string, "T%02d", sig);

    if (refresh_registers())
        return RP_VAL_TARGETRET_ERR;

    cp = msp430_out_treg(&status_string[3], RP_MSP430_REGNUM_PC);
    cp = msp430_out_treg(cp, RP_MSP430_REGNUM_FP);

    return (cp != NULL)  ?  RP_VAL_TARGETRET_OK  :  RP_VAL_TARGETRET_ERR;
}

/* Target method */
static int msp430_process_query(unsigned int *mask,
                                  rp_thread_ref *arg,
                                  rp_thread_info *info)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_process_query()",
                        msp430_target.name);
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_list_query(int first,
                               rp_thread_ref *arg,
                               rp_thread_ref *result,
                               size_t max_num,
                               size_t *num,
                               int *done)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_list_query()",
                        msp430_target.name);
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_current_thread_query(rp_thread_ref *thread)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_current_thread_query()",
                        msp430_target.name);
    return RP_VAL_TARGETRET_NOSUPP;
}

/* Target method */
static int msp430_offsets_query(uint64_t *text, uint64_t *data, uint64_t *bss)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_offsets_query()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    assert(text != NULL);
    assert(data != NULL);
    assert(bss != NULL);

    *text = 0; /*deviceID.MainStart;*/
    *data = 0; /*deviceID.MainStart;*/
    *bss = 0;  /*deviceID.RamStart;*/
    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_crc_query(uint64_t addr, size_t len, uint32_t *val)
{
    uint8_t buf[1];

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_crc_query()",
                        msp430_target.name);

    assert(msp430_status.is_open);

    if (addr > RP_MSP430_MAX_ADDRESS  ||  addr + len > RP_MSP430_MAX_ADDRESS + 1)
    {
        msp430_status.log(RP_VAL_LOGLEVEL_ERR,
                            "%s: bad address 0x%llx",
                            msp430_target.name,
                            addr);

        return RP_VAL_TARGETRET_ERR;
    }

    if (MSP430_Memory(addr, len, &len, 1)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Memory Write error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));

	return RP_VAL_TARGETRET_ERR;
      }

    *val = crc32(buf, sizeof(buf), 0xFFFFFFFF);

    return RP_VAL_TARGETRET_OK;
}

/* Target method */
static int msp430_raw_query(char *in_buf, char *out_buf, size_t out_buf_size)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_raw_query()",
                        msp430_target.name);

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
static int msp430_rcmd_erase(int argc, char *argv[], out_func of, data_func df)
{
    char buf[1000 + 1];

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_rcmd_erase()",
                        msp430_target.name);
    tohex(buf, "Erasing target flash - ");
    of(buf);

    if (argc==2 && strcmp(argv[1], "all")==0)
      {
	if (MSP430_Erase(2, 0x0FFE0, 2)!=0)
	  {
	    int errno = MSP430_Error_Number();

	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Memory erase error",
			      msp430_target.name);
	    msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			      "%s: Error (%d) : %s", errno,
			      MSP430_Error_String(errno));
	    return RP_VAL_TARGETRET_ERR;
	  }
      }
    else
      return RP_VAL_TARGETRET_NOSUPP;

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
static int msp430_rcmd_help(int argc, char *argv[], out_func of, data_func df)
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
static int msp430_remcmd(char *in_buf, out_func of, data_func df)
{
    int count = 0;
    int i;
    char *args[MAXARGS];
    char *ptr;
    unsigned int ch;
    char buf[1000 + 1];
    char *s;

    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_remcmd()",
                        msp430_target.name);
    DEBUG_OUT("command '%s'", in_buf);

    if (strlen(in_buf))
    {
        /* There is something to process */

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
static int msp430_add_break(int type, uint64_t addr, unsigned int len)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_add_break(%d, 0x%llx, %d)",
                        msp430_target.name,
                        type,
                        addr,
                        len);
    if (MSP430_Breakpoint(type ,addr)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Breakpoint add error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));

	return RP_VAL_TARGETRET_ERR;
      }

    return RP_VAL_TARGETRET_OK;
}


/* Target method */
static int msp430_remove_break(int type, uint64_t addr, unsigned int len)
{
    msp430_status.log(RP_VAL_LOGLEVEL_DEBUG,
                        "%s: msp430_remove_break(%d, 0x%llx, %d)",
                        msp430_target.name,
                        type,
                        addr,
                        len);

    if (MSP430_Breakpoint(type, 0)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Breakpoint remove error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));

	return RP_VAL_TARGETRET_ERR;
      }
    return RP_VAL_TARGETRET_OK;
}

/* Output registers in the format suitable
   for TAAn:r...;n:r...;  format */
static char *msp430_out_treg(char *in, unsigned int reg_no)
{
    static const char hex[] = "0123456789abcdef";
    int16_t reg_val;

    if (in == NULL)
        return NULL;

    assert(reg_no < RP_MSP430_NUM_REGS);

    *in++ = hex[(reg_no >> 4) & 0x0f];
    *in++ = hex[reg_no & 0x0f];
    *in++ = ':';

    reg_val = msp430_status.registers[reg_no];
    /* The register goes into the buffer in little-endian order */
    *in++ = hex[(reg_val >> 4) & 0x0f];
    *in++ = hex[reg_val & 0x0f];
    *in++ = hex[(reg_val >> 12) & 0x0f];
    *in++ = hex[(reg_val >> 8) & 0x0f];
    *in++ = ';';
    *in   = '\0';

    return in;
}

/* Table used by the crc32 function to calcuate the checksum. */
static uint32_t crc32_table[256] =
{
    0,
    0
};

static uint32_t crc32(uint8_t *buf, size_t len, uint32_t crc)
{
    if (!crc32_table[1])
    {
        /* Initialize the CRC table and the decoding table. */
        int i;
        int j;
        unsigned int c;

        for (i = 0; i < 256; i++)
	{
	    for (c = i << 24, j = 8; j > 0; --j)
	        c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : (c << 1);
	    crc32_table[i] = c;
	}
    }

    while (len--)
    {
        crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ *buf) & 255];
        buf++;
    }
    return crc;
}

static int refresh_registers(void)
{
    int i;
    int regs[RP_MSP430_NUM_REGS];

    if (MSP430_Registers((char*)regs, 0xFFFF,1)!=0)
      {
	int errno = MSP430_Error_Number();

	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Registers read error",
			  msp430_target.name);
	msp430_status.log(RP_VAL_LOGLEVEL_ERR,
			  "%s: Error (%d) : %s", errno,
			  MSP430_Error_String(errno));
	return FALSE;
      }

    for (i=0; i<RP_MSP430_NUM_REGS; i++)
      {
	msp430_status.registers[i] = (int)regs[i];
      }

    return  FALSE;
}
