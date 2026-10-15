#!/usr/bin/env bash
# jobtest.sh - $!, wait -n, wait %n, disown with real background jobs:
# csh FROM a script on a test Amiga (W=...). The CLI numbers a job prints
# when it starts ([1] 7) are taken out before comparing.
cd "$(dirname "$0")"; W=${W:-probe/w}
. ./install.sh || exit 2
cat > /tmp/jobtest.$$ <<'CSH'
#!csh
Wait 4 &
echo "a $!"
Wait 1 &
echo "b $!"
wait -n; echo "c $?"
jobs
wait $!; echo "d $?"
wait %9; echo "e $?"
Wait 1 &
disown
jobs; echo "f the newest disowned"
wait -n; echo "g $?"
Wait 2
echo "h end"
CSH
$W put /tmp/jobtest.$$ RAM:jobtest.csh >/dev/null; rm -f /tmp/jobtest.$$
got=$($W run 'csh FROM RAM:jobtest.csh' 2>&1 | sed -E 's/^\[([0-9]+)\]( [0-9]+)+$/[\1] N/')
want='[1] N
a %1
[2] N
b %2
c 0
[1] Running  Wait 4
d 0
csh: wait: no such job %9
e 127
[2] N
[1] Running  Wait 4
f the newest disowned
g 0
h end'
$W run 'Delete RAM:jobtest.csh QUIET' >/dev/null 2>&1
if [ "$got" = "$want" ]; then echo "ok   jobtest"; else echo "DIFF jobtest"; diff <(echo "$want") <(echo "$got") | sed 's/^/    /'; exit 1; fi
