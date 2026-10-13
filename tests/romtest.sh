#!/usr/bin/env bash
# romtest.sh - cshell from a ROM (tools/mkextrom.py, src/rominit.S) in the
# private FS-UAE: boots with cshell-ext.rom (tests/romuae.sh), then checks
#  - exec has the module, dos's "shell" points into it, BootShell is the ROM's;
#  - System() lines through it, their rc; 10 of them leave no memory;
#  - a NewShell window: csh's language, a pipe, exit closes it.
# Stops the emulator at the end (KEEP=1: leaves it running). Never the A1200.
cd "$(dirname "$0")"; W=probe/w
make -s -C .. cshell-ext.rom >/dev/null || exit 2
for p in findres segs; do [ -f probe/$p ] || ~/opt/amiga/bin/m68k-amigaos-gcc -Os -noixemul -s -o probe/$p probe/$p.c || exit 2; done
./romuae.sh start >/dev/null || exit 2
bad=0
check() { if [ "$2" == "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     got:    [$2]"; echo "     wanted: [$3]"; bad=1; fi; }
$W put probe/findres RAM:findres >/dev/null; $W put probe/segs RAM:segs >/dev/null
check "exec has the cshell module" "$($W run --max-time 10 'RAM:findres cshell' | cut -c1-21)" "cshell at 00F10100: v"
segs=$($W run --max-time 10 RAM:segs)
echo "$segs" | grep -q '^shell .*= 00F1' && echo "ok   dos's shell is in the ROM module" || { echo "FAIL shell entry: $segs"; bad=1; }
echo "$segs" | grep -q '^BootShell .*= 00FCE1A4' && echo "ok   BootShell is the ROM's" || { echo "FAIL BootShell entry: $segs"; bad=1; }
check "System() lines by the Amiga rules" "$($W run --max-time 10 'Echo "via cshell" ; a comment' 2>&1)" 'via cshell'
$W run --max-time 10 'Fail' >/dev/null 2>&1; check "System() rc" "$?" "10"
avail() { $W run --max-time 20 'Avail FLUSH TOTAL' 2>&1 | tail -1; }
avail >/dev/null; a=$(avail)
for k in 1 2 3 4 5 6 7 8 9 10; do $W run --max-time 10 'Echo x' >/dev/null; done
check "10 System() shells leave no memory" "$((a - $(avail)))" "0"
$W run --max-time 5 'Run >NIL: NewShell "CON:0/20/640/220/romtest/CLOSE"' >/dev/null
$W wait window romtest --timeout 20 >/dev/null; $W wait still --timeout 15 >/dev/null
$W key type 'echo "csh $((6*7))" > RAM:rt.out; echo b a | Sort >> RAM:rt.out' --enter >/dev/null
$W wait still --timeout 10 >/dev/null
check "csh's language in a NewShell" "$($W run --max-time 5 'Type RAM:rt.out' 2>&1)" $'csh 42\nb a'
$W key type 'exit' --enter >/dev/null
$W wait window romtest --gone --timeout 15 >/dev/null && echo "ok   exit closes the window" || { echo "FAIL exit left the window"; bad=1; }
[ -n "$KEEP" ] || ./romuae.sh stop
exit $bad
