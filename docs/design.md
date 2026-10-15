# CShell (csh): a modern shell for the Amiga

*Design written 2026-10-12 with the user; steps 1-7 built 2026-10-13 (csh 0.1b1, ls 0.1, CTerm 0.3b32's shell setting). Everything here is proposed unless it
says decided. The facts behind it are in `~/Projects/Knowledge/amiga/writing-a-shell.md` (from the
AmigaDOS manual, the Amiga Guru Book's shell chapter and the verified notes); a line there marked
read or memory is not yet proven on the 3.2 ROM.*

## Start here (a later session)

**Parallel sessions (set up 2026-10-15, the user's wish):** CTerm, csh and CUtils may each have a
session running at the same time. Stay in your own repo: CTerm's session owns `~/Projects/CTerm`,
csh's owns `~/Projects/CShell`, CUtils' owns `~/Projects/CUtils`; a change another one needs is a
note in its design.md or a message, never an edit there. Shared things:
- **FS-UAE on port 1240** (`~/Projects/CTerm/tests/files/uae.sh`) is used by all three: work in
  your own RAM: drawer (CUtils: `S=x tests/run.sh`), never `uae.sh stop`/restart it without
  asking the others (`ListAgents`, then a message); a crash there - tell the others before restarting.
- **The A1200** (Wasabi): message the others before installing, rebooting or restarting wasabid;
  quit test programs and say when the screen is free. Installing in C:/LIBS: needs the user's go.
- **Ahiru and Maki's FileColours** is on both lists: the csh session does it (in those repos), not CUtils.
- **Next for csh, in this order:** (1) `_CMDLINE` - while a program runs, csh keeps its whole
  command line in the local variable `_CMDLINE` (set where cli_CommandName is set in run.c,
  removed after); the CTerm session reads it for saved layouts, so tell it when it is in and in
  which version; (2) size - the language as a part loaded on first use; (3) jobs: Ctrl-Z, `$!`,
  `disown`, `wait -n`; (4) completion of a keyword's value (`TO`, `LFORMAT`), menu selection;
  (5) the small bash bits in "What csh still lacks"; (6) FileColours in Ahiru and Maki.
  Install on the A1200 only when the user says so (as always, csh and cshell together).

**0.1b8 (2026-10-14, INSTALLED with C:cshell and `Resident shell C:cshell REPLACE`; 0.1b7 kept as
C:csh-0.1b7.bak, C:cshell-0.1b7.bak):** `kill` with a target that is not a %job (a Shell's number,
a name, a -flag) is passed to the kill command (CUtils' kill: by CLI number, 0x address or name),
as `wait` does for Wait. Open Shells keep 0.1b7 until closed.

**0.1b7 (2026-10-14, installed with C:cshell, 0.1b6 kept as .bak):** csh ends when its input
ends while it waits at the prompt (raw read of 0: a CTerm tab or pane closed) - before, the tab
stayed open with csh in it. Goes with CTerm 0.3b35.

**State at the end of 2026-10-14: csh 0.1b6 INSTALLED on the A1200** (the user asked): C:csh and
C:cshell 0.1b6 (C:csh-0.1b5.bak kept); **cshell is the Shell at every boot**: the first block of S:User-Startup
(`;BEGIN CShell` ... `If EXISTS C:cshell` / `Resident shell C:cshell REPLACE`; the old file kept as
S:User-Startup.bak; checked after a reboot: a NewShell runs csh's language), C:ls 0.2, CTerm 0.3b34 with `shell=csh`. 0.1b6 has bash's missing parts (below);
all tests green (PC 61 + 255 checks, 5 language scripts and 12 ROM pairs on the A1200, cshell's
shelltest on the A1200, no memory lost over 10 runs of tests/lang/bash.csh). CTerm **0.3b34**
(bracketed paste fixed) INSTALLED 2026-10-14 (the user asked; CTerm-0.3b33.bak kept).
**Install the two together**: cshell's `&` of csh's own commands needs C:csh of its own version.

**The user's rule for csh (2026-10-13): the best of bash, zsh and fish; when in doubt ask how
they handle it; fast (never slower than the ROM Shell, tests/bench.sh); in the end a full shell
replacement.**

**Done 2026-10-14 (the user: "missing bash features should go in"; * expanded, csh's own commands
with &, prompt extras - "fix"):**
- arrays (indexed, `declare -A`), `declare/typeset/local/readonly` (words not split: `local
  y=$x`), `${x:o:l}` `${x/a/b}` `${x//a/b}` `${x^^}` `${x,,}` `${!x}` `${!a[@]}` `${a[@]:1:2}`,
  `$'..'`, `$RANDOM` `$SECONDS` `$-`, `set -u -f -o pipefail/-o name/-H`, `PIPESTATUS`.
- arithmetic: `++ -- += -= *= ... ?: ** & | ^ << >> ,` (bash's order; the untaken side makes no
  assignments), `16#ff 2#101 010`, `(( ))`, `let`, `for ((;;))`, `for ... { }`.
- `[[ ]]` (no splitting, `==` patterns, `=~` regex in `src/regex.c` - a Pike VM, POSIX ERE
  plus `\w \s`, `BASH_REMATCH`), `-v -nt -ot`.
- `trap` EXIT/INT/ERR/RETURN (HUP, TERM... taken, never come), `getopts`, `select`, `mapfile`,
  `read -a`, `printf -v` and `%f %e %g` (worked on the decimal text, half to even on a tie: no
  float code linked), `cd -`, `cd ..` (is `/`), `cd` alone (HOME, else SYS:), `pushd popd dirs`,
  `unset -f`, `|&`, `<(cmd)` (a T: file).
- `( )` and `$( )` are subshells in effect: the local variables and aliases, current dir,
  functions, arrays, `$1..`, options are put back after (`snap_take/snap_give`, `os_snap`).
- `*` `?` `[..]` match files in csh (bash): `echo *.txt` lists them; an Amiga command gets the
  files - except one whose template takes one name (no `/M`: Dir, CD), which gets the pattern as
  `#?` as on the Amiga; a line too long falls back to the pattern; no match: the pattern goes on
  (`#?.none`, the program says "no match"). `?` alone stays ReadArgs' help. `set -f` stops it.
- csh's own commands with `&`, and a loop/function/block in a pipe's middle: a csh of its own
  (the same file, `csh.self`) runs a T: script with the functions, arrays, `$1..` and options;
  the local variables and aliases come with NP_Cli (verified). Streams; `f | phead 3` with an
  endless `f` ends (the last stage done, Ctrl-C after a second). A quick built-in in the middle
  (echo, printf) still writes a T: file.
- at the prompt: `!! !$ !^ !* !n !-n !str ^a^b` (`src/bang.c`, shown before it runs, as bash);
  `PROMPT`/`PS1` with zsh's escapes (`%~ %c %V %T %* %D %j %M %E %h %F{red} %K{..} %B %U
  %(?.a.b) %(j.a.b)`, `$var` and `$(cmd)` expanded first), `RPROMPT` (right edge, gone when the
  line reaches it), `PS2`, `$CMD_DURATION` (ms, fish). Ctrl-D stops csh's own loops even when an
  INT trap takes Ctrl-C; Ctrl+L in CTerm clears with codes CTerm does not take for a whole-screen
  program (its FF/CSI H/CSI 2J from a raw reader moved csh to CTerm's other screen).
- a pipe's commands are found and loaded once, before any starts (they were looked up twice):
  pipes in the csh language 0.99 of the ROM's.

**Used by others:** CTerm's saved layouts read the tab Shell's cli_SetName (current drawer) and
cli_CommandName (csh sets it while a program runs, run.c) - keep both right. CTerm 0.3b35+ ends a
closed tab's input for good; csh 0.1b7 ends on a raw read of 0 (keep that, or tabs stay open).
Idea from CTerm: keep the running command's whole line in a local variable (`_CMDLINE`) so a
saved layout can start the program again with its arguments.

**What csh still lacks** (the list to work from):
- size: 137 KB (was 95): the language could be a part loaded when first used (the user's rule:
  modular, load on demand); never measured slower to start (84 ms either way on the A1200).
- jobs cannot be stopped (Ctrl-Z), `kill` is a Ctrl-C; `$!`, `disown`, `wait -n` missing.
- completion: a keyword's value (`TO`, `LFORMAT`), menu selection; `**/` (globstar), extglob.
- `exec`, `command`, `printf %q`, `${x@Q}`, `case ;&`, `read -n/-t/-s`, `declare -i`.
- no git: `$(...)` in PROMPT is the way to show anything else.
- Maki does not read ENVARC:FileColours yet (Ahiru does since 0.3b3, 2026-10-15; `ls` is CUtils').
- The user uses csh daily on the A1200 (said 2026-10-15): ask what they ran into before new work.

1. Read this file, then `Knowledge/amiga/writing-a-shell.md`. **The 2026-10-13 audit
   (`docs/audit-2026-10-13.md`) and its fixes: csh 0.1b4 and ls 0.2, installed on the A1200
   2026-10-13 (the user asked; C:csh-0.1b3.bak and C:ls-0.1.bak kept), tried in CTerm there (prompt,
   pipes, colours). Not yet tried: a marked paste in CTerm.** Four nameless tasks (2048 stack,
   pri 2) on the A1200 look like break relays left by an older csh, waiting in freed memory: ask the
   user before removing them. **0.1b5 (2026-10-14)**: cshell, the user shell (see "csh as the user
   shell"); csh itself: run.c's FileInfoBlock freed at the end (260 bytes a run, measured), a pipe
   whose command after the first is missing starts nothing (a writer would wait for ever on 3.2's
   PIPE:), and once a pipe's last stage is done the others get a Ctrl-C after a second. 0.1b5 installed
   on the A1200 2026-10-14 (C:csh-0.1b4.bak kept; the user asked), the four leftover relay tasks
   removed there with `wasabi kill --force` (the user agreed). cshell and the ROM: FS-UAE only.
2. **Step 0 is done** (2026-10-13): the answers are in the knowledge note's section D, the probes in
   `tests/probe`.
3. **Done** (all 2026-10-13, tried in FS-UAE and on the A1200):
   - **Step 1, csh runs commands** (`src/run.c`, `src/line.c`): the ROM's line rules, search order
     and RunCommand on csh's own process; the ROM's internal commands for CD, Set, If, Skip,
     Execute, Prompt...; FailAt, `Set echo on`, Ctrl-D, EndCLI. Amiga scripts (Execute, s-bit,
     `csh FROM file` without `#!csh`) run by these rules, and come out byte for byte as in the
     ROM Shell (step 2 mostly came free with the ROM's own internal commands).
   - **Step 3, the prompt** (`term.c`, `edit.c`, `comp.c`, `hist.c`, `keys.c`, `prefs.c`): csh edits
     its line itself in raw mode; colours as typed, the grey suggestion, Up/Down by prefix, Ctrl+R,
     Tab (commands, files, assigns, `$vars`), `ENVARC:CShell/csh.keys` and `csh.prefs`, history in
     `ENV:CShell/history` (shared between tabs, saved to ENVARC every 20 lines and at the end).
   - **Step 4, the csh language** (`lang.c`, `builtin.c`, `os.c`): at the prompt and in `#!csh`
     scripts. The words go back to Amiga commands as an Amiga line (`amigaarg()`).
   - **Step 5, pipes and jobs** (`job.c`): each Amiga command of a pipe in a process of its own
     (NP_Cli) joined by PIPE:, csh finding and loading it (run_prepare); the last stage in csh
     itself, with the console's breaks pointed at a relay task so Ctrl-C reaches every stage; a
     csh stage (built-in, loop) last reads the pipe in csh, in the middle writes a T: file.
     Amiga-script pipes run the same way, ROM-exact (every stage looked for first). `&`, `jobs`,
     `fg`, `kill`, `wait` (`wait 5` is the Amiga's Wait); "[n] Done" at the next prompt; exit
     with jobs running warns once, then leaves csh's code in memory for them.
   - **Step 6, `ls` and the family colour file** (`tools/ls.c`, retired 2026-10-15 for CUtils' ls, 14 KB): CUtils'
     ls in C (`-1aAdFhlrRSt`, columns from `$COLUMNS` which csh keeps, else the console asked;
     natural order; ExAll), coloured by kind from **`ENVARC:FileColours`** (`KIND CTERM,PLAIN`
     SGR numbers as in csh.prefs, `.EXT KIND`), written with the defaults when missing.
4. **Testing:** `make test` (PC, ASan: 57 line-rule and 105 language checks, `tests/fakeos.c`
   stands in for the Amiga); `tests/pairs.sh` (ROM Shell vs csh, 12 scripts), `tests/langtest.sh`
   (`tests/lang/*.csh` vs `.want` on the real ROM: basic, pipes, ls), `tests/bench.sh` (speed
   against the ROM Shell). All run against `CTerm/tests/files/uae.sh`'s FS-UAE (`tests/probe/w`);
   with `W=probe/a` on the A1200 (shared: check `wasabi ps` first, clean RAM: after).
5. **Speed (the user's rule: never slower than the ROM Shell)**, A1200, best of 3, csh/ROM:
   Amiga scripts 0.80-0.98; the same jobs in the csh language 0.14-0.99 (pipes 0.98 / 0.99, a
   loop with `[ ]` and `$(( ))` 0.14). `ls SYS:C` in a window 0.70 s against C:Dir 1.80 s
   (FS-UAE). Keep `tests/bench.sh` green after every change.
6. **Step 7 is done in CTerm 0.3b32**: `shell=` in CTerm.prefs (Settings, Files: "Then runs"),
   or `CTerm SHELL csh` for one run; the tab's Execute() string gets the shell as its next line,
   so the ROM Shell stays under csh.
   **Installed on the A1200 2026-10-13 (the user agreed):** C:csh 0.1b3, C:ls 0.1 (the user's
   CUtils ls kept as C:ls-cutils.bak), CTerm 0.3b32 with its parts in Work:Utils/CTerm (0.3b31
   kept as CTerm-0.3b31.bak and Parts-0.3b31.bak), and `shell=csh` in ENVARC:/ENV:CTerm.prefs:
   every new CTerm tab starts csh.
   **0.1b3 + CTerm 0.3b33:** keys typed while a command runs stay keys (CTerm's `CSI ?7727 h`,
   set by csh in term_open, reset on exit), csh reads one key at a time (what follows the
   Return stays for the command or the next prompt). `csh FROM`/`COMMAND` end the break relay
   (a relay left behind waited in freed memory; four were found on the A1200 and removed with
   `wasabi kill --force`).
   **Added 2026-10-13 evening:** `~` (HOME, else SYS:), `{a,b}` and `{1..5}`, `local` in
   functions, `S:CShell-Startup` (run once at the first prompt, the csh language), Tab
   completes a command's own keywords (read from its file: the longest run of ReadArgs parts,
   a few stray bytes before it allowed; the ROM's internal commands from a table).
   **Next:** Maki reading `ENVARC:FileColours` (Ahiru does, 0.3b3);
   loading parts on demand (csh is 80 KB; the language could be a part); what the user finds.
7. Build like Sumi and Maki: bebbo's gcc in `~/opt/amiga`, `-fbbb=-`.

## Decided while building (2026-10-13)

- **false is 1** (`test`, `!`, `false`): bash's value; `&&`, `||`, `if` take 0 as true and any
  other rc as false, so an Amiga command's 5 (Search not finding) is false too.
- **Built-ins and Amiga commands of the same name**: commands are any case, so the Amiga one
  keeps its place where they clash: csh's `eval` and `time` only in lower case (C:Eval does
  sums), `break` only inside a loop (C:Break), `echo` with NOLINE/FIRST/LEN/TO, `alias` without
  `=`, `set` with nothing after it go to the ROM's own. No `type` built-in (C:Type prints files;
  Which says where a command is). `Set name value` and `set name value` both set.
- **A word back to the Amiga**: a word that had quotes is quoted the Amiga way (`*` -> `**`, `"`
  -> `*"`, LF -> `*N`); `KEY=value` unquoted stays as it is (ReadArgs' keys).
- **`*` matches files in csh (2026-10-14, the user's rule: as bash/zsh/fish)**; superseded: "an
  unquoted `*` becomes `#?` and the program matches". A command whose template takes one name
  (no `/M`: Dir, CD) still gets the pattern as `#?`; so does a line the files make too long, and a
  pattern nothing matches. `*` alone is no longer the console (`CONSOLE:` is). `#?` is never
  matched by csh: it stays the Amiga's own way to hand a program a pattern.
- **Ctrl-D stops csh's own commands** even when `trap ... INT` takes Ctrl-C (2026-10-14), as it
  stops an Amiga script: the way out of a loop whose trap catches every Ctrl-C.
- **csh's own command in a process of its own** (`&`, a pipe's middle) is a second csh running a
  T: script (2026-10-14): bash's subshell, what it changes stays there.
- **`#` is a comment at a word's start, except an Amiga pattern** (`#?`, `#(`, `#[`, `##`, `#~`).
- **`$( )` and `( )` are not subshells** (no fork on the Amiga): they run in csh itself, so a
  variable set inside stays set.
- **`>&2` writes to `*`** (the console), the Amiga's error stream for a program without one.
- **Colours in CTerm are 90-97** (its Shell tab takes 30-37 as the Amiga's pens).
- **The prompt**: with the ROM's default prompt (`%N.%S> `) a failed rc shows before the `>`.
- **A pipe's last Amiga command runs in csh** (as the ROM Shell's does: a process fewer), Ctrl-C
  given to every stage by a relay task (ACTION_CHANGE_SIGNAL); a job writes to csh's console.
- **The family colour file is `ENVARC:FileColours`**: `KIND CTERM,PLAIN` with SGR numbers (the
  form of csh.prefs; CTerm's theme colours are 90-97), `.EXT KIND`. Kinds: drawer link program
  script archive picture sound source text icon hidden. A program is a name with no ending and
  the e bit (Ahiru's rule): reading each file's hunk header was exact but made `ls` of C:
  slower than C:Dir.

## csh as the user shell: cshell (2026-10-14, step 1 of "csh in a ROM")

The user asked (2026-10-13): could csh replace the built-in shell in a custom ROM, in the emulator, for
fun? Three steps agreed: (1) csh as a real user shell, (2) tried with `Resident shell C:cshell REPLACE`,
(3) a ROM image with the shell module swapped (Remus/Capitoline, or LoadModule). **All three done in
FS-UAE (2026-10-14); see "csh in a ROM".** Never on the A1200 unless the user asks.

- **`cshell`** is built from the same source (`make cshell`, `-DCSH_USERSHELL`): one binary, one code
  for every Shell window. **Pure** through `-resident32`: each start gets its own copy of the data
  (47 KB), a4 pointing at it. Its own start-up, `src/shellstart.S` (libnix's nlrcrt0.S changed): the
  start-up packet taken first (before any library code could get it as an answer to a packet of its
  own), no Workbench path, no command line, `_keepdata` (jobs left running at the end keep the data).
- **Other tasks of csh's** (a pipe stage's process, the break relay) have no a4: `_stage_stub` takes it
  from the struct stage (first long; the process's argument is its hex address), `_relay_stub` from
  the relay task's tc_UserData.
- **main.c**: `shell_init` (the Guru Book's ShellInit.c: Res2 != 0 refused, segArray[3] to [4],
  CliInitNewcli / CliInitRun, the packet answered at once for NewShell, Run and an asynchronous
  System(), at the end for a synchronous one with the rc), `shell_end` (the streams closed by the
  flags, the current dir unlocked). **CliInitRun changes registers it should keep** (a4: csh lost its
  data and wrote nowhere, then crashed in the next command): called through `_cliinit` in shellstart.S,
  every register saved.
- **3.2 facts seen** (FS-UAE, Kickstart 47.96): NewShell and Run both start the shell with Res1 0
  (CliInitRun), and CliInitRun's answer had no bit 31 (a ROM address, not flags): the "flags not
  valid" path of the Guru Book is the one taken. **wasabid's `run` (System()) uses the user shell**:
  with cshell installed every `wasabi run` goes through it - its lines by the Amiga script rules, as
  the ROM Shell would.
- In the loop: a background Shell (Run, System()) never shows a prompt; System() ends when its command
  lines do; commands get cli_StandardInput as their input (not the command stream: Type failed).
- Tested: `tests/shelltest.sh` (System() lines and rc, 10 System() shells leave no memory, a NewShell
  window with csh's language, exit closes it; the BootShell put back with `tests/probe/bootshell`).
  By hand: two cshell windows at once, pipes, `&` jobs, `Run`, EndShell.

## csh in a ROM (2026-10-14, step 3: done in FS-UAE)

`make cshell-ext.rom` (tools/mkextrom.py, src/rominit.S), `tests/romuae.sh start` boots the private
FS-UAE with it, `tests/romtest.sh` checks it (8 checks, all green 2026-10-14). The 3.2 Kickstart file
is not changed: cshell lives in a 256 KB second ROM.

- **Where**: exec looks for modules in $F80000-$1000000 and $F00000-$F80000 (its range table, read
  from 3.2's exec at $F80410). UAE's own boot ROM (for a drawer given as a hard drive) is at
  $F00000-$F0FFFF; FS-UAE's `uae_kickstart_ext_rom_file2` maps a file from $F00000 with that first
  64 KB under UAE's (its `..._address` option was refused), so the module starts 64 KB into the file:
  RomTag "cshell" at $F10100, the code at $F101C4 (relocated there by mkextrom.py), the data hunk
  after it (read-only: shellstart.S copies it to RAM for each Shell).
- **How it becomes the shell**: the RomTag has RTF_AFTERDOS; its init (rominit.S) points dos's
  "shell" entry at cshell's seglist, as `Resident shell ... REPLACE` would, before the Startup-Sequence.
- **The boot stays the ROM Shell's**: 3.2 starts even the boot CLI through "shell", and only the ROM
  Shell puts its internal commands (Run, CD, Echo...) into the resident list, when it first runs.
  So cshell's start looks for "CD" first: not there means this is the boot Shell, and it jumps to
  the ROM Shell's code (the BootShell entry) with the start-up packet still waiting. Two dead ends
  first: a "shell" module of version 48 replacing the ROM's (the internal commands went with it) and
  pointing BootShell at cshell (the same) - the boot stopped at its first line, `run`.
- After the boot, every Shell is csh: NewShell and the Shell icon, Run, System() (wasabid's `run` too).
- Seen with a debug build: FS-UAE's `serial_port` must name a file that exists (`: > file` first);
  then KPrintF lands there from the first moment of the boot, before wasabid (romuae.sh: SERIAL=).
- Stopping FS-UAE (systemctl stop) prints a segfault of fs-uae on the PC: after the tests, harmless.

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
| wildcards | the program matches `#?` itself | the shell expands `*` | csh expands `*` (2026-10-14); a command whose template takes one name gets `#?` and matches itself; `#?` typed goes to the program as it is |
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

- **Decided by step 0: use the ROM's own internal commands for them** (verified: RunCommand on
  their segments works from csh's process and If/Skip/Execute move `cli_CurrentInput`) (the Guru Book says third-party
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
- **Retired 2026-10-15 (the user asked): csh's own `ls` (`tools/ls.c`) is gone**; `ls` is CUtils'
  (`~/Projects/CUtils`, src/ls.c, its own tests), C:ls 0.3b1 on the A1200 since 2026-10-15, which
  grew from it and reads the same FileColours (lib/cu_colours.c). Below is the history.
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
7. **CTerm**: csh as the command a new tab starts (a setting) - done, CTerm 0.3b32. Later,
   maybe, csh as the user shell for every Shell window.

Each step is a beta (`0.1b1`...) tried in FS-UAE, then on the A1200.

## Open questions

Answered by step 0 (2026-10-13, verified in FS-UAE on 3.2; details in the knowledge note,
section D). What it means for csh:

1. **The ROM's internals run from csh** with `RunCommand`; If/Skip read ahead through
   `cli_CurrentInput`; Execute makes the T: copy and points `cli_CurrentInput` at it (inside a
   script it swallows and closes the old input). csh never caches `cli_CurrentInput` and deletes
   `cli_CommandFile` at its end.
2. **Internal on 3.2**: also Run, Execute, NewShell, History, PushCD/PopCD/SwapCD, `.key`/`.bra`/
   `.ket`, `(`. Which is not. csh's own `History`, `Alias` and `CD` (the prompt's) replace those;
   the rest are the ROM's.
3. `.KEY`: `/S` = the keyword itself, `/N` checked, `/M` space-joined, `/F` the rest, `/K` both forms.
   Free with the ROM's Execute.
4. Redirection anywhere; **`*>` / `*>>` = the error stream** (csh: `2>` as its own spelling of `*>`
   in the csh language, `*>` everywhere).
5. Native `|` needs spaces, rc = the last stage's, Ctrl-C does not reach earlier stages (csh can do
   better: send it to every stage).
6. Expansion: `$var` inside quotes too, variables before redirection and `;`, alias `$var` at
   definition; the `=` trap holds. Amiga scripts copy this exactly.
7. Messages: all on the normal output; Skip's is "object not found".
8. Implied CD yes; s bit = script, `/*` = ARexx; e-only text does not run.
9. Prompt: only `%N %S %s %R %%`; csh adds its own escapes.

Still open: EndCLI in csh (step 1); which pipe stage gets Ctrl-C.

Need a decision later:
10. Where csh gets Amiga commands' templates for completion (a shipped table for 3.2's `C:`, read
    once by running `cmd ?` with empty input, or both).
11. ~~The family colour file's name and form~~: decided, `ENVARC:FileColours` (above).
12. Whether csh ever becomes the user shell (needs pure code: libnix's `-resident` start-up?).
