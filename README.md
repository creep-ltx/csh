# CShell (csh)

A modern shell for the Amiga: the shell inside a CTerm tab. It speaks a bash-style
language (pipes, variables, loops, functions, `*` wildcards) **and** the Amiga's own
script language, so every existing script still runs in it. At the prompt it brings
what fish and zsh are loved for: suggestions from history, the line coloured as you
type, completion that knows commands, drawers, assigns and volumes. Every key can be
changed.

The ROM Shell stays where it is: it boots the Amiga and runs commands for other
programs.

## Make csh the Shell of every window

`make cshell` builds `cshell`: csh as the system's own Shell. Copy it to the Amiga, then
in a Shell:

    Protect RAM:cshell +p
    Resident shell RAM:cshell REPLACE

From then on every new Shell is csh: NewShell, the Shell icon, `Run`, and the commands
other programs start (wasabi's `run` too). The boot is not touched. Until the next reboot.

**Back to the ROM Shell:** reboot, or run `bootshell` (`tests/probe/bootshell.c`).

Tried in FS-UAE and on the A1200 (`tests/shelltest.sh`, `A1200=yes` for the A1200). Install
`C:csh` with it, of the same version: `&` with csh's own commands runs a second csh from there.

**From ROM instead** (FS-UAE): `make cshell-ext.rom`, then `tests/romuae.sh start` boots the
emulator with csh as the Shell from a second ROM; the Kickstart file is not changed
(`tests/romtest.sh` checks it). How it works: docs/design.md, "csh in a ROM".

**State: 0.1b6 (2026-10-14)** - runs commands and Amiga scripts exactly as the ROM Shell
does; its own prompt (colours as you type, suggestions from history, Ctrl+R, Tab, `!!`, every key
in `ENVARC:CShell/csh.keys`, `PROMPT`/`RPROMPT` with zsh's `%` escapes); the bash-style language
at the prompt and in `#!csh` scripts: arrays, `[[ ]]`, `(( ))`, `trap`, `getopts`, `select`,
subshells that keep their changes to themselves, `*` matching files. Pipes, `&` and jobs of its
own (csh's own commands too); `ls` coloured by kind (`ENVARC:FileColours`, shared with the
terminal family). Faster than the ROM Shell in every measured case.
Start with [docs/design.md](docs/design.md), "Start here".

    make            builds csh and ls (bebbo's gcc in ~/opt/amiga)
    make test       the line rules and the language, checked on the PC
    tests/pairs.sh  ROM Shell and csh side by side in FS-UAE (W=probe/a: the A1200)
    tests/langtest.sh, tests/bench.sh   the language on the real ROM; the speed

On the Amiga: `csh` in a Shell or CTerm tab; `exit` goes back to the ROM Shell. CTerm
0.3b32 can start csh in every new tab (Settings, Files: "Then runs", or `CTerm SHELL csh`).
`csh FROM script [args]` runs a script (`#!csh` on its first line: the csh language),
`csh COMMAND "text"` runs csh text and ends. Your own aliases and settings go in
`S:CShell-Startup` (csh's language), run once at the first prompt.
