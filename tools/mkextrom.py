#!/usr/bin/env python3
"""mkextrom.py - cshell as a ROM module (docs/design.md "csh in a ROM").

    tools/mkextrom.py cshell rominit.bin cshell-ext.rom

Makes a 256 KB ROM file for $F00000, where exec looks for modules besides
the Kickstart: its ranges are $F80000-$1000000 and $F00000-$F80000 (read
from 3.2's exec). UAE's own boot ROM takes $F00000-$F0FFFF when it gives
a drawer as a hard drive, and UAE maps a second extended ROM
(kickstart_ext_rom_file2, tests/romuae.sh) from $F00000 with that first
64 KB hidden under its own: so the module starts 64 KB into the file, at
$F10100. The Kickstart itself is not changed.

The module is a RomTag "cshell" with RTF_AFTERDOS: exec runs its init
(src/rominit.S, assembled to rominit.bin) once dos.library is up, before
the boot Shell runs the Startup-Sequence; it points dos's "shell" entry
(NewShell, Run, System()) at cshell's seglist here. The ROM's own "shell"
module and "BootShell" are left alone: the ROM Shell puts its internal
commands (Run, CD, Echo...) into the resident list when it first runs, as
the boot Shell - a "shell" module of version 48, and cshell as BootShell,
both lost them and the boot stopped at its first line (tried 2026-10-14).

cshell is a pure (-resident32) program of two hunks: the code is placed
at a fixed address with its relocations done here, after a fake segment
head (size, next 0) as dos's seglists have; the data hunk (its first
values and the zeros of its bss) right after it, read-only - the start-up
copies it into RAM for each Shell.

No 0x1111 at its start: Kickstart would jump into an extended ROM at
$F00000 that starts with it (the CDTV's way in)."""
import struct, sys

SIZE = 256 * 1024
BASE = 0xF00000                  # the file's first byte
START = 0x10000                  # the module's place in it: past UAE's boot ROM
HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3F3, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_END, HUNK_SYMBOL, HUNK_RELOC32SHORT = 0x3EC, 0x3F2, 0x3F0, 0x3FC


def load_hunks(path):
    d = open(path, 'rb').read()
    p = 0

    def L():
        nonlocal p
        v = struct.unpack('>I', d[p:p + 4])[0]
        p += 4
        return v

    def W():
        nonlocal p
        v = struct.unpack('>H', d[p:p + 2])[0]
        p += 2
        return v

    if L() != HUNK_HEADER:
        sys.exit("not an Amiga program")
    while L():
        pass
    L()
    first, last = L(), L()
    sizes = [(L() & 0x3FFFFFFF) * 4 for _ in range(last - first + 1)]
    hunks = [{'size': s, 'data': b'', 'relocs': []} for s in sizes]
    h = -1
    while p < len(d):
        t = L() & 0x3FFFFFFF
        if t in (HUNK_CODE, HUNK_DATA):
            h += 1
            n = L() * 4
            hunks[h]['data'] = d[p:p + n]
            hunks[h]['kind'] = 'code' if t == HUNK_CODE else 'data'
            p += n
        elif t == HUNK_BSS:
            h += 1
            L()
            hunks[h]['kind'] = 'bss'
        elif t == HUNK_RELOC32:
            while True:
                c = L()
                if not c:
                    break
                to = L()
                hunks[h]['relocs'] += [(to, L()) for _ in range(c)]
        elif t == HUNK_RELOC32SHORT:
            while True:
                c = W()
                if not c:
                    break
                to = W()
                hunks[h]['relocs'] += [(to, W()) for _ in range(c)]
            if p % 4:
                p += 2
        elif t == HUNK_SYMBOL:
            while True:
                n = L()
                if not n:
                    break
                p += n * 4 + 4
        elif t == HUNK_END:
            pass
        else:
            sys.exit("unknown hunk type %x" % t)
    return hunks


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    hunks = load_hunks(sys.argv[1])
    init = open(sys.argv[2], 'rb').read()
    mark = init.find(b'CSHS')
    if mark < 0:
        sys.exit("rominit.bin: no CSHS place for the seglist")

    rom = bytearray(SIZE)
    tag = START + 0x100
    name = tag + 26
    ver = b'cshell 1.0 (14.10.2026) csh as the shell\r\n\0'
    idstr = name + 8
    initat = (idstr + len(ver) + 3) & ~3
    seghead = (initat + len(init) + 3) & ~3    # the fake seglist: size, next (0), then the code
    code = seghead + 8
    codesize = (hunks[0]['size'] + 3) & ~3
    data = (code + codesize + 15) & ~15
    end = data + hunks[1]['size']
    if end > SIZE:
        sys.exit("does not fit: %d bytes" % end)
    addr = [BASE + code, BASE + data]

    for h, at in zip(hunks, (code, data)):
        rom[at:at + len(h['data'])] = h['data']
    for h, at in zip(hunks, (code, data)):
        for to, off in h['relocs']:
            o = at + off
            v = struct.unpack('>I', rom[o:o + 4])[0]
            rom[o:o + 4] = struct.pack('>I', (v + addr[to]) & 0xFFFFFFFF)

    RTF_AFTERDOS = 0x04
    struct.pack_into('>HIIBBBb', rom, tag, 0x4AFC, BASE + tag, BASE + end, RTF_AFTERDOS, 1, 0, -101)
    struct.pack_into('>III', rom, tag + 14, BASE + name, BASE + idstr, BASE + initat)
    rom[name:name + 7] = b'cshell\0'
    rom[idstr:idstr + len(ver)] = ver
    rom[initat:initat + len(init)] = init
    struct.pack_into('>I', rom, initat + mark, (BASE + seghead + 4) >> 2)
    struct.pack_into('>II', rom, seghead, codesize + 8, 0)
    open(sys.argv[3], 'wb').write(rom)
    print("%s: cshell at $%06X (init $%06X), code $%06X (%d bytes), data $%06X (%d), %d of %d bytes used"
          % (sys.argv[3], BASE + tag, BASE + initat, BASE + code, codesize, BASE + data, hunks[1]['size'], end, SIZE))


main()
