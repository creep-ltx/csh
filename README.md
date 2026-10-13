# CShell (csh)

A modern shell for the Amiga: the shell inside a CTerm tab. It speaks a bash-style
language (pipes, variables, loops, functions, `*` wildcards) **and** the Amiga's own
script language, so every existing script still runs in it. At the prompt it brings
what fish and zsh are loved for: suggestions from history, the line coloured as you
type, completion that knows commands, drawers, assigns and volumes. Every key can be
changed.

The ROM Shell stays where it is: it boots the Amiga and runs commands for other
programs.

**State: 0.1b1 (2026-10-13)** - runs commands and Amiga scripts exactly as the ROM Shell
does; its own prompt (colours as you type, suggestions from history, Ctrl+R, Tab, every key in
`ENVARC:CShell/csh.keys`); the bash-style language at the prompt and in `#!csh` scripts. Pipes, `&` and
jobs of its own; `ls` coloured by kind (`ENVARC:FileColours`, shared with the terminal family).
Faster than the ROM Shell in every measured case.
Start with [docs/design.md](docs/design.md), "Start here".

    make            builds csh and ls (bebbo's gcc in ~/opt/amiga)
    make test       the line rules and the language, checked on the PC
    tests/pairs.sh  ROM Shell and csh side by side in FS-UAE (W=probe/a: the A1200)
    tests/langtest.sh, tests/bench.sh   the language on the real ROM; the speed

On the Amiga: `csh` in a Shell or CTerm tab; `exit` goes back to the ROM Shell.
`csh FROM script [args]` runs a script (`#!csh` on its first line: the csh language),
`csh COMMAND "text"` runs csh text and ends.
