#!csh
# pipes and jobs on the real ROM (RAM:pcat copies its input, RAM:rcx N exits N)
echo "one two" | RAM:pcat
printf 'a\nb\nc\n' | RAM:pcat | RAM:pcat
RAM:rcx 5 | RAM:pcat >NIL:
echo "rc of pcat last: $?"
echo x | RAM:rcx 7
echo "rc of rcx last: $?"
n=0
printf 'l1\nl2\nl3\n' | RAM:pcat | while read line; do
  case "$line" in l*) n=$((n + 1));; esac
done
echo "lines read in csh: $n"
v=$(echo inner | RAM:pcat | RAM:pcat)
echo "captured: $v"
echo "to a file" | RAM:pcat >RAM:pipeout
Type RAM:pipeout
Delete RAM:pipeout QUIET
nosuchcmd | RAM:pcat
echo "after unknown: $?"
j=$(Wait 1 &)
case "$j" in "[1] "*) echo "job started";; *) echo "no job: $j";; esac
jobs
wait
echo "jobs done"
