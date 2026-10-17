# CShell (csh)

A modern shell for the Amiga: the shell inside a CTerm tab. It speaks a bash-style
language (pipes, variables, loops, functions, `*` wildcards) **and** the Amiga's own
script language, so every existing script still runs in it. At the prompt it brings
what fish and zsh are loved for: suggestions from history, the line coloured as you
type, completion that knows commands, drawers, assigns and volumes. Every key can be
changed.

It is **every Shell after the boot**: Shell windows, the Shell icon, `Run`, CTerm's tabs,
and the commands other programs start (wasabi's `run` too). The ROM Shell still boots the
Amiga and keeps its built-in commands (`CD`, `Run`, `If`...), which csh uses.

## Install: one file, `L:csh`

`make` builds `csh` and `useshell`. On the Amiga:

    Copy csh L:csh
    Protect L:csh p ADD
    Protect L:csh e SUB
    Copy useshell C:

and at the top of `S:User-Startup`:

    ;BEGIN CShell
    If EXISTS L:csh
       useshell L:csh
    EndIf
    ;END CShell

After the next boot every new Shell is csh. **Why `L:` and not executable:** csh is the
system's Shell (as Workbench 1.3's was, `L:Shell-Seg`), not a command; started as a command
it would wait for ever, so nothing should find it: `L:` is not searched, and without the `e`
bit even `L:csh` typed says "file is not executable". **Updating** without a reboot: copy the
new `L:csh`, then `useshell L:csh`; Shells already open keep the old code (in memory until a
reboot). Not `Resident shell ... REPLACE`: run twice it frees the code the running Shells use.
**Back to the ROM Shell:** take the block out, reboot. CTerm needs no `shell=` line (its tabs
are csh by themselves). How it works: docs/design.md, "One csh".

**From ROM instead** (FS-UAE only): `make cshell-ext.rom`, then `tests/romuae.sh start`;
docs/design.md, "csh in a ROM".

**State: 0.1b11 (2026-10-15)** - runs commands and Amiga scripts exactly as the ROM Shell
does; its own prompt (colours as you type, suggestions from history, Ctrl+R, Tab, `!!`, every key
in `ENVARC:CShell/csh.keys`, `PROMPT`/`RPROMPT` with zsh's `%` escapes); the bash-style language
at the prompt and in `#!csh` scripts: arrays, `[[ ]]`, `(( ))`, `trap`, `getopts`, `select`,
subshells that keep their changes to themselves, `*` matching files. Pipes, `&` and jobs of its
own (csh's own commands too). `ls` is CUtils' (coloured by kind from `ENVARC:FileColours`, shared
with the terminal family). Faster than the ROM Shell in every measured case (tests/bench.sh):
it reads Amiga scripts itself (the ROM's `Execute` only for `.KEY` scripts and arguments).
Start with [docs/design.md](docs/design.md), "Start here".

    make            builds csh and useshell (bebbo's gcc in ~/opt/amiga)
    make test       the line rules and the language, checked on the PC
    tests/*.sh      on a test Amiga (W=...): each installs csh first (tests/install.sh);
                    pairs.sh and bench.sh compare with the ROM Shell, so a fresh machine

In csh: `csh FROM script [args]` runs a script in a subshell (`#!csh` on its first line: the
csh language, else an Amiga script), `csh COMMAND "text"` (or `-c`) runs csh text; neither
starts anything new. Your own aliases and settings go in `S:CShell-Startup` (csh's language),
run once at the first prompt.

## License

MIT, see [LICENSE](LICENSE).
