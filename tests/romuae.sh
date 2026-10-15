#!/usr/bin/env bash
# romuae.sh start|stop [EXTROM] - the private FS-UAE of CTerm/tests/files/uae.sh
# (rig "ctfiles" on the shared clean test Amigas, wasabid on port 1240)
# started with an extended ROM from $F00000 (default: cshell-ext.rom made by
# tools/mkextrom.py), the Kickstart unchanged. That emulator must not run at
# the same time without the ROM: this stops it first, and stop stops it.
# SERIAL=FILE: the emulated serial port to a file (KPrintF from the boot).
D=$(cd "$(dirname "$0")/.." && pwd)
C=~/Projects/CTerm/tests/files/uae.sh
EXT=${2:-$D/cshell-ext.rom}
export WASABI_HOST=127.0.0.1 WASABI_PORT=1240
case "$1" in
start)
  [ -f "$EXT" ] || { echo "romuae.sh: no $EXT (python3 tools/mkextrom.py cshell $EXT)" >&2; exit 1; }
  "$C" stop >/dev/null 2>&1
  # UAE's second extended ROM, at the address mkextrom.py built it for
  EXTRA="uae_kickstart_ext_rom_file2 = $EXT${SERIAL:+
serial_port = $SERIAL}" "$C" start >/dev/null || { echo "romuae.sh: no wasabi on 1240" >&2; exit 1; }
  echo up
  ;;
stop) "$C" stop ;;
*) echo "usage: romuae.sh start|stop [EXTROM]" >&2; exit 2 ;;
esac
