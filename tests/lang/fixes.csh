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
RAM:rcx 0 | RAM:pcat >NIL:     # the first pipe takes csh's two job signals (kept till csh ends)
RAM:sigs
for i in {1..25}; do csh COMMAND 'RAM:rcx 0 | RAM:pcat' >NIL:; done
RAM:sigs
RAM:rcx 0 | RAM:pcat && echo pipes still work
printf '#!csh\nwhile read l; do echo got $l; done\n' > RAM:cs
Protect RAM:cs +s >NIL:
echo hi | RAM:cs
Delete RAM:ascr RAM:cs QUIET >NIL:
