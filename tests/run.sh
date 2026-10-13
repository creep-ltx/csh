#!/usr/bin/env bash
# run.sh - the PC checks: src/line.c with tests/line_test.c, under ASan.
set -e
T=$(cd "$(dirname "$0")" && pwd)
gcc -O1 -g -fsanitize=address,undefined -Wall -Wno-pointer-sign \
    -o /tmp/csh-line-test "$T/line_test.c" "$T/../src/line.c"
/tmp/csh-line-test
