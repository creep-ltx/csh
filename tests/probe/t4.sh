#!/usr/bin/env bash
# t4 - the ROM Shell script batch (step 0); each run is a fresh CLI
cd "$(dirname "$0")"; W=./w
for f in s/*; do $W put $f RAM:$(basename $f) >/dev/null; done
$W run --max-time 5 "Protect RAM:sbit +s" ; $W run --max-time 5 "Protect RAM:ebit +e"; $W run --max-time 5 "Protect RAM:rexx +s"
r() { echo "### $1"; $W run --max-time 15 "$1"; echo "(exit $?)"; }
r "Execute RAM:k1 bob"
r "Execute RAM:k1 bob flag 12 x y z"
r "Execute RAM:k1 bob FLAG NUM=7"
r "Execute RAM:k1 bob num=abc"
r "Execute RAM:k2 one two three four"
r "Execute RAM:k3 opt=val"
r "Execute RAM:k3 OPT val2 thing"
r "Execute RAM:k3"
r "Execute RAM:k4"
r "Execute RAM:k5"
r "Execute RAM:redir"
r "Execute RAM:pipe"
r "Execute RAM:expand"
r "Type RAM:x10"
r "Execute RAM:msgs >RAM:msgout"
r "Type RAM:msgout"
r "Execute RAM:fail"
r "Execute RAM:misc"
r "RAM:execlf"
