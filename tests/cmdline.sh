#!/usr/bin/env bash
# cmdline.sh - _CMDLINE (the running program's line, for CTerm's saved
# layouts) is kept for lines typed at the prompt only, so this types them
# into a csh in a CON: window and reads what they wrote. W=... as the
# other tests (a test Amiga of your own: it opens a window there).
cd "$(dirname "$0")"; W=${W:-probe/w}
. ./install.sh || exit 2
printf '#!csh\nGet _CMDLINE >>RAM:clout\n' > /tmp/cmdline-cs.$$
printf 'Get _CMDLINE >>RAM:clout\n' > /tmp/cmdline-as.$$
$W put /tmp/cmdline-cs.$$ RAM:clcs >/dev/null; $W put /tmp/cmdline-as.$$ RAM:clas >/dev/null
rm -f /tmp/cmdline-*.$$
$W run 'Delete RAM:clout QUIET' >/dev/null 2>&1
$W run 'Protect RAM:clcs +s' >/dev/null; $W run 'Protect RAM:clas +s' >/dev/null
$W run --detach 'NewShell "CON:0/20/640/200/cmdline test"' >/dev/null
$W wait --window 'cmdline test' >/dev/null 2>&1 || sleep 3
sleep 1
for l in 'Get _CMDLINE >>RAM:clout' 'Get   _CMDLINE   >>RAM:clout' 'echo x | Get _CMDLINE >>RAM:clout' \
         'RAM:clcs a b' 'RAM:clas' 'Execute RAM:clas' 'echo "after [$_CMDLINE]" >>RAM:clout' 'exit'; do
  $W key type --enter "$l" >/dev/null; sleep 1
done
got=$($W run 'Type RAM:clout' 2>&1)
want='Get _CMDLINE
Get _CMDLINE
echo x | Get _CMDLINE
RAM:clcs a b
RAM:clas
Execute RAM:clas
after []'
$W run 'Delete RAM:clout RAM:clcs RAM:clas QUIET' >/dev/null 2>&1
if [ "$got" = "$want" ]; then echo "ok   cmdline"; else echo "DIFF cmdline"; diff <(echo "$want") <(echo "$got") | sed 's/^/    /'; exit 1; fi
