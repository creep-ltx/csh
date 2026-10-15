# install.sh - sourced by the tests (W set): csh as the system Shell on the
# test machine, the way S:User-Startup does it on a real one (docs/design.md
# "One csh"): L:csh, pure, not executable; Resident shell L:csh REPLACE;
# useshell (the system "shell" and Execute()'s BootShell entries). Run again
# it updates (the old code stays in memory until a reboot).
make -s -C .. csh useshell >/dev/null || return 2
$W put ../csh RAM:csh.new >/dev/null || return 2
$W put ../useshell RAM:useshell >/dev/null || return 2
$W run --max-time 10 'Copy RAM:csh.new L:csh' >/dev/null || return 2
$W run --max-time 5 'Delete RAM:csh.new QUIET' >/dev/null
$W run --max-time 5 'Protect L:csh p ADD' >/dev/null
$W run --max-time 5 'Protect L:csh e SUB' >/dev/null
$W run --max-time 10 'Resident shell L:csh REPLACE' >/dev/null || return 2
$W run --max-time 5 'RAM:useshell' >/dev/null || return 2
