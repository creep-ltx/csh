/* line.c - the ROM Shell's rules for a command line, as tested on the 3.2
 * ROM (Knowledge/amiga/writing-a-shell.md, section D):
 *  - an alias is the first word only; "[]" in it takes the rest of the line,
 *    else the rest is appended;
 *  - $name and ${name} are replaced everywhere, inside quotes too; an
 *    unknown one stays as written; *$ stops it; names are letters and
 *    digits, anything else needs ${...};
 *  - variables are replaced before redirections and ";" are looked at;
 *  - `cmd` is the command's output, its line ends made spaces;
 *  - ";" outside quotes starts a comment; in quotes * escapes the next
 *    character, so *" does not end the quote;
 *  - >file >>file <file <>file *>file *>>file anywhere in the line, the
 *    name joined or after a space; a "|" word on its own is a pipe;
 *  - a line joined with "+" holds LFs: the text after the first one goes
 *    to the command as the start of its input.
 * No Amiga calls in here: tests/line_test.c runs it on the PC. */
#include <string.h>
#include "line.h"

static int isname(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int isspc(char c) { return c == ' ' || c == '\t'; }

/* Appends n bytes to out; returns 0 when it would not fit. */
static int put(char *out, int *o, int size, const char *s, int n)
{
    if (*o + n >= size) return 0;
    memcpy(out + *o, s, n); *o += n; out[*o] = 0;
    return 1;
}

int line_alias(const char *line, char *out, int size, line_get_fn alias)
{
    char word[64], text[LINE_MAX];
    const char *p = line, *rest, *br;
    int n = 0, o = 0;
    while (isspc(*p)) p++;
    if (*p == '"') return 0;            /* a quoted command word is never an alias */
    while (*p && !isspc(*p) && *p != ';' && *p != '\n' && n < (int)sizeof word - 1) word[n++] = *p++;
    word[n] = 0;
    if (!n || (*p && !isspc(*p) && *p != ';' && *p != '\n')) return 0;
    if (alias(word, text, sizeof text) < 0) return 0;
    rest = p;
    while (isspc(*rest)) rest++;
    out[0] = 0;
    if ((br = strstr(text, "[]"))) {
        int rl = strlen(rest);
        while (rl && rest[rl - 1] == '\n') rl--;
        if (!put(out, &o, size, text, br - text) || !put(out, &o, size, rest, rl)
            || !put(out, &o, size, br + 2, strlen(br + 2))) return -1;
    } else {
        if (!put(out, &o, size, text, strlen(text))) return -1;
        if (*rest && *rest != '\n' && !put(out, &o, size, " ", 1)) return -1;
        if (!put(out, &o, size, rest, strlen(rest))) return -1;
    }
    return 1;
}

int line_vars(const char *in, char *out, int size, line_get_fn var)
{
    char name[64], val[LINE_MAX];
    int o = 0, q = 0;
    out[0] = 0;
    while (*in) {
        char c = *in;
        if (c == '"') { q = !q; if (!put(out, &o, size, in++, 1)) return -1; continue; }
        if (c == '*' && q && in[1] && in[1] != '$') {   /* *x in quotes: kept as a pair */
            if (!put(out, &o, size, in, 2)) return -1;
            in += 2; continue;
        }
        if (c == '*' && in[1] == '$') {    /* *$ stops the variable */
            if (q && !put(out, &o, size, "*", 1)) return -1;
            if (!put(out, &o, size, "$", 1)) return -1;
            in += 2; continue;
        }
        if (c == '$') {
            const char *p = in + 1, *e;
            int n = 0, l;
            if (*p == '{') {
                e = strchr(p + 1, '}');
                if (e && e - p - 1 < (int)sizeof name && e > p + 1) {
                    n = e - p - 1; memcpy(name, p + 1, n); e++;
                }
            } else {
                while (isname(p[n]) && n < (int)sizeof name - 1) n++;
                memcpy(name, p, n); e = p + n;
            }
            if (n) {
                name[n] = 0;
                if ((l = var(name, val, sizeof val)) >= 0) {
                    if (!put(out, &o, size, val, l)) return -1;
                    in = e; continue;
                }
                if (!put(out, &o, size, in, e - in)) return -1;   /* unknown: as written */
                in = e; continue;
            }
        }
        if (!put(out, &o, size, in++, 1)) return -1;
    }
    return o;
}

int line_backquotes(const char *in, char *out, int size, line_run_fn run)
{
    char cmd[LINE_MAX], res[LINE_MAX];
    int o = 0, q = 0;
    out[0] = 0;
    while (*in) {
        char c = *in;
        if (c == '"') q = !q;
        if (c == '*' && in[1] == '`') {          /* *` : a backquote as itself */
            if (q && !put(out, &o, size, "*", 1)) return -1;
            if (!put(out, &o, size, "`", 1)) return -1;
            in += 2; continue;
        }
        if (c == '*' && q && in[1]) {
            if (!put(out, &o, size, in, 2)) return -1;
            in += 2; continue;
        }
        if (c == '`') {
            const char *e = strchr(in + 1, '`');
            int n, l, i;
            if (!e) return -1;
            n = e - in - 1;
            if (n >= (int)sizeof cmd) return -1;
            memcpy(cmd, in + 1, n); cmd[n] = 0;
            l = run(cmd, res, sizeof res);
            if (l < 0) l = 0;
            while (l && (res[l - 1] == '\n' || res[l - 1] == ' ')) l--;
            for (i = 0; i < l; i++) if (res[i] == '\n') res[i] = ' ';
            if (!put(out, &o, size, res, l)) return -1;
            in = e + 1; continue;
        }
        if (!put(out, &o, size, in++, 1)) return -1;
    }
    return o;
}

/* The end of the word at p: a space or ";" outside quotes ends it. */
static const char *wordend(const char *p)
{
    int q = 0;
    while (*p && *p != '\n') {
        if (*p == '"') q = !q;
        else if (q && *p == '*' && p[1] && p[1] != '\n') p++;
        else if (!q && (isspc(*p) || *p == ';')) break;
        p++;
    }
    return p;
}

/* Copies a word into dst, the quotes around it (or in it) taken off. */
static void unquote(char *dst, int size, const char *s, int n)
{
    int o = 0, q = 0, i;
    for (i = 0; i < n && o < size - 1; i++) {
        if (s[i] == '"') { q = !q; continue; }
        if (q && s[i] == '*' && i + 1 < n) {
            char c = s[++i];
            dst[o++] = c == 'N' || c == 'n' ? '\n' : c == 'E' || c == 'e' ? 27 : c;
            continue;
        }
        dst[o++] = s[i];
    }
    dst[o] = 0;
}

int line_parse(const char *line, struct cmdline *c)
{
    const char *p = line, *gap = line;
    int o = 0, havecmd = 0;
    memset(c, 0, sizeof *c);
    for (;;) {
        const char *w, *e, *f;
        char *dst = NULL;
        int skip = 0;
        while (isspc(*p)) p++;
        if (!*p || *p == '\n' || *p == ';') break;
        w = p; e = wordend(p);
        if (*w == '>' || *w == '<' || (*w == '*' && w[1] == '>')) {
            f = w;
            if (*f == '*') { f += 2; dst = c->err; if (*f == '>') { f++; c->err_append = 1; } }
            else if (*f == '<') {
                f++; dst = c->in;
                if (*f == '>') { f++; c->inout = 1; }
            } else { f++; dst = c->out; if (*f == '>') { f++; c->out_append = 1; } }
            if (f >= e) {                    /* "> file": the name is the next word */
                const char *n = e;
                while (isspc(*n)) n++;
                if (*n && *n != '\n' && *n != ';') { f = n; e = wordend(n); }
            }
            unquote(dst, 256, f, e - f);
            if (c->inout) strcpy(c->out, c->in);
            skip = 1;
        } else if (e - w == 1 && *w == '|') {
            c->pipe = 1;
        }
        if (!skip) {
            if (!havecmd) {
                unquote(c->cmd, sizeof c->cmd, w, e - w);
                havecmd = 1;
            } else {
                const char *g = o ? gap : w;   /* keep the spaces between arguments */
                if (o + (e - g) + 2 >= (int)sizeof c->args) return -1;
                memcpy(c->args + o, g, e - g); o += e - g;
            }
        }
        gap = e;
        p = e;
    }
    /* lines joined with "+" (main.c): what follows the first LF is the
     * command's own input, passed on as it is */
    if (*p == ';') p += strcspn(p, "\n");
    if (*p == '\n' && p[1]) {
        int n = strlen(p);
        while (n > 1 && p[n - 1] == '\n') n--;
        if (o + n + 2 >= (int)sizeof c->args) return -1;
        memcpy(c->args + o, p, n); o += n;
    }
    c->args[o++] = '\n'; c->args[o] = 0;
    c->arglen = o;
    return c->cmd[0] ? 1 : 0;
}
