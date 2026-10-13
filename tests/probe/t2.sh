#!/usr/bin/env bash
cd "$(dirname "$0")"
./w put runint RAM:runint >/dev/null
for f in skip.txt if.txt ex.txt; do ./w put $f RAM:$f >/dev/null; done
for c in "runint Echo hello world" "runint Set FOO bar" "runint CD RAM:" "runint Failat 30" \
  "runint -in RAM:skip.txt Skip x" "runint -in RAM:if.txt If EXISTS RAM:nothere" \
  "runint -in RAM:if.txt If EXISTS RAM:" "runint Execute RAM:ex.txt hi" "runint Echo"; do
  echo "### $c"; ./w run --max-time 10 "RAM:$c"; echo "(exit $?)"; done
