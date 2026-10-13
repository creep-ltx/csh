#!/usr/bin/env bash
# pairs.sh [NAME...] - each tests/pairs/NAME.txt run by the 3.2 ROM Shell
# (Execute) and by csh (csh FROM), outputs and return codes compared. Runs
# against the private FS-UAE of CTerm/tests/files/uae.sh (port 1240).
cd "$(dirname "$0")"; W=${W:-probe/w}; P=pairs
$W put ../csh RAM:csh >/dev/null || exit 2
for f in probe/rcx probe/pcat probe/perr $P/inner.key probe/s/sbit probe/s/ebit probe/s/rexx pairs/noe; do $W put $f RAM:$(basename $f) >/dev/null; done
$W run --max-time 5 "Protect RAM:sbit +s" >/dev/null; $W run --max-time 5 "Protect RAM:rexx +s" >/dev/null
names=("$@"); [ ${#names[@]} = 0 ] && names=($(cd $P && ls *.txt | sed 's/\.txt$//'))
bad=0
for n in "${names[@]}"; do
  $W put $P/$n.txt RAM:$n.txt >/dev/null
  rom=$($W run --max-time 30 "Execute RAM:$n.txt" 2>&1); rrc=$?
  $W run --max-time 5 "Delete RAM:x10 RAM:r1 RAM:r2 RAM:r3 RAM:r4 RAM:r5 RAM:e1 RAM:e2 RAM:e3o RAM:e3e RAM:p1 QUIET" >/dev/null 2>&1
  # the outer Shell reports csh's own failing rc; the ROM's Execute has no such line
  $W run --max-time 30 "RAM:csh FROM RAM:$n.txt" >/tmp/csh-pair.out 2>&1; crc=$?
  csh=$(grep -v '^RAM:csh failed returncode' /tmp/csh-pair.out)
  $W run --max-time 5 "Delete RAM:x10 RAM:r1 RAM:r2 RAM:r3 RAM:r4 RAM:r5 RAM:e1 RAM:e2 RAM:e3o RAM:e3e RAM:p1 QUIET" >/dev/null 2>&1
  if [ "$rom" == "$csh" ] && [ $rrc == $crc ]; then echo "same $n (rc $rrc)"
  else echo "DIFF $n (rom rc $rrc, csh rc $crc)"; diff <(echo "$rom") <(echo "$csh") | sed 's/^/    /'; bad=1; fi
done
exit $bad
