/* line.h - the ROM Shell's rules for a command line (line.c): aliases,
 * $variables, backquotes, and splitting into command word, arguments and
 * redirections. Pure C, so tests/line_test.c runs it on the PC. */
#ifndef CSH_LINE_H
#define CSH_LINE_H

#define LINE_MAX 1024

/* Looks a name up; returns its length, or -1 when there is none. */
typedef int (*line_get_fn)(const char *name, char *buf, int size);
/* Runs a command and gives its output; returns the length or -1. */
typedef int (*line_run_fn)(const char *cmd, char *buf, int size);

struct cmdline {
    char cmd[256];            /* the command word, quotes taken off */
    char args[LINE_MAX];      /* the rest, redirections taken out, ends in "\n" */
    int arglen;               /* strlen(args), the "\n" counted */
    char in[256], out[256], err[256];   /* redirection files, "" = none */
    int out_append, err_append, inout;  /* >>, *>>, <> (in and out one file) */
    int errtoout;             /* 2>&1, &>: the error stream goes where the output goes */
    int pipe;                 /* an unquoted "|" word: a pipe */
};

int line_alias(const char *line, char *out, int size, line_get_fn alias);
int line_vars(const char *in, char *out, int size, line_get_fn var);
int line_backquotes(const char *in, char *out, int size, line_run_fn run);
int line_parse(const char *line, struct cmdline *c);

#endif
