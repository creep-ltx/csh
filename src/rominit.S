| rominit.S - the init of cshell's ROM module (tools/mkextrom.py): exec runs
| it after dos.library is up (RTF_AFTERDOS), before the boot Shell starts
| the Startup-Sequence. It points dos's "shell" entry - the user shell of
| NewShell, Run and System() - at cshell, as "Resident shell C:cshell
| REPLACE" does (the Guru Book 14.5.4). "BootShell" stays the ROM's: it
| boots (the ROM Shell puts its internal commands - Run, CD, Echo... - into
| the resident list when it first runs; with cshell booting, the
| Startup-Sequence's first "Run" was an unknown command), and Execute()
| always uses it.
| Position-independent; mkextrom.py fills in ourseg (cshell's seglist, a BPTR).

seg_Seg		=	8
		.text
		.globl	_rominit
_rominit:
		movem.l	d2-d3/a2/a6,-(sp)
		move.l	4.w,a6
		lea	dosname(pc),a1
		moveq	#36,d0
		jsr	-552(a6)		| OpenLibrary
		tst.l	d0
		beq	9f
		move.l	d0,a2
		jsr	-132(a6)		| Forbid
		exg	a2,a6			| a6 dos, a2 exec
		lea	shellname(pc),a0
		bsr	repoint
		exg	a2,a6
		jsr	-138(a6)		| Permit
		move.l	a2,a1
		jsr	-414(a6)		| CloseLibrary
9:		movem.l	(sp)+,d2-d3/a2/a6
		moveq	#0,d0
		rts

| the system entry named a0, if there, to ourseg
repoint:	move.l	a0,d1
		moveq	#0,d2
		moveq	#1,d3			| system entries (seg_UC CMD_SYSTEM)
		jsr	-780(a6)		| FindSegment
		tst.l	d0
		beq	1f
		move.l	d0,a0
		move.l	ourseg(pc),seg_Seg(a0)
1:		rts

		.even
ourseg:		.long	0x43534853		| "CSHS": mkextrom.py puts the BPTR here
dosname:	.asciz	"dos.library"
shellname:	.asciz	"shell"
		.even
