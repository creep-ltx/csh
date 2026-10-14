/* bang.c - history expansion at the prompt, as bash and zsh do it:
 *
 *   !!       the last line          !n    line n (as history numbers them)
 *   !$       its last word          !-n   n lines back
 *   !^       its first argument     !str  the last line starting with str
 *   !*       all its arguments      ^old^new   the last line, old made new
 *
 * Not in '...', not after \, $ or {, and not when ! is followed by a blank,
 * = or ( (so "! cmd", "[ a != b ]", "${!x}" stay as they are). Pure C:
 * tested on the PC (tests/lang_test.c). */
#include <string.h>
#include "lang.h"

/* the words of a line (quotes kept): word k's start and length */
static int wordat(const char *l, int k, int *len)
{
    int i = 0, w = 0;
    for (;;) {
        int s, q = 0;
        while (l[i] == ' ' || l[i] == '\t') i++;
        if (!l[i] || l[i] == '\n') return -1;
        s = i;
        while (l[i] && l[i] != '\n' && (q || (l[i] != ' ' && l[i] != '\t'))) {
            if (q) { if (l[i] == q) q = 0; }
            else if (l[i] == '\'' || l[i] == '"') q = l[i];
            else if (l[i] == '\\' && l[i + 1]) i++;
            i++;
        }
        if (w++ == k) { *len = i - s; return s; }
    }
}

static int nwords(const char *l)
{
    int n = 0, len;
    while (wordat(l, n, &len) >= 0) n++;
    return n;
}

static int put(char *out, int o, int size, const char *s, int n)
{
    if (o + n >= size) n = size - o - 1;
    if (n > 0) memcpy(out + o, s, n);
    return o + (n > 0 ? n : 0);
}

static int linelen(const char *l)
{
    int n = strlen(l);
    while (n && (l[n - 1] == '\n' || l[n - 1] == '\r')) n--;
    return n;
}

/* 1 = changed (out has it), 0 = nothing to do, -1 = an event not found (said) */
int bang_expand(const char *in, char *out, int size, const char *(*back)(int k))
{
    int i = 0, o = 0, changed = 0, q = 0;
    const char *last = back(1);
    if (in[0] == '^') {                 /* ^old^new^ */
        const char *a = in + 1, *b = strchr(a, '^'), *e, *p, *hit = NULL, *rest;
        int ol;
        if (!b || b == a || !last) return 0;
        ol = b - a;
        for (p = last; *p; p++) if (!strncmp(p, a, ol)) { hit = p; break; }
        if (!hit) {
            char t[64];
            int n = ol < 60 ? ol : 60;
            memcpy(t, a, n); t[n] = 0;
            sh_error("substitution failed:", t);
            return -1;
        }
        e = strchr(b + 1, '^');
        rest = e ? e + 1 : b + 1 + linelen(b + 1);
        o = put(out, o, size, last, hit - last);
        o = put(out, o, size, b + 1, (e ? e : rest) - b - 1);
        o = put(out, o, size, hit + ol, linelen(hit + ol));
        o = put(out, o, size, rest, strlen(rest));
        out[o] = 0;
        return 1;
    }
    while (in[i]) {
        char c = in[i], d = in[i + 1];
        if (q) { if (c == '\'') q = 0; o = put(out, o, size, in + i, 1); i++; continue; }
        if (c == '\'') { q = 1; o = put(out, o, size, in + i, 1); i++; continue; }
        if (c == '\\' && d) { o = put(out, o, size, in + i, 2); i += 2; continue; }
        if (c != '!' || !d || d == ' ' || d == '\t' || d == '\n' || d == '=' || d == '(' || d == '"' ||
            (i && (in[i - 1] == '$' || in[i - 1] == '{'))) {
            o = put(out, o, size, in + i, 1); i++;
            continue;
        }
        {
            const char *line = NULL;
            int j = i + 1, k, len, n;
            if (d == '!' || d == '$' || d == '^' || d == '*') { line = last; j = i + 2; }
            else if ((d >= '0' && d <= '9') || d == '-') {
                int neg = d == '-', num = 0;
                if (neg) j++;
                while (in[j] >= '0' && in[j] <= '9') num = num * 10 + in[j++] - '0';
                if (neg) line = num ? back(num) : NULL;
                else {
                    int total = 0;
                    while (back(total + 1)) total++;
                    line = num >= 1 && num <= total ? back(total - num + 1) : NULL;
                }
            } else {
                int s = j;
                while (in[j] && in[j] != ' ' && in[j] != '\t' && in[j] != '\n' && in[j] != ';' && in[j] != '|' && in[j] != '&' && in[j] != ':') j++;
                for (k = 1; (line = back(k)); k++) if (!strncmp(line, in + s, j - s)) break;
            }
            if (!line) {
                char t[64];
                n = j - i < 60 ? j - i : 60;
                memcpy(t, in + i, n); t[n] = 0;
                sh_error("event not found:", t);
                return -1;
            }
            if (d == '$') {
                if ((n = nwords(line)) && (k = wordat(line, n - 1, &len)) >= 0) o = put(out, o, size, line + k, len);
            } else if (d == '^') {
                if ((k = wordat(line, 1, &len)) >= 0) o = put(out, o, size, line + k, len);
            } else if (d == '*') {
                if ((k = wordat(line, 1, &len)) >= 0) o = put(out, o, size, line + k, linelen(line + k));
            } else o = put(out, o, size, line, linelen(line));
            changed = 1;
            i = j;
        }
    }
    out[o] = 0;
    return changed;
}
