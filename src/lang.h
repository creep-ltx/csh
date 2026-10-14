/* lang.h - the csh language (docs/design.md "The csh language"): the
 * common part of bash on Amiga terms. lang.c reads, parses and runs it;
 * builtin.c has csh's own commands; everything that touches the Amiga
 * goes through the os_ calls below (os.c on the Amiga, tests/fakeos.c on
 * the PC, so the language is tested on the PC by the hundred). */
#ifndef CSH_LANG_H
#define CSH_LANG_H

#include "line.h"

#define SH_FALSE 1               /* what csh's own "false" returns (test, !, false) */

/* ---- what lang.c needs from the system (os.c) ---- */
void *os_alloc(long n);          /* cleared memory, or NULL */
void os_free(void *p);
int os_getvar(const char *name, char *buf, int size);      /* length, -1 = none */
int os_getlocal(const char *name, char *buf, int size);    /* the local one only (not ENV:) */
#define V_LOCAL  0              /* csh's own (an AmigaDOS local variable) */
#define V_EXPORT 1              /* export: ENV:, the local copy that would hide it gone */
#define V_ASSIGN 2              /* x=1: local, or ENV: when x was exported */
void os_setvar(const char *name, const char *val, int how);
void os_unsetvar(const char *name, int env);    /* env: an exported one goes from ENV: too */
void os_end(void);                               /* csh ends: what os.c kept, freed */
int os_getalias(const char *name, char *buf, int size);
void os_setalias(const char *name, const char *val);
long os_run(struct cmdline *c);  /* an Amiga command (searched, run); its rc */
void os_write(const char *s, int n);             /* to the output of the moment */
void os_flush(void);                             /* what os_write gathered, sent */
void os_tty(const char *s, int n);               /* to csh's own console, whatever is redirected (time) */
void os_err(const char *s, int n);               /* the error stream of the moment (2>, else the console) */
int os_readline(char *buf, int size);            /* from the input of the moment; -1 = end */
void *os_redir(struct cmdline *c);               /* open + select; NULL = failed (said) */
void os_unredir(void *save);
void *os_capture(void);                          /* $(...): the output to a file */
int os_captured(void *save, char **text);        /* back; *text os_alloc'ed */
int os_break(void);                              /* Ctrl-C since the last look */
int os_test(const char *path, int op);           /* -e -f -d -r -w -x -s */
int os_glob(const char *pattern, void (*fn)(const char *name, void *u), void *u);
int os_globok(const char *cmd);                  /* the command takes several names (its template has /M) */
long os_mtime(const char *path);                 /* a file's date in seconds, -1 = none */
void *os_snap(void);                             /* ( ): the local variables and current dir ... */
void os_restore(void *snap);                     /* ... put back as they were */
long os_ticks(void);                             /* fiftieths of a second */
int os_pwd(char *buf, int size);                 /* the current dir's name */
void os_setrc(long rc);
char *os_readfile(const char *path, long *len);  /* os_alloc'ed, 0-ended */
int os_head(const char *path, char *buf, int n); /* a file's first n bytes; how many, -1 = none */
int os_tempfile(const char *text, int n, char *name, int size);   /* here-documents */
void os_delete(const char *name);
int os_cd(const char *dir);                      /* 0 = failed */
long os_number(void);                            /* $$: the CLI number */
void os_quit(long rc);                           /* exit at the prompt: csh ends */
int os_interactive(void);
long os_stackleft(void);         /* bytes of stack still free (no MMU: running off it corrupts memory) */
#define STACK_NEEDED 6000        /* a function call, $( ), source, an alias: refused below this */
void os_history(void);

/* pipes and jobs (job.c on the Amiga). Handles are BPTRs as longs. A
 * stage's in and out are handed to os_spawn, which closes them when it is
 * done, except the job's own (os_job_in / os_job_out). */
void *os_job_new(const char *text, int bg);
long os_job_in(void *job);               /* the first stage reads here */
long os_job_out(void *job);              /* the last stage writes here */
int os_pipe(void *job, long *readh, long *writeh);
void *os_prepare(struct cmdline *c);     /* the command found and loaded, not run; NULL = it cannot be (said) */
void os_unprepare(void *prep);           /* ... given back unused */
int os_spawn(void *job, struct cmdline *c, long in, long out, void *prep);   /* prep used up (NULL: found now); 0 = it could not start (said) */
int os_exists(const char *cmd);          /* a command of that name to run (not loaded) */
const char *os_self(void);               /* csh's own file, for a csh of its own */
long os_lastrun(void *job, struct cmdline *c, long in, void *prep);   /* the last Amiga command, run in csh */
void *os_select(long in, long out);      /* csh's own stage: these streams for a while */
void os_unselect(void *s);
long os_tempout(void *job);              /* csh's own stage in the middle writes a T: file ... */
long os_tempin(void *job);               /* ... the next stage reads it */
void os_close(void *job, long h);        /* closed unless it is the job's own */
void os_job_tmp(void *job, const char *name);   /* deleted when the job ends */
long os_job_end(void *job, long lastrc, int lastinternal, long *rcs, int *nrcs);   /* waits (Ctrl-C to each) or goes to the background; rcs: each stage's that ran apart */
long job_list(void);                     /* jobs */
long job_fg(const char *spec);           /* fg [%n] */
long job_kill(const char *spec);         /* kill [%n]: Ctrl-C to each stage */
long job_waitall(void);                  /* wait */

/* ---- lang.c ---- */
struct shfield { char *s; int len; char quoted, pattern; };

int lang_parse_check(const char *text);   /* 1 complete, 0 needs more, -1 error (PC tests) */
long lang_eval(const char *text);         /* runs text as csh (eval, -c, tests) */
int lang_line(const char *line, int interactive);   /* a line typed: 0 = more lines wanted */
void lang_reset(void);                    /* the unfinished command thrown away (Ctrl-C) */
void lang_end(void);                      /* csh ends: functions, $1.., the pending text freed */
extern int lang_more;
long lang_source(const char *path, int argc, char **argv);
void sh_print(const char *s);
void sh_printn(const char *s, int n);
void sh_error(const char *a, const char *b);     /* "csh: a b" */
long sh_rc(void);
int sh_getpos(int i, char **s);                  /* positional parameter i (1..) */
int sh_npos(void);
void sh_setpos(int argc, char **argv);           /* set's / shift's own */
int sh_local(const char *name, const char *val); /* local: -1 outside a function */
extern int sh_opt_e, sh_opt_x, sh_opt_u, sh_opt_f, sh_opt_pipefail;
int sh_get(const char *name, char *buf, int size);          /* a variable, arrays' [0] too; -1 = not set */
int sh_set(const char *name, const char *val, int how);     /* 0 = refused (read-only, said) */
int sh_unset(const char *name);                              /* name or name[key] */
int sh_unsetfunc(const char *name);
int sh_isarray(const char *name);                            /* 0 no, 1 indexed, 2 associative */
void sh_setarray(const char *name, int argc, char **argv);
void sh_readonly(const char *name);
long sh_trap(int argc, char **argv);
long sh_let(int argc, char **argv);
void sh_hook(int on);                                        /* printf -v: output into a string */
char *sh_hooked(void);                                       /* ... it, os_alloc'ed */
void lang_exittrap(void);                                    /* csh ends: the EXIT trap */
long lang_script(const char *path, int argc, char **argv, const char *text);
int bang_expand(const char *in, char *out, int size, const char *(*back)(int k));   /* bang.c: !! !$ ...; back(1) the last line */
char *lang_expandstr(const char *s);                         /* a string expanded as in "..." (the prompt), os_alloc'ed */
extern int sh_opt_histexp;                                   /* set -H / +H */
#define RE_GROUPS 10
int re_match(const char *re, const char *s, int nocase, int *caps, int *ngroups);   /* regex.c: 1, 0, -1 bad */
extern int sh_break, sh_continue, sh_return, sh_exiting;
extern long sh_exitrc;
extern int sh_loops;
int gmatch(const char *pat, const char *s, int nocase);   /* sh pattern: * ? [...] */
long arith(const char *expr, int *err);

/* ---- builtin.c ---- */
extern const char *const builtin_names[];
int builtin_find(const char *name);              /* -1 = not one */
long builtin_run(int which, int argc, char **argv);

#endif
