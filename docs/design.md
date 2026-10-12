# CShell (csh): a modern shell for the Amiga

*Design written 2026-10-12 with the user; nothing built yet. Everything here is proposed unless it
says decided. The facts behind it are in `~/Projects/Knowledge/amiga/writing-a-shell.md` (from the
AmigaDOS manual, the Amiga Guru Book's shell chapter and the verified notes); a line there marked
read or memory is not yet proven on the 3.2 ROM.*

## Start here (a later session)

1. Read this file, then `Knowledge/amiga/writing-a-shell.md`.
2. The next step is **step 0** in the plan below: answer the open questions on the real 3.2 ROM
   Shell in FS-UAE (`CTerm/tests/files/uae.sh`, its own wasabi port), and write what was seen into
   the knowledge note.
3. Build like Sumi and Maki: bebbo's gcc in `~/opt/amiga`, `-fbbb=-`, a PC test harness with fake
   Amiga headers (`Maki/tests/fake`).

## The idea

Two layers, as on Linux: **CTerm** is the terminal, **csh** is the shell. csh is a native Amiga
shell that speaks a bash-style language **and** the Amiga's own script language, so it can be the
only shell used day to day, and every existing script still runs in it.

The ROM Shell is never removed: it is in Kickstart (3.2: `shell 47.47`), it keeps the boot, and
it keeps running commands for other programs.

## Decided with the user (2026-10-12)

- **Name** CShell, command **`csh`** ("this is Amiga, not Unix"). Code in `~/Projects/CShell`.
- **`#!csh`** on a script's first line marks a csh script; without it a script is an Amiga script.
- **bash's language, the common part of it**: variables, quoting, `$(...)`, pipes and redirection,
  `if` / `for` / `while` / `case`, functions, `&&` / `||`, `$((...))`, wildcards, aliases, jobs,
  here-documents. Arrays, traps, process substitution and the rare corners only when a real
  script needs them. Not fish's language (nothing written for bash runs in it), not zsh's extras.
- **fish's and zsh's comforts at the prompt**: a grey suggestion from history while typing, the
  line coloured as typed, completion that knows commands, paths, assigns and volumes.
- **Amiga scripts run in csh itself** (not handed to the ROM Shell), the same as the ROM Shell
  runs them.
- **The ROM Shell keeps the boot** (Startup-Sequence, User-Startup) and other programs'
  Execute()/SystemTags(). Making csh the shell of new Shell windows comes later, if ever.
- **Keys are split as on Linux**: CTerm keeps its own (Right Amiga = menus, Left Amiga = panes);
  every other key goes to csh, which decides what it means. All of csh's keys are in a file.
- **Colours: the program that writes the text picks them**, CTerm's theme decides the shades.

## The layers (why the Amiga confuses)

| Layer | Job | In a CTerm tab |
|---|---|---|
| Window | what you see | CTerm |
| console.device | draws letters, keys to characters | not used: CTerm draws itself |
| Console handler (CON:) | collects and edits the line | CTerm is its own (`CTerm/src/console.c`): it answers DISK_INFO with 'CTRM' |
| Shell | reads the line, runs it | the ROM Shell today, **csh** |

## How csh runs

### A program inside a CLI (decided for the start)

csh starts as an ordinary command: typed in a Shell, or as the command a CTerm tab starts. It
then owns the CLI process it runs in: `Cli()` is valid, the Path, current directory, stack and
local variables are that CLI's. It reads lines and runs each command **on its own process** with
`RunCommand()`, exactly as the ROM Shell does. Commands see a correct CLI.

Why this way: the Guru Book's route of a real *user shell* (installed with `Resident shell ...
REPLACE`, started by the startup packet) needs **pure, re-entrant code** (one copy serves every
Shell) and private DOS calls, and an old CTerm build crashed at boot when started through
`SYS_UserShell`. The command route has none of that and is what wasabid already does. A user
shell can come later, from the same code, if it is ever wanted.

`EndCLI` / `EndShell` in a csh tab end csh; the ROM Shell under it then ends too when the tab
closes. Quitting csh with `exit` gives the ROM Shell back (useful while csh is young).

### Running a command, as the ROM Shell does

The search order and the steps around each command are copied from the Guru Book's UserShell.c
and the notes (details in `writing-a-shell.md`):

1. Aliases, then csh's built-ins, then the **resident list** (walked by hand under Forbid: its
   header is at `DosInfo+$10`, not where the NDK says; `FindSegment` cannot list), the current
   directory, the Path (`cli_CommandDir`, requesters off), then `C:` as `"C:" + name` (so
   multi-assigns work). A name with `:` or `/` is not searched. A drawer typed alone is a `CD`.
2. A file with the **s** bit is a script: Amiga or csh by its first line (`#!csh`); `/*` first
   is ARexx, handed to `RX`.
3. Around the call: stack = `cli_DefaultStack * 4`; open redirections, `SelectInput` /
   `SelectOutput`; `SetProgramDir`, `SetProgramName`; `cli_Module = seglist`; clear Ctrl-C/E/F;
   `SetIoErr(0)`; `RunCommand(seg, stack, args + "\n", len)`. After: if the command zeroed
   `cli_Module` it stays loaded (wasabid's detach rule), else `UnLoadSeg`; `cli_Module = 0`;
   restore the streams; store `cli_ReturnCode`, `cli_Result2`, `$RC`, `$Result2`.
4. The console stays **cooked** while a command runs (Ed and More switch to raw themselves), and
   Ctrl-C reaches the command because it runs on csh's own process.

### The console

csh edits the prompt line itself: while it waits for a line it takes the console **raw**
(`SetMode 1`, as Sumi and Maki do), draws the line with escape sequences, and goes back to cooked
before it runs anything. So it behaves the same in CTerm, CCON or a plain CON: window, and never
fights the console's own editor. The toolkit is Sumi's `tui.c` input side (both key conventions,
the mouse, the window size and resizes). In a console that is not interactive (a script, NIL:),
csh reads lines plainly.

## Two languages, one shell

### Which language a line is in

- **A script file**: `#!csh` on the first line means csh; anything else is an Amiga script, run
  the Amiga way from top to bottom (`.KEY`, `If`, `Skip`, `Lab`...).
- **At the prompt**: the csh language. Amiga commands still work there, because they are
  commands: `CD`, `Path`, `Set`, `Alias`, `Resident`, `Stack`, `Echo` are csh built-ins that also
  take their Amiga form. Only the Amiga *flow* words (`If` / `Else` / `EndIf`, `Skip` / `Lab`,
  `.KEY`) belong to Amiga scripts alone; at the prompt `if` is the csh `if`.

### Where they clash, and what csh does

| | Amiga Shell | csh language | csh's rule |
|---|---|---|---|
| `;` | starts a comment | separates commands | csh: separator; Amiga scripts: comment |
| `#` | a pattern character (`#?`) | starts a comment (at a word's start) | csh: comment only at a word's start, so `#?.txt` stays a pattern |
| `*` | escape in quotes (`*N`, `*"`), alone = the console | wildcard | csh: wildcard in a word with other characters, `*` alone is still the console; `\` escapes |
| wildcards | the program matches `#?` itself | the shell expands `*` | csh turns `*` into `#?` and lets the program match (Amiga programs take patterns; many take only one argument); csh's own `for f in *.txt` expands it itself |
| `Set` | `Set name value` | `name=value`, `set -e` | both: `Set name value` and `set name value` set; `name=value` too; `set -x`-style flags are csh options |
| `Alias` | `Alias name cmd []` | `alias name='cmd'` | both forms (an `=` means bash's) |
| `Echo` | `NOLINE`, `FIRST`, `LEN` | `-n`, `-e` | both |
| unknown `$x` | left as written (`$x`) | empty | csh: empty; Amiga scripts: left as written (the `If "$x" EQ "*$x"` idiom relies on it) |
| `` `cmd` `` | the output, lines joined by spaces | the output | the same; `$(cmd)` too |
| redirection | `>` `<` `>>` `<>` | also `2>`, `&>`, `|` | Amiga forms everywhere; `2>` goes to the error stream where 3.2 has one (open question) |
| return codes | 0 / 5 / 10 / 20, `FailAt` | 0 = true, else false | `&&`, `||`, `if` take 0 as true and 5 and above as false (`Search` not finding = false, like grep); `set -e` = stop on failure; Amiga scripts keep `FailAt` |
| case | commands any case | commands exact case | any case for commands and Amiga words; csh keywords (`if`, `for`) lower case |

### Variables

csh's variables **are** AmigaDOS local variables (`SetVar` with `LV_VAR`), so `Get`, `$name` in
an Amiga script and every program that calls `GetVar` see the same ones. `export name` makes it
global (ENV:), which is the Amiga's environment. `$RC`, `$Result2` and `$process` are kept up to
date as the ROM Shell keeps them.

## Amiga scripts: csh behaves as the ROM Shell

csh's Amiga mode is a faithful copy of the ROM Shell's main loop: it reads the script through
`cli_CurrentInput` line by line, so `Execute`, `If`, `Skip` and `Lab` (which move
`cli_CurrentInput` along) behave exactly as in the ROM Shell.

- **Proposed: use the ROM's own internal commands for them** (the Guru Book says third-party
  shells may run the 2.0 Shell's internal commands, because the CLI structure is standard): then
  `If`, `Skip`, `Lab`, `Execute` (with `.KEY` and its T: copy), `FailAt`, `Quit`, `Why`, `Set`,
  `Get` are the ROM's, and match it by definition. Step 0 tests whether `RunCommand` on an
  internal command's segment works from csh. If it does not, csh implements them itself, from
  the manual, and step 2's tests compare every one against the ROM Shell.
- `FailAt`, the "failed returncode" message, `Quit` ending every level, Ctrl-D ending the script
  and Ctrl-C ending nested ones, `+` continuation lines, the `*` escapes and the `=` rule: all as
  the ROM Shell, with the same messages on the same stream.

## The csh language

The common part of bash (above), on Amiga terms: Amiga paths (`DH0:`, `Work:`, `/` is parent),
no ixemul, no fake Unix paths, no fake `fork`.

- **Pipes**: each stage runs in a process of its own with a CLI (`NP_Cli`), running the command
  with `RunCommand` as above, joined by `PIPE:` (queue-handler). Known traps: `WaitForChar` never
  reports data on 3.2.3's PIPE:, open the reading end before starting the writer, a full RAM:
  breaks pipes (`Knowledge/amiga/cli-tools-and-archives.md`). `$?` / `$RC` is the last stage's.
- **Jobs**: `cmd &` runs it the same way, in the background, with its output to the console;
  `jobs`, `fg`, `kill` (Ctrl-C to the job's process). On the Amiga a stuck program cannot be
  killed by force; csh says so instead of pretending.
- **Built-ins of its own** for what AmigaDOS lacks in scripts (`AmigaTools/commands.txt`): `test`
  / `[`, `read`, `printf`, `time`, `source`, `type` (where a command comes from), `history`.
  Programs like `ls`, `cp`, `mv`, `wc`, `tail` stay programs (CUtils and friends), not built-ins.

## The prompt: editing the line

From CCON (which already feels like fish/bash/zsh) and CTerm's console:

- History: Up/Down, **Ctrl+R** search, shared between csh tabs, kept on disk
  (`ENVARC:CShell/history`, the last 500 lines).
- **Grey suggestion** from history while typing, → takes it (fish).
- **The line coloured** as typed: a command that exists green, an unknown one red, strings,
  variables, options, comments in their colours.
- **Tab completion**: commands (aliases, built-ins, resident list, `C:` and every directory of a
  multi-assign, the Path), files and drawers, assigns and volumes, variables after `$`; a list
  under the line when there are several. Arguments of Amiga commands from their templates (open
  question: where csh gets the templates).
- KingCON's keys from CCON: Ctrl+P repeats an argument, Alt+Left/Right by word, Ctrl+ by path part.
- **Every key in `ENVARC:CShell/csh.keys`**, written with the defaults on the first start, the
  same file form as Sumi's and Maki's.
- The **prompt** is configurable: the Amiga escapes (`%N` CLI number, `%S` the directory, `%R`
  the return code) and colours; the default looks like the Amiga's `1.Work:>` with the directory
  and a red return code when the last command failed.

## Colours

- csh colours its prompt and the typed line.
- File lists are coloured by whoever lists. csh's `ls` is **CUtils' `ls`**
  (`AmigaTools/cutils`, already colours drawers and the hidden class grey, colours only on a
  terminal), rewritten in C and taught the kinds: drawers, icons (`.info`), hidden (`h` bit),
  archives, programs, scripts (`s` bit), pictures, modules, text.
- **One colour file for the family** (csh's `ls`, Ahiru, Maki): kinds and their colours from the
  ANSI sixteen, so CTerm's theme still decides the shades. Name and form open; Ahiru's colours
  are the start.

## Files

| File | What |
|---|---|
| `C:csh` | the shell |
| `S:CShell-Startup` | run when csh starts (aliases, prompt), like `S:Shell-Startup`; a csh script |
| `ENVARC:CShell/csh.keys` | the keys |
| `ENVARC:CShell/csh.prefs` | settings (history length, colours of the line, suggestions on/off) |
| `ENVARC:CShell/history` | the history |

## What it takes from earlier work

Knowledge and ideas, not code copied blindly (CCON and CUtils are Amiga E, csh is C):

- **CCON** (`AmigaTools/ccon`): the line editor's feel, KingCON's keys, command completion from
  the resident list, `C:` and the Path, the config rule (built-in < file < given now).
- **CTerm** (`CTerm/src/console.c`): the 3.2 Shell handshake, completion against the current
  drawer, Ctrl-C delivery (CHANGE_SIGNAL: signal the port's task), a Shell on a FileHandle.
- **The console notes** (`Knowledge/amiga/console-protocol.md`): what Ed, More and List expect.
- **CUtils**: `ls`.
- **Sumi / Maki**: `tui.c` input, the keys file, the PC test harness.
- **The Guru Book's sample shell** (`Knowledge/manuals`, the shell chapter and its UserShell.c):
  the main loop, the search order, what to do around each command.

## Built for

AmigaOS 3.2, 68020 or better (as CTerm). Small and quick on a stock 14 MHz A1200 for running
commands and Amiga scripts; the comforts (suggestions, colouring) may be lighter there. Parts
loaded only when used where it pays (the user's rule): the csh-language interpreter and the
completion tables can be in separate parts if the core grows past ~60 KB.

## Testing

- **The PC harness** (fake Amiga headers, gcc with ASan): the line parser, both languages'
  expansion and quoting rules, the Amiga script interpreter's flow. Thousands of cases in seconds.
- **Against the ROM Shell, in FS-UAE**: the same script run by the ROM Shell and by csh, output and
  return codes compared byte for byte. Every Amiga-script feature gets such a pair; the scripts in
  the 3.2 `S:` drawer and a set from Aminet are the regression suite. The manual's `Skip BACK`
  loop with `Eval` is the first test.
- **On the A1200** last, with the user, after the emulator says yes.

## Plan

0. **Questions to the ROM Shell** (FS-UAE, 3.2): the open questions below that block design;
   write the answers into `Knowledge/amiga/writing-a-shell.md`.
1. **Runs commands**: csh in a CTerm tab; the search order, `RunCommand`, `CD`, `Path`,
   `Prompt`, redirection, return codes, Ctrl-C, `exit`; a plain line editor.
2. **Amiga scripts**: the main loop on `cli_CurrentInput`, the internal commands (the ROM's or
   csh's own), `.KEY`, s-bit scripts; tested pair by pair against the ROM Shell.
3. **The prompt**: history and Ctrl+R, completion, suggestions, colouring, `csh.keys`.
4. **The csh language**: variables, quoting, `$(...)`, `;`, `&&` / `||`, `if` / `for` / `while`
   / `case`, functions, `*`, here-documents, `$((...))`, `#!csh` scripts, `S:CShell-Startup`.
5. **Pipes and jobs**.
6. **`ls` and the family's colour file** (with Ahiru and Maki).
7. **CTerm**: csh as the command a new tab starts (a setting). Later, maybe, csh as the user
   shell for every Shell window.

Each step is a beta (`0.1b1`...) tried in FS-UAE, then on the A1200.

## Open questions

Need the real 3.2 ROM Shell (step 0):

1. Does `RunCommand` on an internal command's segment (CD, If, Skip, Set...) work from a program
   that is not the ROM Shell, and do `If` / `Skip` then read ahead through `cli_CurrentInput`?
2. The exact list of internal commands on 3.2 (walk the resident list for `seg_UC == -2`): are
   Run, Execute, NewShell, Which internal?
3. In `.KEY`, what `/S`, `/N`, `/M`, `/F` do and what a set `/S` puts in.
4. Can redirection go anywhere in the line on 3.2? Is there an error-stream redirection?
5. Native `|` on 3.2: the syntax, the handler, return codes and Ctrl-C across a pipe.
6. Expansion order on a line (alias, `$var`, backquotes, `*` escapes, redirection); is `$var`
   expanded inside quotes?
7. The exact texts of "failed returncode", "Unknown command", Skip's "not found", and which stream.
8. Implied CD and s-bit / ARexx detection rules; does an `e`-only file without `s` run?
9. Prompt escapes beyond `%N %S %R` on 3.2.

Need a decision later:

10. Where csh gets Amiga commands' templates for completion (a shipped table for 3.2's `C:`, read
    once by running `cmd ?` with empty input, or both).
11. The family colour file's name and form.
12. Whether csh ever becomes the user shell (needs pure code: libnix's `-resident` start-up?).
