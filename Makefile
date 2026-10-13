# CShell (csh) - the shell for AmigaOS 3.2. Cross-built with Bebbo's gcc
# (~/opt/amiga). -fbbb=-: bebbo's backend pass off, as for every program of
# the terminal family (Knowledge/amiga/toolchain-and-testing.md).
# make test: the PC checks of the line rules (tests/run.sh).

AMIGA   = $(HOME)/opt/amiga
CC      = $(AMIGA)/bin/m68k-amigaos-gcc
CFLAGS  = -Os -m68020 -noixemul -fomit-frame-pointer -fno-common -fbbb=- -Wall -Wno-pointer-sign
LDFLAGS = -noixemul -s
SRC     = src/main.c src/run.c src/line.c src/term.c src/keys.c src/prefs.c src/hist.c src/comp.c src/edit.c src/lang.c src/builtin.c src/os.c
OBJ     = $(patsubst src/%.c,build/%.o,$(SRC))

all: csh

csh: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

build/%.o: src/%.c src/csh.h src/line.h src/edit.h src/lang.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

test:
	tests/run.sh

clean:
	rm -rf build csh

.PHONY: all test clean
