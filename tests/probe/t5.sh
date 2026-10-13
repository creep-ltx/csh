#!/usr/bin/env bash
cd "$(dirname "$0")"; W=./w
for f in s/errs s/runt; do $W put $f RAM:$(basename $f) >/dev/null; done
r() { echo "### $1"; $W run --max-time 15 "$1"; echo "(exit $?)"; }
r "Execute RAM:errs"
r "Execute RAM:runt"
r "Status"
r "Run >NIL: RAM:rcx 0"
r "Execute RAM:misc"
r "Protect RAM:rexx +s"
r "RAM:rexx"
r "Copy C:Info RAM:pinfo"
r "Protect RAM:pinfo +ph"
r "List RAM:pinfo"
r "RAM:pinfo RAM:"
r "Resident"
