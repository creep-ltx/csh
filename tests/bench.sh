#!/usr/bin/env bash
# bench.sh [NAME...] - each tests/bench/b-NAME.txt run by the ROM Shell
# (Execute), then by csh as the Shell (Execute; tests/install.sh), timed on the PC around wasabi (the
# same overhead for both; the scripts are long so it hardly counts).
# Each is run 3 times, the best kept. cshlang: the same job as a #!csh
# script (b-NAME.csh) in the csh language. On a fresh test machine (W=...).
cd "$(dirname "$0")"; W=${W:-probe/w}
# the ROM Shell's half first: the machine must not have csh as its Shell yet
[ -f probe/whoshell ] || ~/opt/amiga/bin/m68k-amigaos-gcc -Os -noixemul -s -o probe/whoshell probe/whoshell.c || exit 2
$W put probe/whoshell RAM:whoshell >/dev/null
$W run --max-time 10 RAM:whoshell | grep -q 'seg4 ROM' || { echo "bench.sh: csh is already the Shell here - a fresh test machine (amiga-test ... --fresh)" >&2; exit 2; }
$W put probe/rcx RAM:rcx >/dev/null; $W put probe/pcat RAM:pcat >/dev/null
python3 -c "open('/tmp/bench-big','w').write(''.join('line %05d of the pipe test file, some text to make it longer\\n'%i for i in range(400)))"; $W put /tmp/bench-big RAM:bench-big >/dev/null
names=("$@"); [ ${#names[@]} = 0 ] && names=(internal loadseg cpath vars loop pipe)
best() { local b=999999 t s e; for i in 1 2 3; do s=$(date +%s%N); $W run --max-time 120 "$1" >/dev/null 2>&1; e=$(date +%s%N); t=$(( (e-s)/1000000 )); [ $t -lt $b ] && b=$t; done; echo $b; }
declare -A rom
for n in "${names[@]}"; do
  $W put bench/b-$n.txt RAM:b-$n.txt >/dev/null; $W put bench/b-$n.csh RAM:b-$n.csh >/dev/null
  rom[$n]=$(best "Execute RAM:b-$n.txt")
done
# then csh as the Shell: the Amiga script by csh (Execute), the same job in csh's language
. ./install.sh || exit 2
printf "%-10s %8s %8s %6s %9s %6s\n" script rom-ms csh-ms csh/rom cshlang lang/rom
for n in "${names[@]}"; do
  r=${rom[$n]}; c=$(best "Execute RAM:b-$n.txt"); l=$(best "csh FROM RAM:b-$n.csh")
  printf "%-10s %8d %8d %6s %9d %6s\n" $n $r $c $(python3 -c "print(f'{$c/$r:.2f}')") $l $(python3 -c "print(f'{$l/$r:.2f}')")
done
