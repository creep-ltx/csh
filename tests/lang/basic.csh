#!csh
# the csh language on the real ROM: Amiga commands with bash's words
echo "args: $# [$1] [$2]"
x=world
echo "hello $x" 'no $x'
CD RAM:
mkdir_test=RAM:cshdir
MakeDir $mkdir_test
[ -d $mkdir_test ] && echo "made it"
echo one > RAM:cshdir/f1.txt
echo two > RAM:cshdir/f2.txt
for f in RAM:cshdir/*.txt; do echo "file $f"; done
List RAM:cshdir/f1.t* QUICK NOHEAD
n=0
while [ $n -lt 3 ]; do n=$((n + 1)); done
echo "n=$n"
count() { echo "$# things"; }
count a b c
v=$(Type RAM:cshdir/f1.txt)
echo "read back: $v"
if Search RAM:cshdir/f2.txt two >NIL:; then echo "found two"; fi
Search RAM:cshdir/f2.txt nothing >NIL: || echo "rc after miss: $?"
case "$x" in w*) echo "starts with w";; *) echo no;; esac
printf "%s-%03d\n" pad 7
Set amigavar 42
Get amigavar
echo "amigavar is $amigavar"
Delete RAM:cshdir ALL QUIET
[ -e RAM:cshdir ] || echo "gone"
Type <<END
here $x
END
echo done
