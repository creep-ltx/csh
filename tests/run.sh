#!/usr/bin/env bash
# run.sh - the PC checks, under ASan: the ROM line rules (src/line.c) and
# the csh language (src/lang.c + builtin.c on tests/fakeos.c).
set -e
T=$(cd "$(dirname "$0")" && pwd)
F="-O1 -g -fsanitize=address,undefined -Wall -Wno-pointer-sign"
gcc $F -o /tmp/csh-line-test "$T/line_test.c" "$T/../src/line.c"
/tmp/csh-line-test
gcc $F -o /tmp/csh-lang-test "$T/lang_test.c" "$T/fakeos.c" "$T/../src/lang.c" "$T/../src/builtin.c" "$T/../src/regex.c" "$T/../src/bang.c"
/tmp/csh-lang-test
