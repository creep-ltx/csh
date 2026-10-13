#!/usr/bin/env bash
# bench.sh [NAME...] - each tests/bench/b-NAME.txt run by the ROM Shell
# (Execute) and by csh (csh FROM), timed on the PC around wasabi (the
# same overhead for both; the scripts are long so it hardly counts).
# Each is run 3 times, the best kept. FS-UAE of CTerm/tests/files/uae.sh.
cd "$(dirname "$0")"; W=probe/w
$W put ../csh RAM:csh >/dev/null; $W put probe/rcx RAM:rcx >/dev/null
names=("$@"); [ ${#names[@]} = 0 ] && names=(internal loadseg cpath vars loop)
best() { local b=999999 t s e; for i in 1 2 3; do s=$(date +%s%N); $W run --max-time 120 "$1" >/dev/null 2>&1; e=$(date +%s%N); t=$(( (e-s)/1000000 )); [ $t -lt $b ] && b=$t; done; echo $b; }
printf "%-10s %8s %8s %6s\n" script rom-ms csh-ms csh/rom
for n in "${names[@]}"; do
  $W put bench/b-$n.txt RAM:b-$n.txt >/dev/null
  r=$(best "Execute RAM:b-$n.txt"); c=$(best "RAM:csh FROM RAM:b-$n.txt")
  printf "%-10s %8d %8d %6s\n" $n $r $c $(python3 -c "print(f'{$c/$r:.2f}')")
done
