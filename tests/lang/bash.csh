#!csh
# bash's parts added 2026-10-14, on the real ROM (RAM:phead N reads N lines)
MakeDir RAM:cb
CD RAM:cb
echo a > a.txt; echo b > b.txt; echo c > c.doc
echo *.txt "*.txt" *.none
ls -1 *.txt
Dir *.doc
List *.txt QUICK NOHEAD
a=(one two "three four")
echo ${#a[@]} ${a[2]} ${a[-1]}
for x in "${a[@]}"; do echo "[$x]"; done
declare -A m=([x]=1 [y]=2)
echo ${m[x]}${m[y]} ${#m[@]}
s="Hello Amiga"
echo ${s^^} ${s:6} ${s/Amiga/World}
[[ $s == Hello* ]] && echo match
[[ $s =~ ^H(el+)o ]] && echo ${BASH_REMATCH[1]}
(( n = 6 * 7 )); echo $n
for ((i = 0; i < 3; i++)); do printf '%d' $i; done; echo
x=1; (x=2; CD RAM:; echo "in $x"); echo "out $x"; pwd
y=$(cd RAM:; pwd); echo "$y then $(pwd)"
cd ..; pwd; cd -; cd RAM:cb/; cd ../cb; pwd
pushd RAM: >NIL:; pwd; popd >NIL:; pwd
f() { while true; do echo y; done; }
f | RAM:phead 3
v=42; g() { echo "from g v=$v $1"; }
set -- p1 p2
j=$({ g "$2"; } >RAM:cb/g.out &)
wait
Type RAM:cb/g.out
set -- -v -n 3 file
while getopts vn: o; do echo "$o${OPTARG:+=$OPTARG}"; done
shift $((OPTIND - 1)); echo "rest $1"
printf '%.3f %e %g\n' 3.14159 1234.5 0.5
echo $(( RANDOM < 32768 )) $(( SECONDS >= 0 ))
select c in red green; do echo "picked $c"; break; done <<<2
printf 'a\nb\n' | mapfile -t L; echo ${#L[@]} ${L[1]}
set -o pipefail; RAM:rcx 3 | RAM:pcat >NIL:; echo "$? ${PIPESTATUS[@]}"; set +o pipefail
readonly R=1
R=2
echo "R=$R"
trap 'echo trapped at the end' EXIT
CD RAM:
Delete RAM:cb ALL QUIET
echo done
