#!/usr/bin/env bash
cd "$(dirname "$0")"
for f in runint skip.txt if.txt ex.txt rest.txt; do ./w put $f RAM:$f >/dev/null; done
printf 'Execute RAM:ex.txt hi\nEcho rest1\n' > /tmp/exin.txt; ./w put /tmp/exin.txt RAM:exin.txt >/dev/null
for c in "runint -inter Execute RAM:ex.txt hi" "runint -in RAM:rest.txt Execute RAM:ex.txt hi"; do
  echo "### $c"; ./w run --max-time 10 "RAM:$c"; echo "(exit $?)"; ./w ping >/dev/null || { echo DEAD; exit 1; }; done
