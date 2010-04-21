/*
 * Описание регистров отладочного блока OnCD.
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
/* Регистр IRd команды JTAG */
#define IR_EXTEST		0x00
#define IR_SAMPLE_PRELOAD	0x11
#define IR_IDCODE		0x33
#define IR_DEBUG_REQUEST	0x44
#define IR_DEBUG_ENABLE		0x55
#define IR_BYPASS		0xff

/* Биты регистра IRd */
#define	IRd_RUN		0x20	/* 0 - step mode, 1 - run continuosly */
#define	IRd_READ	0x40	/* 0 - write, 1 - read registers */
#define	IRd_FLUSH_PIPE	0x40	/* for EnGO: instruction pipe changed */
#define	IRd_STEP_1CLK	0x80	/* for step mode: run for 1 clock only */

/* Младшие биты IRd: номер регистра OnCD */
#define	OnCD_OSCR	0x00	/* Control & State Register */
#define	OnCD_OMBC	0x01	/* BreakPoint Match Counter */
#define	OnCD_OMLR0	0x02	/* Address Boundary Register 0 */
#define	OnCD_OMLR1	0x03	/* Address Boundary Register 1 */
#define	OnCD_OBCR	0x04	/* BreakPoint Control Register */
#define	OnCD_IRdec	0x05	/* Last CPU Instruction, can be supplied. */
#define	OnCD_OTC	0x06	/* Trace Counter */
#define	OnCD_PCdec	0x07	/* Decoding Instruction(IRdec) Address */
#define	OnCD_PCexec	0x08	/* Executing Instruction Address */
#define	OnCD_PCmem	0x09	/* Memory Access Instruction Address */
#define	OnCD_PCfetch	0x0A	/* PC (Fetching Instruction Address) */
#define	OnCD_OMAR	0x0B	/* Memory Address Register */
#define	OnCD_OMDR	0x0C	/* Memory Data Register */
#define	OnCD_MEM	0x0D	/* Memory Access Register (EnMEM) */
#define	OnCD_PCwb	0x0E	/* Address of instruction at write back stage */
#define	OnCD_MEMACK	0x0E	/* Memory Operation Acknowlege (EnXX) */
#define	OnCD_EXIT	0x0F	/* Exit From Debug Mode (EnGO) */

/* OSCR Register */
#define	OSCR_SlctMEM	0x0001	/* Allow Memory Access */
#define OSCR_RO		0x0002	/* 0: Write, 1: Read */
#define OSCR_TME	0x0004	/* Trace Mode Enable */
#define OSCR_IME	0x0008	/* Debug Interrupt Enable */
#define OSCR_MPE	0x0010	/* Flash CPU Pipeline At Debug Exit */
#define OSCR_RDYm	0x0020	/* RDY signal state */
#define OSCR_MBO	0x0040	/* BreakPoint triggered */
#define OSCR_TO		0x0080	/* Trace Counter triggered */
#define OSCR_SWO	0x0100	/* SoftWare enter into Debug mode */
#define OSCR_SO		0x0200	/* CPU Mode, 0: running, 1: Debug */
#define OSCR_DBM	0x0400	/* Debug Mode On, mc12 and above */
#define OSCR_NDS	0x0800	/* Do not stop in delay slot, mc12 and above */
#define OSCR_VBO	0x1000	/* Exception catched, mc12 and above */
#define OSCR_NFEXP	0x2000	/* Do not raise exception at pc fetch */
#define OSCR_WP0	0x4000	/* WP0 triggered, only for mc12 and above */
#define OSCR_WP1	0x8000	/* WP1 triggered, only for mc12 and above */
