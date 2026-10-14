/* csh.h - CShell, the shell for AmigaOS 3.2 (docs/design.md). */
#ifndef CSH_H
#define CSH_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include "line.h"

#define CSH_VERSION "0.1b6"
#define CSH_DATE    "14.10.2026"

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
    LONG bg0;              /* cli_Background when csh started (EndCLI sets it) */
    int broke;             /* a Ctrl-C stopped csh's language since the last prompt */
    int usershell;         /* cshell: csh is this Shell itself (Resident shell ... REPLACE) */
    int system;            /* ... started by System(): ends when its commands do */
    char outercf[256];     /* the script csh was started from (its T: file is not csh's to delete) */
    char self[300];        /* csh's own file (PROGDIR: and its name): a csh of its own for & runs it */
};

extern struct csh csh;

/* run.c */
void run_line(const char *line, int depth);
LONG run_cmd(struct cmdline *c, int depth);
LONG run_internal(const char *name, const char *args, struct cmdline *redir);

/* run.c: a command found and loaded for a pipe stage or a job (job.c) */
struct prepared {
    BPTR seg;                   /* the code (0: romline instead) */
    struct Segment *res;        /* a resident entry: its count raised */
    BPTR progdir;
    LONG stack;
    char romline[512];          /* a script: the line for the ROM Shell */
};
int run_prepare(struct cmdline *c, struct prepared *p);
int run_exists(const char *name);
void run_end(void);             /* csh ends: run.c's FileInfoBlock given back */
void run_release(struct prepared *p, int detached);
LONG run_prepared(struct cmdline *c, struct prepared *p);

/* job.c */
void job_news(void);
int job_count(void);
int job_running(void);
void job_leave(void);
int job_spawn_prepared(void *job, struct cmdline *c, long in, long out, struct prepared *p);
long job_lastrun_prepared(void *job, struct cmdline *c, long in, struct prepared *p);

/* lang.c: the csh language */
int lang_isscript(const char *path);              /* "#!csh" on its first line */
LONG lang_runscript(const char *path, struct cmdline *c);
int lang_line(const char *line, int interactive); /* a line typed at the prompt */
extern int lang_more;                             /* lang_line wants another line */
int run_capture(const char *cmd, char *buf, int size);
void script_end(void);
LONG script_now(BPTR until);    /* main.c: an Execute's lines run now (from the csh language) */
void set_rc(LONG rc, LONG r2);
int ltoa10(LONG n, char *b);
int csh_tag(char *b);           /* "<CLI number>-<this csh>": temp names no other csh shares */

#endif
