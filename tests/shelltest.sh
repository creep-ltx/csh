#!/usr/bin/env bash
# shelltest.sh - cshell (csh as the system's user shell) in the private
# FS-UAE of CTerm/tests/files/uae.sh: Resident shell RAM:cshell REPLACE,
# then
#  - System() lines (wasabid's run uses the user shell): the Amiga script
#    rules, rc passed back;
#  - memory: 10 System() shells must leave nothing (Avail FLUSH);
#  - a NewShell window: the prompt, csh's language, a pipe, exit closes it;
#  - the BootShell put back at the end (tests/probe/bootshell).
# Never on the A1200: replacing its shell is the user's to decide.
cd "$(dirname "$0")"; W=${W:-probe/w}
[ "$W" = probe/a ] && { echo "shelltest.sh: not on the A1200" >&2; exit 2; }
make -s -C .. cshell >/dev/null || exit 2
[ -f probe/bootshell ] || ~/opt/amiga/bin/m68k-amigaos-gcc -Os -noixemul -s -o probe/bootshell probe/bootshell.c || exit 2
$W put ../cshell RAM:cshell >/dev/null || exit 2
$W put probe/bootshell RAM:bootshell >/dev/null
$W run --max-time 5 'Protect RAM:cshell +p' >/dev/null
$W run --max-time 5 'Resident shell RAM:cshell REPLACE' >/dev/null
bad=0
check() { if [ "$2" == "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     got:    [$2]"; echo "     wanted: [$3]"; bad=1; fi; }

got=$($W run --max-time 10 'Echo "system line" ; the ROM rules: a comment' 2>&1)
check "System() lines by the Amiga rules" "$got" 'system line'
$W run --max-time 10 'Fail' >/dev/null 2>&1; rc=$?
[ $rc = 0 ] && { echo "FAIL rc of a failing System() line is 0"; bad=1; } || echo "ok   System() rc passed back ($rc)"

avail() { $W run --max-time 20 'Avail FLUSH TOTAL' 2>&1 | tail -1; }
avail >/dev/null; a=$(avail)
for k in 1 2 3 4 5 6 7 8 9 10; do $W run --max-time 10 'Echo x' >/dev/null; done
b=$(avail)
check "10 System() shells leave no memory" "$((a - b))" "0"

$W run --max-time 5 'Run >NIL: NewShell "CON:0/20/640/220/shelltest/CLOSE"' >/dev/null
$W wait window shelltest --timeout 20 >/dev/null || { echo "FAIL no window"; bad=1; }
$W wait still --timeout 15 >/dev/null
$W key type 'echo "csh $((6*7))" > RAM:st.out; echo b a | Sort >> RAM:st.out' --enter >/dev/null
$W wait still --timeout 10 >/dev/null
check "csh's language in a NewShell" "$($W run --max-time 5 'Type RAM:st.out' 2>&1)" $'csh 42\nb a'
$W key type 'exit' --enter >/dev/null
$W wait window shelltest --gone --timeout 15 >/dev/null && echo "ok   exit closes the window" || { echo "FAIL exit left the window"; bad=1; }

$W run --max-time 5 'Delete RAM:st.out QUIET' >/dev/null
$W run --max-time 5 RAM:bootshell >/dev/null
exit $bad
