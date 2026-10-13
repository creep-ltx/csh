/* csh.h - CShell, the shell for AmigaOS 3.2 (docs/design.md). */
#ifndef CSH_H
#define CSH_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include "line.h"

#define CSH_VERSION "0.1b1"
#define CSH_DATE    "13.10.2026"

#ifndef CMD_INTERNAL
#define CMD_SYSTEM   -1
#define CMD_INTERNAL -2
#define CMD_DISABLED -999
#endif

struct csh {
    struct Process *me;
    struct CommandLineInterface *cli;
    BPTR in, out;          /* csh's own Input()/Output(): where commands read and write */
    BPTR module;           /* csh's own seglist in cli_Module, put back after each command */
    int fromfile;          /* reading FROM a file: end when it ends */
    int quit;              /* exit typed, or the CLI ended */
    LONG rc;               /* the last return code, csh's own at the end */
};

extern struct csh csh;

/* run.c */
void run_line(const char *line, int depth);
int run_capture(const char *cmd, char *buf, int size);
void script_end(void);
void set_rc(LONG rc, LONG r2);

#endif
