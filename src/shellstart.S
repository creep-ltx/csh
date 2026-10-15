| shellstart.S - the start of cshell, csh as the system's user shell
| (docs/design.md "csh as the user shell"): libnix's resident start-up
| (nlrcrt0.S, M.Fleischer and G.Nikl) changed in three places.
|
|  - A shell process is sent a start-up packet on pr_MsgPort before it may
|    use DOS (the Guru Book 14.5.1): taken first, before any library code
|    could send a packet of its own and get it as an answer. Kept in
|    __shellpkt for main.c.
|  - Every start gets its own copy of the data (one code for every Shell
|    window: a user shell must be pure), a4 pointing at it; tc_TrapData too.
|  - No Workbench path, no command line (a shell has none).
|  - __keepdata set: the data stays allocated at the end (jobs csh leaves
|    running use it).
|  - Built with -DCSH_CMDSTART (csh, "Resident csh C:csh"): started as a
|    command, so no start-up packet to wait for and no BootShell check;
|    __shellpkt stays 0 and main.c runs as plain csh, the code shared.
|
| Two entries for code that runs in other tasks of csh's (job.c): they
| have no a4 of their own.
|   _stage_stub  a pipe stage's process: a0 = its argument, the hex address
|                of its struct stage, whose first long is a4
|   _relay_stub  the break relay task: a4 in its tc_UserData

.LVOForbid	=	-132
.LVOAllocMem	=	-198
.LVOFreeMem	=	-210
.LVOPutMsg	=	-366
.LVOGetMsg	=	-372
.LVOWaitPort	=	-384
.LVOCopyMemQuick =	-630

tc_TrapData	=	46
tc_UserData	=	88
pr_MsgPort	=	92
ThisTask	=	276
Attn		=	0x128
ln_Name		=	10
dp_Port		=	4

		.globl	_exit
		.globl	__exit
		.text

start:
| The first Shell of a boot is the ROM's: it puts its internal commands
| (Run, CD, Echo...) into the resident list when it starts, and nothing
| else does. With none there yet (no "CD"), this start goes to the ROM
| Shell's own code (the BootShell entry), the start-up packet untaken.
| For cshell in a ROM (rominit.S): the boot Shell starts through "shell".
		movem.l	d0/a0,-(sp)
		movel	4:W,a6
		lea	dosname(pc),a1
		moveq	#36,d0
		jsr	-552(a6)		| OpenLibrary
		move.l	d0,a5
		tst.l	d0
		beq	ours
		exg	a5,a6
		lea	cdname(pc),a0
		bsr	sysseg
		bne	ours1			| CD is there: the ROM Shell has run; cshell
		lea	bootname(pc),a0
		bsr	sysseg
		beq	ours1
		move.l	d0,a0
		move.l	8(a0),d0		| seg_Seg
		beq	ours1
		lsl.l	#2,d0
		addq.l	#4,d0
		move.l	d0,a2			| the BootShell's code
		exg	a5,a6
		move.l	a5,a1
		jsr	-414(a6)		| CloseLibrary
		movem.l	(sp)+,d0/a0
		jmp	(a2)
ours1:		exg	a5,a6
		move.l	a5,a1
		jsr	-414(a6)		| CloseLibrary
ours:		movem.l	(sp)+,d0/a0
		movel	4:W,a6
		movel	a6@(ThisTask:W),a3

		subl	a2,a2			| no packet (csh: a command)
#ifndef CSH_CMDSTART
| the start-up packet: before anything else touches the port
		lea	a3@(pr_MsgPort:L),a0
		jsr	a6@(.LVOWaitPort:L)
		lea	a3@(pr_MsgPort:L),a0
		jsr	a6@(.LVOGetMsg:L)
		movel	d0,a2			| the message; its ln_Name is the packet
#endif

		btst.b	#1,1+Attn(a6)
		jne	cpuok
		move.l	#0x68020,d7		| alert: needs a 68020
		jsr	-0x6c(a6)
		moveq.l	#20,d7
		bra	nomem

cpuok:
		movql	#103,d7			| ERROR_NO_FREE_STORE
		movel	#___data_size,d2
		movel	d2,d0
		movql	#1,d1
		swap	d1			| MEMF_CLEAR
		jsr	a6@(.LVOAllocMem:L)
		movel	d0,a4
		movel	a4,d0
		beq	nomem
		lea	___a4_init-0x7ffe,a5
		movel	d2,d0
		movel	a5,a0
		movel	a4,a1
		jsr	a6@(.LVOCopyMemQuick:L)
		lea	___datadata_relocs,a0
		movel	a0@+,d0
		beq	no_reloc
		movel	a5,d1
		subl	a4,d1
do_reloc:	movel	a0@+,a1
		subl	d1,a4@(a1:l)
		subql	#1,d0
		bne	do_reloc
no_reloc:	lea	a4@(0x7ffe:L),a4
		movel	a4,tc_TrapData(a3)

		movel	sp,a4@(___SaveSP:L)
		movel	a6,a4@(_SysBase:L)
		movel	a2,d0
		beq	nopkt
		movel	a2@(ln_Name:W),a4@(__shellpkt:L)
nopkt:
		lea	emptyline,a0
		movel	a0,a4@(___commandline:L)
		movql	#1,d0
		movel	d0,a4@(___commandlen:L)
		movel	d2,a4@(___MemLen:L)

		lea	___INIT_LIST__,a2
		addqw	#4,a2
		moveql	#-1,d2
		jbsr	callfuncs
		movel	a4@(___env:L),sp@-
		movel	a4@(___argv:L),sp@-
		movel	a4@(___argc:L),sp@-
		jbsr	_main
		movel	d0,sp@(4:L)

_exit:
__exit:
		movel	(sp)+,a0
		movel	(sp)+,d0
		movel	a4@(___SaveSP:L),sp
		movel	d0,-(sp)
		movel	a0,-(sp)
	 	lea	___EXIT_LIST__,a2
		addqw	#4,a2
		moveql	#0,d2
		jbsr	callfuncs

__Exit: .globl	__Exit
____exit: .globl	____exit
		movel	sp@(4:L),d7
		movel	a4@(___SaveSP:L),sp
		movel	a4@(_SysBase:L),a6
		tstl	a4@(__keepdata:L)
		bne	todos
		movel	a4@(___MemLen:L),d0
		lea	a4@(-0x7ffe:L),a1
		jsr	a6@(.LVOFreeMem:L)
todos:
		movel	d7,d0
		rts

| no memory (or no 68020): the packet goes back as it came (the Guru
| Book's emergency exit), the caller is not left waiting
nomem:
		movel	a2,d0
		beq	nomem1			| a command: nothing to give back
		movel	a2,a1
		movel	a1@(ln_Name:W),a0
		movel	a0@(dp_Port:W),a0
		jsr	a6@(.LVOPutMsg:L)
nomem1:		movel	d7,d0
		rts

___restore_a4: .globl ___restore_a4
_geta4: 	.globl _geta4
		movel	4:W,a4
		movel	a4@(ThisTask:W),a4
		movel	tc_TrapData(a4),a4
		rts

| a pipe stage: a0 -> "xxxxxxxx\n", the struct stage's address in hex
_stage_stub:	.globl	_stage_stub
		moveq	#0,d1
		moveq	#7,d2
1:		moveq	#0,d0
		move.b	(a0)+,d0
		sub.b	#48,d0			| '0'
		cmp.b	#9,d0
		bls	2f
		sub.b	#39,d0			| 'a' - '0' - 10
2:		lsl.l	#4,d1
		or.l	d0,d1
		dbra	d2,1b
		move.l	d1,a0
		move.l	(a0),a4
		movel	4:W,a0
		movel	a0@(ThisTask:W),a0
		movel	a4,tc_TrapData(a0)
		jmp	_stage_entry

| LONG cliinit(struct DosPacket *dp, LONG newcli, struct DosLibrary *dos):
| CliInitNewcli() or CliInitRun(), every register but d0 kept as it was
| (they are private; trusting them with a4, csh's data, is not worth it)
_cliinit:	.globl	_cliinit
		movem.l	d1-d7/a0-a6,-(sp)
		move.l	4+56(sp),a0		| dp
		move.l	8+56(sp),d0		| newcli
		move.l	12+56(sp),a6		| DOSBase
		tst.l	d0
		beq	3f
		jsr	-930(a6)		| CliInitNewcli(a0)
		bra	4f
3:		jsr	-936(a6)		| CliInitRun(a0)
4:		movem.l	(sp)+,d1-d7/a0-a6
		rts

| the break relay: a4 in its tc_UserData
_relay_stub:	.globl	_relay_stub
		movel	4:W,a0
		movel	a0@(ThisTask:W),a0
		movel	a0@(tc_UserData:W),a4
		movel	a4,tc_TrapData(a0)
		jmp	_relay_entry

callfuncs:	lea	a4@(cleanupflag:L),a5
		movel	a2,a3
		moveql	#0,d3
		jra	oldpri
stabloop:	movel	a3@+,d4
		movel	a5@,d5
		cmpl	d4,d5
		jne	notnow
		movel	d0,a0
		jsr	a0@
notnow: 	eorl	d2,d4
		eorl	d2,d5
		cmpl	d5,d4
		jcc	oldpri
		cmpl	d3,d4
		jls	oldpri
		movel	d4,d3
oldpri: 	movel	a3@+,d0
		jne	stabloop
		eorl	d2,d3
		movel	d3,a5@
		cmpl	d2,d3
		jne	callfuncs
		rts

| FindSegment(a0, 0, TRUE) with a6 = DOSBase; Z clear when found
sysseg:		move.l	a0,d1
		moveq	#0,d2
		moveq	#1,d3
		jsr	-780(a6)
		tst.l	d0
		rts

emptyline:	.ascii	"\n\0"
dosname:	.asciz	"dos.library"
cdname:		.asciz	"CD"
bootname:	.asciz	"BootShell"
		.even

		.data

_SysBase:	.globl _SysBase
		.long ___nocommandline
___SaveSP:	.globl ___SaveSP
		.long ___cpucheck
		.long ___initlibraries
		.long ___initcpp

.comm		__WBenchMsg,4
.comm		___commandline,4
.comm		___commandlen,4
.comm		___MemLen,4
.comm		___argc,4
.comm		___argv,4
.comm		___env,4
.comm		__shellpkt,4
.comm		__keepdata,4
.lcomm		cleanupflag,4

	.globl	___LIB_LIST__
	.globl	___INIT_LIST__
	.globl	___EXIT_LIST__
	.globl	___CTOR_LIST__
	.globl	___DTOR_LIST__

.section	.dlist___LIB_LIST__,"aw"
___LIB_LIST__:
		.long 0

.section	.list___INIT_LIST__,"aw"
___INIT_LIST__:
		.long 0

.section	.list___EXIT_LIST__,"aw"
___EXIT_LIST__:
		.long 0

.section	.list___CTOR_LIST__,"aw"
___CTOR_LIST__:
		.long 0

.section	.list___DTOR_LIST__,"aw"
___DTOR_LIST__:
		.long 0

.section	.end_of_lists,"aw"
		.long 0
.section	.end_of_dlists,"aw"
		.long 0
