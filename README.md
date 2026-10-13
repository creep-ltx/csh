# CShell (csh)

A modern shell for the Amiga: the shell inside a CTerm tab. It speaks a bash-style
language (pipes, variables, loops, functions, `*` wildcards) **and** the Amiga's own
script language, so every existing script still runs in it. At the prompt it brings
what fish and zsh are loved for: suggestions from history, the line coloured as you
type, completion that knows commands, drawers, assigns and volumes. Every key can be
changed.

The ROM Shell stays where it is: it boots the Amiga and runs commands for other
programs.

**State: 0.1b1 (2026-10-13)** - csh runs commands and Amiga scripts exactly as the ROM
Shell does (compared line by line in FS-UAE); the comforts at the prompt and the bash-style
language come next. Start with [docs/design.md](docs/design.md), "Start here".

    make            builds csh (bebbo's gcc in ~/opt/amiga)
    make test       the line rules, checked on the PC
    tests/pairs.sh  ROM Shell and csh side by side in FS-UAE

On the Amiga: `csh` in a Shell or CTerm tab; `exit` goes back to the ROM Shell,
`csh FROM script` runs a script and ends.
