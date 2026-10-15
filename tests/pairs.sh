#!/usr/bin/env bash
# pairs.sh [NAME...] - each tests/pairs/NAME.txt run by the 3.2 ROM Shell
# (Execute) and by csh (Execute once csh is the Shell: tests/install.sh),
# outputs and return codes compared. On a fresh test machine (W=...).
cd "$(dirname "$0")"; W=${W:-probe/w}; P=pairs
# the ROM Shell's half first: the machine must not have csh as its Shell yet
[ -f probe/whoshell ] || ~/opt/amiga/bin/m68k-amigaos-gcc -Os -noixemul -s -o probe/whoshell probe/whoshell.c || exit 2
$W put probe/whoshell RAM:whoshell >/dev/null
$W run --max-time 10 RAM:whoshell | grep -q 'seg4 ROM' || { echo "pairs.sh: csh is already the Shell here - a fresh test machine (amiga-test ... --fresh)" >&2; exit 2; }
for f in probe/rcx probe/pcat probe/perr $P/inner.key probe/s/sbit probe/s/ebit probe/s/rexx pairs/noe; do $W put $f RAM:$(basename $f) >/dev/null; done
$W run --max-time 5 "Protect RAM:sbit +s" >/dev/null; $W run --max-time 5 "Protect RAM:rexx +s" >/dev/null
names=("$@"); [ ${#names[@]} = 0 ] && names=($(cd $P && ls *.txt | sed 's/\.txt$//'))
bad=0
clean() { $W run --max-time 5 "Delete RAM:x10 RAM:r1 RAM:r2 RAM:r3 RAM:r4 RAM:r5 RAM:e1 RAM:e2 RAM:e3o RAM:e3e RAM:p1 QUIET" >/dev/null 2>&1; }
mkdir -p /tmp/csh-pairs
for n in "${names[@]}"; do
  $W put $P/$n.txt RAM:$n.txt >/dev/null
  $W run --max-time 30 "Execute RAM:$n.txt" >/tmp/csh-pairs/$n.rom 2>&1; echo $? >/tmp/csh-pairs/$n.romrc
  clean
done
# then csh as the Shell: the same Execute, now run by csh
. ./install.sh || exit 2
for n in "${names[@]}"; do
  csh=$($W run --max-time 30 "Execute RAM:$n.txt" 2>&1); crc=$?
  clean
  rom=$(cat /tmp/csh-pairs/$n.rom); rrc=$(cat /tmp/csh-pairs/$n.romrc)
  if [ "$rom" == "$csh" ] && [ $rrc == $crc ]; then echo "same $n (rc $rrc)"
  else echo "DIFF $n (rom rc $rrc, csh rc $crc)"; diff <(echo "$rom") <(echo "$csh") | sed 's/^/    /'; bad=1; fi
done
exit $bad
