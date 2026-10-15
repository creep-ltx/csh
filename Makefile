# CShell (csh) - the shell for AmigaOS 3.2. Cross-built with Bebbo's gcc
# (~/opt/amiga). -fbbb=-: bebbo's backend pass off, as for every program of
# the terminal family (Knowledge/amiga/toolchain-and-testing.md).
# make test: the PC checks of the line rules (tests/run.sh).

AMIGA   = $(HOME)/opt/amiga
CC      = $(AMIGA)/bin/m68k-amigaos-gcc
CFLAGS  = -Os -m68020 -noixemul -fomit-frame-pointer -fno-common -fbbb=- -Wall -Wno-pointer-sign
LDFLAGS = -noixemul -s
SRC     = src/main.c src/run.c src/line.c src/term.c src/keys.c src/prefs.c src/hist.c src/comp.c src/edit.c src/lang.c src/builtin.c src/os.c src/job.c src/regex.c src/bang.c
OBJ     = $(patsubst src/%.c,build/%.o,$(SRC))

# cshell: csh as the system's user shell (Resident shell C:cshell REPLACE):
# pure, so -resident32 (every Shell window a copy of the data, a4 at it)
# and its own start-up, src/shellstart.S (the start-up packet first).
SOBJ    = $(patsubst src/%.c,build-shell/%.o,$(SRC))
SFLAGS  = $(CFLAGS) -resident32 -DCSH_USERSHELL

all: csh cshell cshr useshell

cshell: build-shell/shellstart.o $(SOBJ)
	$(CC) -noixemul -resident32 -m68020 -nostartfiles -s build-shell/shellstart.o $(SOBJ) $(SLIBS) -o $@

build-shell/shellstart.o: src/shellstart.S
	@mkdir -p build-shell
	$(CC) -m68020 -c $< -o $@

# cshr: the same code started as a command, for "Resident csh C:cshr": one
# copy in memory for every csh started from a Shell (CTerm's shell=csh
# tabs). Its own start-up (no start-up packet): telling a Shell start from
# a command start at run time was tried and is not safe (System() Shells
# come with a CLI, cli_Module set too).
cshr: build-shell/cmdstart.o $(SOBJ)
	$(CC) -noixemul -resident32 -m68020 -nostartfiles -s build-shell/cmdstart.o $(SOBJ) $(SLIBS) -o $@

build-shell/cmdstart.o: src/shellstart.S
	@mkdir -p build-shell
	$(CC) -m68020 -DCSH_CMDSTART -c $< -o $@

# useshell: after "Resident shell C:cshell REPLACE" with cshell already the
# Shell, makes the new code the one new Shells run (tools/useshell.c)
useshell: tools/useshell.c
	$(CC) -Os -noixemul -s tools/useshell.c -o $@

# cshell-ext.rom: cshell as a ROM module, for the emulator (tests/romuae.sh)
cshell-ext.rom: cshell build-shell/rominit.bin tools/mkextrom.py
	python3 tools/mkextrom.py cshell build-shell/rominit.bin $@

build-shell/rominit.bin: src/rominit.S
	@mkdir -p build-shell
	$(CC) -m68020 -c $< -o build-shell/rominit.o
	$(AMIGA)/bin/m68k-amigaos-objcopy -O binary -j .text build-shell/rominit.o $@

build-shell/%.o: src/%.c src/csh.h src/line.h src/edit.h src/lang.h
	@mkdir -p build-shell
	$(CC) $(SFLAGS) -c $< -o $@


csh: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

build/%.o: src/%.c src/csh.h src/line.h src/edit.h src/lang.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

test:
	tests/run.sh

clean:
	rm -rf build build-shell csh cshell cshr useshell

.PHONY: all test clean
