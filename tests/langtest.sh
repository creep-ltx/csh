#!/usr/bin/env bash
# langtest.sh [NAME...] - each tests/lang/NAME.csh run by csh in FS-UAE
# (csh FROM it, with two arguments) against NAME.want. W=probe/a: the A1200.
cd "$(dirname "$0")"; W=${W:-probe/w}
$W put ../csh RAM:csh >/dev/null || exit 2
$W put ../ls RAM:ls >/dev/null; $W put probe/pcat RAM:pcat >/dev/null; $W put probe/rcx RAM:rcx >/dev/null
names=("$@"); [ ${#names[@]} = 0 ] && names=($(cd lang && ls *.csh | sed 's/\.csh$//'))
bad=0
for n in "${names[@]}"; do
  $W put lang/$n.csh RAM:$n.csh >/dev/null
  got=$($W run --max-time 60 "RAM:csh FROM RAM:$n.csh first \"second word\"" 2>&1); rc=$?
  if [ "$got" == "$(cat lang/$n.want)" ]; then echo "ok   $n (rc $rc)"
  else echo "DIFF $n (rc $rc)"; diff <(cat lang/$n.want) <(echo "$got") | sed 's/^/    /'; bad=1; fi
done
exit $bad
