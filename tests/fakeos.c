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
static int nvars;
static char out[65536];
static int outn;
static int capstack[16], ncap;
static const char *input;          /* what read and os_readline give */
long fake_rcs[256];                /* the rc of [Name] commands: by first letter */
int fake_quit = -1;

void fake_reset(const char *in)
{
    int i;
    for (i = 0; i < nvars; i++) { free(vars[i].name); free(vars[i].val); }
    nvars = 0; outn = 0; out[0] = 0; ncap = 0; input = in ? in : "";
    memset(fake_rcs, 0, sizeof fake_rcs);
    fake_quit = -1;
}

const char *fake_out(void) { out[outn] = 0; return out; }

static int find(const char *n, int alias)
{
    int i;
    for (i = 0; i < nvars; i++) if (vars[i].alias == alias && !strcasecmp(vars[i].name, n)) return i;
    return -1;
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
    int i = find(name, alias);
    if (i < 0) { if (nvars == MAXV) return; i = nvars++; vars[i].name = strdup(name); vars[i].val = NULL; vars[i].alias = alias; }
    free(vars[i].val);
    vars[i].val = strdup(val);
    vars[i].global = global;
}

void os_setvar(const char *name, const char *val, int global) { set(name, val, 0, global); }
void os_unsetvar(const char *name)
{
    int i = find(name, 0);
    if (i < 0) return;
    free(vars[i].name); free(vars[i].val);
    vars[i] = vars[--nvars];
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

long os_rom(const char *line, int bg)
{
    char b[2048];
    snprintf(b, sizeof b, "[ROM%s|%s]\n", bg ? " &" : "", line);
    os_write(b, strlen(b));
    return 0;
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
    snprintf(b, sizeof b, "{redir%s%s%s%s%s%s}", c->in[0] ? " <" : "", c->in, c->out[0] ? " >" : "", c->out, c->err[0] ? " *>" : "", c->err);
    os_write(b, strlen(b));
    return (void *)1;
}
void os_flush(void) { }
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

void os_setrc(long rc) { char b[16]; snprintf(b, sizeof b, "%ld", rc); set("RC", b, 0, 0); }

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

int os_tempfile(const char *text, int n, char *name, int size)
{
    static int k;
    char b[600];
    snprintf(name, size, "T:hd%d", ++k);
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
