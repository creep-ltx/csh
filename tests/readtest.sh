#!/usr/bin/env bash
# readtest.sh - read -n, -s, -t on a real console: typed into a csh in a
# CON: window (W=...: a test Amiga of your own; it opens a window there).
cd "$(dirname "$0")"; W=${W:-probe/w}
$W put ../csh RAM:csh >/dev/null || exit 2
printf 'RAM:csh\n' > /tmp/readtest-sc.$$; $W put /tmp/readtest-sc.$$ RAM:rtsc >/dev/null; rm -f /tmp/readtest-sc.$$
$W run 'Delete RAM:rt QUIET' >/dev/null 2>&1
$W run --detach 'NewShell "CON:0/20/640/200/read test" FROM RAM:rtsc' >/dev/null
sleep 3
t() { $W key type "$1" >/dev/null; sleep 0.4; }
k() { $W key press "$1" >/dev/null; sleep 0.6; }
t 'read -n1 k; echo "1 [$k]" >>RAM:rt'; k return; t 'z'
t 'read -n 3 k; echo "2 [$k]" >>RAM:rt'; k return; t 'abc'
t 'read -s pw; echo "3 [$pw]" >>RAM:rt'; k return; t 'hush'; k backspace; t 'h'; k return
t 'read -t 1 x; echo "4 $?" >>RAM:rt'; k return; sleep 2
t 'read -t 4 y; echo "5 $? [$y]" >>RAM:rt'; k return; t 'in time'; k return
t 'exit'; k return; t 'EndCLI'; k return
got=$($W run 'Type RAM:rt' 2>&1)
want='1 [z]
2 [abc]
3 [hush]
4 142
5 0 [in time]'
$W run 'Delete RAM:rt RAM:rtsc QUIET' >/dev/null 2>&1
if [ "$got" = "$want" ]; then echo "ok   readtest"; else echo "DIFF readtest"; diff <(echo "$want") <(echo "$got") | sed 's/^/    /'; exit 1; fi
