/*
 * Команды и регистры процессора MIPS32.
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

/*
 * Opcodes for some mips instructions
 */
#define	MIPS_JR		0x8		/* jr rs; */
#define	MIPS_JAL	(0x3 << 26)	/* jal 0; jump and link */
#define	MIPS_ADD	0x20		/* add rd, rs, rt */
#define	MIPS_ADDI	(0x8 << 26)	/* addi rt << 16, rs << 21, imed */
#define	MIPS_ADDIU	(0x8 << 26)	/* addiu rt << 16, rs << 21, imed */
#define MIPS_MFHI	0x10		/* mfhi, (rd << 11) */
#define MIPS_MFLO	0x12		/* mflo, (rd << 11) */
#define MIPS_MTHI	0x11		/* mthi, (rd << 21) */
#define MIPS_MTLO	0x13		/* mtlo, (rd << 21) */
#define MIPS_SLL	0x0		/* sll dest << 11, src << 16, sa << 6 */
#define MIPS_SRL	(0x0 | 0x3)	/* srl dest << 11, src << 16, sa << 6 */
#define MIPS_NOP	MIPS_SLL	/* nop (SLL, r0, r0, 0) */
#define	MIPS_OR		37		/* add rd, rs, rt */
#define MIPS_ORI	(0xd << 26)	/* ori rt << 16, rs << 21, imed */
#define MIPS_MFC0	(0x10 << 26)	/* mfc0 rt << 16, rd << 11, sel */
#define MIPS_MTC0	((0x10<<26)|(4<<21))/* mtc0 rt << 16, rd << 11, sel */
#define MIPS_TLBP	0x42000008	/* tlbp */
#define MIPS_TLBR	0X42000001	/* tlbr */
#define MIPS_TLBWI	0X42000002	/* tlbwi */
#define MIPS_TLBWR	0X42000006	/* tlbwr */
#define MIPS_LUI	(15 << 26)	/* lui (r << 16) imm */
#define MIPS_SW		(0x2b << 26)	/* sw (rbase << 21) (rto << 16) offt */
#define MIPS_LW		(0x23 << 26)	/* lw (rbase << 21) (rto << 16) offt */
#define MIPS_SWC1	(0x39 << 26)	/* swc1 (rbase << 21) (rto << 16) offt */
#define MIPS_LWC1	(0x31 << 26)	/* lwc1 (rbase << 21) (rto << 16) offt */
#define MIPS_MTC1	((0x11<<26)|(4<<21))/* mtc0 rt << 16, rd << 11, sel */
#define MIPS_CTC1	((0x11<<26)|(6<<21))/* ctc1 rt << 16, fs << 11 */
#define MIPS_CFC1	((0x11<<26)|(2<<21))/* cfc1 rt << 16, fs << 11 */

/*
 * CP0 defines
 */
#define CP0_INDEX	0
#define CP0_RANDOM	1
#define CP0_ENTRYLO0	2
#define CP0_ENTRYLO1	3
#define CP0_CONTEXT	4
#define CP0_PAGEMASK	5
#define CP0_WIRED	6
#define CP0_7		7
#define CP0_BADVADDR	8
#define CP0_COUNT	9
#define CP0_ENTRYHI	10
#define CP0_COMPARE	11
#define CP0_STATUS	12
#define CP0_CAUSE	13
#define CP0_EPC		14
#define CP0_PRID	15
#define CP0_CONFIG	16
#define CP0_LLADDR	17
#define CP0_WATCHLO	18
#define CP0_WATCHHI	19
#define CP0_20		20
#define CP0_21		21
#define CP0_22		22
#define CP0_DEBUG	23
#define CP0_DEPC	24
#define CP0_PERFCNT	25
#define CP0_ERRCTL	26
#define CP0_CACHEERR	27
#define CP0_TAGLO	28
#define CP0_TAGHI	29
#define CP0_ERROREPC	30
#define CP0_DESAVE	31

/*
 * Status register.
 */
#define ST_IE		0x00000001	/* разрешение прерываний */
#define ST_EXL		0x00000002	/* уровень исключения */
#define ST_ERL		0x00000004	/* уровень ошибки */
#define ST_UM		0x00000010	/* режим пользователя */
#define ST_IM_SW0	0x00000100	/* программное прерывание 0 */
#define ST_IM_SW1	0x00000200	/* программное прерывание 1 */
#define ST_IM_IRQ0	0x00000400	/* внешнее прерывание 0 */
#define ST_IM_IRQ1	0x00000800	/* внешнее прерывание 1 */
#define ST_IM_IRQ2	0x00001000	/* внешнее прерывание 2 */
#define ST_IM_IRQ3	0x00002000	/* внешнее прерывание 3 */
#define ST_IM_MCU	0x00008000	/* от внутренних устройств микроконтроллера */
#define ST_NMI		0x00080000	/* причина сброса - NMI */
#define ST_TS		0x00200000	/* TLB-закрытие системы */
#define ST_BEV		0x00400000	/* размещение векторов: начальная загрузка */

#define ST_CU0		0x10000000	/* разрешение сопроцессора 0 */
#define ST_CU1		0x20000000	/* разрешение сопроцессора 1 (FPU) */

/*
 * Coprocessor 1 (FPU) registers.
 */
#define CP1_FIR		0	/* implementation and revision */
#define CP1_FCCR	25	/* condition codes */
#define CP1_FEXR	26	/* exceptions */
#define CP1_FENR	28	/* enables */
#define CP1_FCSR	31	/* control/status */
