#!csh
# more than 127 names: an Amiga command gets the pattern instead (ReadArgs takes 127)
makedir RAM:mn
cd RAM:mn
for i in {1..200}; do echo x > f$i; done
List * LFORMAT %n >RAM:mn.out
echo list rc=$?
n=0
while read l; do n=$((n+1)); done < RAM:mn.out
echo $n names listed
Delete * QUIET
echo delete rc=$?
cd RAM:
Delete RAM:mn RAM:mn.out QUIET
echo few: $(echo a > RAM:fa1; echo b > RAM:fa2; List RAM:fa* LFORMAT %n)
Delete RAM:fa1 RAM:fa2 QUIET
