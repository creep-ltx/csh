/* fakeos - the os_ calls of src/lang.h on the PC, for tests/lang_test.c:
 * variables in memory, output into a buffer, an Amiga command shown as
 * "[Name|args]" (what csh would hand to run.c) instead of run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../src/lang.h"

#define MAXV 200
static struct { char *name, *val; int alias, global; } vars[MAXV];
static int nvars, nexp;
static char out[65536];
static int outn;
static int capstack[16], ncap;
static const char *input;          /* what read and os_readline give */
long fake_rcs[256];                /* the rc of [Name] commands: by first letter */
int fake_quit = -1;

void fake_reset(const char *in)
{
    int i;
    extern int fake_h_reset(void);
    fake_h_reset();
    for (i = 0; i < nvars; i++) { free(vars[i].name); free(vars[i].val); }
    nvars = 0; outn = 0; out[0] = 0; ncap = 0; input = in ? in : ""; nexp = 0;
    memset(fake_rcs, 0, sizeof fake_rcs);
    fake_quit = -1;
}

const char *fake_out(void) { out[outn] = 0; return out; }

/* alias 1: an alias; else global 0 a local variable, 1 one in ENV: (two
 * stores, as on the Amiga: the local one is seen first) */
static int findg(const char *n, int alias, int global)
{
    int i;
    for (i = 0; i < nvars; i++)
        if (vars[i].alias == alias && (alias || vars[i].global == global) && !strcasecmp(vars[i].name, n)) return i;
    return -1;
}

static int find(const char *n, int alias)
{
    int i = findg(n, alias, 0);
    return i >= 0 || alias ? i : findg(n, 0, 1);
}

static void del(int i)
{
    free(vars[i].name); free(vars[i].val);
    vars[i] = vars[--nvars];
}

void *os_alloc(long n) { return calloc(1, n); }
void os_free(void *p) { free(p); }

int os_getvar(const char *name, char *buf, int size)
{
    int i = find(name, 0);
    if (i < 0) return -1;
    snprintf(buf, size, "%s", vars[i].val);
    return strlen(buf);
}

static void set(const char *name, const char *val, int alias, int global)
{
    int i = findg(name, alias, global);
    if (i < 0) { if (nvars == MAXV) return; i = nvars++; vars[i].name = strdup(name); vars[i].val = NULL; vars[i].alias = alias; }
    free(vars[i].val);
    vars[i].val = strdup(val);
    vars[i].global = global;
}

/* as os.c: export writes ENV: and drops the local copy that would hide it;
 * x=1 of an exported name writes ENV: too, unset deletes it there */
static char exported[16][64];
static int isexp(const char *n) { int i; for (i = 0; i < nexp; i++) if (!strcasecmp(exported[i], n)) return i; return -1; }
void os_setvar(const char *name, const char *val, int how)
{
    int i;
    if (how == V_EXPORT || (how == V_ASSIGN && isexp(name) >= 0)) {
        set(name, val, 0, 1);
        if ((i = findg(name, 0, 0)) >= 0) del(i);
        if (how == V_EXPORT && isexp(name) < 0 && nexp < 16) snprintf(exported[nexp++], 64, "%s", name);
        return;
    }
    set(name, val, 0, 0);
}
void os_end(void) { }
int os_getlocal(const char *name, char *buf, int size)
{
    int i = findg(name, 0, 0);
    if (i < 0) return -1;
    snprintf(buf, size, "%s", vars[i].val);
    return strlen(buf);
}
void os_unsetvar(const char *name, int env)
{
    int i = findg(name, 0, 0), k;
    if (i >= 0) del(i);
    if (env && (k = isexp(name)) >= 0) {
        if ((i = findg(name, 0, 1)) >= 0) del(i);
        memmove(exported[k], exported[k + 1], (nexp - k - 1) * sizeof exported[0]);
        nexp--;
    }
}
int os_getalias(const char *name, char *buf, int size)
{
    int i = find(name, 1);
    if (i < 0) return -1;
    snprintf(buf, size, "%s", vars[i].val);
    return strlen(buf);
}
void os_setalias(const char *name, const char *val) { set(name, val, 1, 0); }

void os_write(const char *s, int n)
{
    if (outn + n >= (int)sizeof out) n = sizeof out - outn - 1;
    memcpy(out + outn, s, n);
    outn += n;
}

long os_run(struct cmdline *c)
{
    char b[2048];
    int n = c->arglen;
    while (n && c->args[n - 1] == '\n') n--;
    snprintf(b, sizeof b, "[%s|%.*s%s%s%s%s%s%s%s]\n", c->cmd, n, c->args,
             c->in[0] ? " <" : "", c->in, c->out[0] ? (c->out_append ? " >>" : " >") : "", c->out,
             c->err[0] ? " *>" : "", c->err, c->errtoout ? " 2>&1" : "");
    os_write(b, strlen(b));
    return fake_rcs[(unsigned char)c->cmd[0]];
}


/* read -n -N -t -s from the fake input: no input with a time limit is a
 * time out */
int os_readkeys(char *buf, int size, int n, int exact, long ticks, int silent)
{
    int got = 0;
    (void)silent;
    if (!*input) return ticks >= 0 ? -2 : -1;
    while (*input && got < size - 1 && (!n || got < n)) {
        char c = *input++;
        if (c == '\n' && !exact) { if (!n) buf[got++] = c; break; }
        buf[got++] = c;
    }
    buf[got] = 0;
    return got;
}

/* a name with a capital is an Amiga command in C: */
int os_which(const char *name, char *buf, int size)
{
    if (name[0] < 'A' || name[0] > 'Z') return 0;
    snprintf(buf, size, "C:%s", name);
    return 1;
}

int os_readline(char *buf, int size)
{
    const char *nl;
    int n;
    if (!*input) return -1;
    nl = strchr(input, '\n');
    n = nl ? nl - input + 1 : (int)strlen(input);
    if (n >= size) n = size - 1;
    memcpy(buf, input, n); buf[n] = 0;
    input += n;
    return n;
}

void *os_redir(struct cmdline *c)
{
    char b[900];
    /* a drawer that is not there: the open fails (said, as PrintFault does) */
    if (!strncmp(c->in, "NOSUCH:", 7) || !strncmp(c->out, "NOSUCH:", 7) || !strncmp(c->err, "NOSUCH:", 7)) {
        os_write("{redir failed}", 14);
        return NULL;
    }
    snprintf(b, sizeof b, "{redir%s%s%s%s%s%s}", c->in[0] ? " <" : "", c->in, c->out[0] ? " >" : "", c->out, c->err[0] ? " *>" : "", c->err);
    os_write(b, strlen(b));
    return (void *)1;
}
void os_flush(void) { }
/* the console: not into a $( ) (dropped there), else shown in place */
void os_tty(const char *s, int n) { if (!ncap) os_write(s, n); }
void os_err(const char *s, int n) { if (!ncap) os_write(s, n); }
void os_unredir(void *s) { (void)s; os_write("{/redir}", 8); }

void *os_capture(void)
{
    if (ncap == 16) return NULL;
    capstack[ncap++] = outn;
    return (void *)1;
}

int os_captured(void *s, char **text)
{
    int start = capstack[--ncap], n = outn - start;
    (void)s;
    *text = malloc(n + 1);
    memcpy(*text, out + start, n); (*text)[n] = 0;
    outn = start;
    return n;
}

int os_break(void) { return 0; }
long os_stackleft(void) { return 1L << 20; }

int os_test(const char *path, int op)
{
    /* the fake disk: files a.txt b.txt, drawer dir */
    int isfile = !strcmp(path, "a.txt") || !strcmp(path, "b.txt"), isdir = !strcmp(path, "dir");
    if (op == 'f') return isfile;
    if (op == 'd') return isdir;
    return isfile || isdir;
}

int os_glob(const char *pattern, void (*fn)(const char *name, void *u), void *u)
{
    static const char *files[] = { "b.txt", "a.txt", "c.info", "dir", NULL };
    int i, n = 0;
    for (i = 0; files[i]; i++) if (gmatch(pattern, files[i], 1)) { fn(files[i], u); n++; }
    return n;
}

void os_setrc(long rc) { char b[24]; snprintf(b, sizeof b, "%ld", rc); set("RC", b, 0, 0); }

/* Dir and CD take one name (their templates have no /M) */
int os_globok(const char *cmd) { return strcasecmp(cmd, "Dir") && strcasecmp(cmd, "CD"); }
long os_mtime(const char *path) { return !strcmp(path, "a.txt") ? 100 : !strcmp(path, "b.txt") ? 200 : -1; }

/* ( ): the variables (not the exported ones in ENV:) put back */
struct fsnap { int n; struct { char *name, *val; int alias, global; } v[MAXV]; };
void *os_snap(void)
{
    struct fsnap *s = calloc(1, sizeof *s);
    int i;
    for (i = 0; i < nvars; i++) if (!vars[i].global) {
        s->v[s->n].name = strdup(vars[i].name); s->v[s->n].val = strdup(vars[i].val);
        s->v[s->n].alias = vars[i].alias; s->v[s->n].global = 0; s->n++;
    }
    return s;
}
void os_restore(void *p)
{
    struct fsnap *s = p;
    int i;
    for (i = 0; i < nvars; ) { if (!vars[i].global) del(i); else i++; }
    for (i = 0; i < s->n; i++) {
        set(s->v[i].name, s->v[i].val, s->v[i].alias, 0);
        free(s->v[i].name); free(s->v[i].val);
    }
    free(s);
}

int os_head(const char *path, char *buf, int n)
{
    FILE *f = fopen(path, "rb");
    int r;
    if (!f) return -1;
    r = fread(buf, 1, n, f);
    fclose(f);
    return r;
}

char *os_readfile(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *t;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    t = calloc(1, n + 1);
    if (fread(t, 1, n, f) != (size_t)n) { fclose(f); free(t); return NULL; }
    fclose(f);
    *len = n;
    return t;
}

static int hdk;
int os_tempfile(const char *text, int n, char *name, int size)
{
    int k = hdk;
    char b[600];
    snprintf(name, size, "T:hd%d", ++k);
    hdk = k;
    snprintf(b, sizeof b, "{heredoc %s:%.*s}", name, n, text);
    os_write(b, strlen(b));
    return 1;
}
void os_delete(const char *name) { (void)name; }
int os_cd(const char *d) { (void)d; return 1; }
long os_number(void) { return 7; }
void os_quit(long rc) { fake_quit = rc; }
int os_interactive(void) { return 0; }
void os_history(void) { os_write("[history]\n", 10); }
long os_ticks(void) { return 0; }
int os_pwd(char *b, int n) { snprintf(b, n, "Work:"); return 1; }

/* pipes and jobs: shown, not run. Handles: 1 the input, 2 the output,
 * pipes 10, 11, ... (read, write), temp files 50.. */
static int fakeh = 10;
void *os_job_new(const char *text, int bg) { char b[300]; snprintf(b, sizeof b, "{job%s %s}", bg ? " &" : "", text); os_write(b, strlen(b)); return (void *)1; }
long os_job_in(void *j) { (void)j; return 1; }
long os_job_out(void *j) { (void)j; return 2; }
const char *os_self(void) { return "csh"; }
int os_exists(const char *cmd) { return strncmp(cmd, "NoSuch", 6) != 0; }
int os_pipe(void *j, long *r, long *w) { (void)j; *r = fakeh++; *w = fakeh++; return 1; }
void *os_prepare(struct cmdline *c)
{
    if (!strncmp(c->cmd, "NoSuch", 6)) { os_write(c->cmd, strlen(c->cmd)); os_write(": Unknown command\n", 18); return NULL; }
    return (void *)1;
}
void os_unprepare(void *p) { (void)p; }
int os_spawn(void *j, struct cmdline *c, long in, long out, void *prep)
{
    (void)prep;
    char b[1200];
    int n = c->arglen;
    (void)j;
    while (n && c->args[n - 1] == '\n') n--;
    snprintf(b, sizeof b, "[spawn %s|%.*s in=%ld out=%ld%s%s%s%s%s]", c->cmd, n, c->args, in, out,
             c->in[0] ? " <" : "", c->in, c->out[0] ? " >" : "", c->out, c->errtoout ? " 2>&1" : "");
    os_write(b, strlen(b));
    return 1;
}
void *os_select(long in, long out) { char b[64]; snprintf(b, sizeof b, "{sel in=%ld out=%ld}", in, out); os_write(b, strlen(b)); return (void *)1; }
void os_unselect(void *s) { (void)s; os_write("{/sel}", 6); }
static long lasttemp = 50;
long os_tempout(void *j) { (void)j; return ++lasttemp; }
long os_tempin(void *j) { (void)j; return lasttemp + 100; }
void os_close(void *j, long h) { char b[32]; (void)j; if (h > 2) { snprintf(b, sizeof b, "{close %ld}", h); os_write(b, strlen(b)); } }
void os_job_tmp(void *j, const char *name) { (void)j; (void)name; }
long os_job_end(void *j, long rc, int li, long *rcs, int *nrcs) { char b[64]; (void)j; (void)rcs; *nrcs = 0; snprintf(b, sizeof b, "{end rc=%ld%s}\n", rc, li ? " own" : ""); os_write(b, strlen(b)); return rc; }
long job_list(void) { os_write("[jobs]\n", 7); return 0; }
long job_fg(const char *s) { (void)s; os_write("[fg]\n", 5); return 0; }
long job_kill(const char *s) { (void)s; os_write("[kill]\n", 7); return 0; }
long job_waitall(void) { os_write("[wait]\n", 7); return 0; }
long job_wait(const char *s) { char b[64]; snprintf(b, sizeof b, "[wait %s]\n", s); os_write(b, strlen(b)); return 0; }
long job_waitany(void) { os_write("[wait -n]\n", 10); return 0; }
long job_disown(const char *s) { char b[64]; snprintf(b, sizeof b, "[disown %s]\n", s ? s : ""); os_write(b, strlen(b)); return 0; }
int job_lastbg(void) { return 2; }
int fake_h_reset(void) { fakeh = 10; lasttemp = 50; hdk = 0; return 0; }
long os_lastrun(void *j, struct cmdline *c, long in, void *prep)
{
    (void)prep;
    char b[1200];
    int n = c->arglen;
    (void)j;
    while (n && c->args[n - 1] == '\n') n--;
    snprintf(b, sizeof b, "[last %s|%.*s in=%ld%s%s]", c->cmd, n, c->args, in, c->out[0] ? " >" : "", c->out);
    os_write(b, strlen(b));
    return fake_rcs[(unsigned char)c->cmd[0]];
}
