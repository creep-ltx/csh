#!/usr/bin/env bash
# comptest.sh - Tab completion typed into a csh in a CON: window: the menu
# (Tab, Tab, Shift+Tab after a list), a keyword's value (Copy x TO Al<Tab>
# offers drawers, not the ALL switch), KEY=value (TO=Al<Tab>). W=... as the
# other tests (a test Amiga of your own: it opens a window there).
cd "$(dirname "$0")"; W=${W:-probe/w}
. ./install.sh || exit 2
$W run 'Delete RAM:ct ALL QUIET' >/dev/null 2>&1
$W run 'MakeDir RAM:ct RAM:ct/Alldir' >/dev/null
for f in abc1 abc2 abc3; do $W run "Echo >RAM:ct/$f $f" >/dev/null; done
$W run --detach 'NewShell "CON:0/20/640/200/comp test"' >/dev/null
sleep 3
t() { $W key type "$1" >/dev/null; sleep 0.5; }
k() { $W key press "$1" >/dev/null; sleep 0.7; }
t 'cd RAM:ct'; k return
t 'echo ab'; k tab; k tab; k tab; k tab; k shift+tab; t '>RAM:ct/out1'; k return
t 'Copy abc1 TO Al'; k tab; k return
t 'Copy abc2 TO=Al'; k tab; k return
t 'exit'; k return
got="$($W run 'Type RAM:ct/out1' 2>&1)|$($W run 'List RAM:ct/Alldir LFORMAT %N' 2>&1 | sort | tr '\n' ' ')"
want='abc1|abc1 abc2 '
$W run 'Delete RAM:ct ALL QUIET' >/dev/null 2>&1
if [ "$got" = "$want" ]; then echo "ok   comptest"; else echo "DIFF comptest: want [$want] got [$got]"; exit 1; fi
