#!/usr/bin/env bash
# romuae.sh start|stop [EXTROM] - the private FS-UAE of CTerm/tests/files/uae.sh
# (its drive copy /tmp/ctfiles, wasabid on port 1240) started with an
# extended ROM from $F00000 (default: cshell-ext.rom made by tools/mkextrom.py),
# the 3.2 Kickstart unchanged. That emulator (uae.sh) must not run at the
# same time: this stops it first.
D=$(cd "$(dirname "$0")/.." && pwd)
C=~/Projects/CTerm/tests/files/uae.sh
W=/tmp/ctfiles; U=cshrom-uae
EXT=${2:-$D/cshell-ext.rom}
export WASABI_HOST=127.0.0.1 WASABI_PORT=1240
case "$1" in
start)
  "$C" stop >/dev/null 2>&1
  systemctl --user stop $U.scope 2>/dev/null
  [ -d "$W/AmigaOS3.2" ] || { "$C" start && "$C" stop; } || exit 1
  [ -f "$EXT" ] || { echo "romuae.sh: no $EXT (python3 tools/mkextrom.py cshell $EXT)" >&2; exit 1; }
  grep -v '^kickstart_ext_file' "$W/ctfiles.fs-uae" > "$W/cshrom.fs-uae"
  # UAE's second extended ROM, at the address mkextrom.py built it for
  echo "uae_kickstart_ext_rom_file2 = $EXT" >> "$W/cshrom.fs-uae"     # from $F00000; its first 64 KB under UAE's boot ROM
  [ -n "$SERIAL" ] && echo "serial_port = $SERIAL" >> "$W/cshrom.fs-uae"   # KPrintF to a file
  (cd ~/FS-UAE && SDL_VIDEODRIVER=offscreen systemd-run --user --scope --unit=$U \
     -p MemoryMax=2G fs-uae "$W/cshrom.fs-uae" >"$W/cshrom.out" 2>&1 &)
  for i in $(seq 1 60); do wasabi ping >/dev/null 2>&1 && break; sleep 2; done
  wasabi ping >/dev/null 2>&1 || { echo "romuae.sh: no wasabi on 1240 (see $W/cshrom.out)" >&2; exit 1; }
  echo up
  ;;
stop)
  systemctl --user stop $U.scope 2>/dev/null
  while systemctl --user is-active --quiet $U.scope; do sleep 1; done
  ;;
esac
