#!csh
# fixes.csh - the 2026-10-13 audit's fixes that only the Amiga can show
echo "Echo in-script" > RAM:ascr
Protect RAM:ascr +s >NIL:
RAM:ascr && echo after
x=$(RAM:ascr); echo "[$x]"
for i in 1 2; do RAM:ascr; echo loop $i; done
export CSHT=1
CSHT=2
GetEnv CSHT
unset CSHT
GetEnv CSHT >NIL: || echo gone from ENV:
[ -d "" ] && echo empty-is-a-dir || echo empty-is-nothing
x=$(printf %d abc); echo "[$x]"
RAM:sigs
for i in {1..25}; do RAM:csh COMMAND 'RAM:rcx 0 | RAM:pcat' >NIL:; done
RAM:sigs
RAM:rcx 0 | RAM:pcat && echo pipes still work
d=RAM:deep
MakeDir $d >NIL:
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do d=$d/d$i; MakeDir $d >NIL:; done
Stack 4096
RAM:ls -R RAM:deep >RAM:lsout; echo ls rc $?
Stack 16000
Search RAM:lsout d12 >NIL: && echo deepest listed
RAM:ls RAM:nosuch RAM:ascr; echo ls rc $?
printf '#!csh\nwhile read l; do echo got $l; done\n' > RAM:cs
Protect RAM:cs +s >NIL:
echo hi | RAM:cs
Delete RAM:deep ALL QUIET >NIL:
Delete RAM:ascr RAM:cs RAM:lsout QUIET >NIL:
