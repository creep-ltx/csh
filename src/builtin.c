/* builtin.c - csh's own commands (docs/design.md: built-ins for what
 * AmigaDOS lacks in scripts). Programs like ls, cp, wc stay programs.
 *
 * Commands are any case on the Amiga, so where a name is also an Amiga
 * command the Amiga one keeps its place: csh's eval only in lower case
 * (C:Eval does sums), break only inside a loop (C:Break signals a CLI),
 * and echo with NOLINE/FIRST/LEN, alias without '=', set with nothing
 * after it are handed to the ROM's own (a builtin returns -999 for that).
 * False is SH_FALSE; a usage error 10, the Amiga's ERROR. */
#include <string.h>
#include "lang.h"

#define NOTMINE -999

enum { B_EXIT, B_RETURN, B_BREAK, B_CONTINUE, B_SHIFT, B_TRUE, B_FALSE, B_COLON, B_ECHO, B_PRINTF,
       B_TEST, B_BRACKET, B_READ, B_SOURCE, B_DOT, B_EVAL, B_EXPORT, B_UNSET, B_SET, B_ALIAS,
       B_HISTORY, B_TIME, B_PWD, B_JOBS, B_FG, B_KILL, B_WAIT, B_LOCAL, B_CD, B_PUSHD, B_POPD, B_DIRS,
       B_TRAP, B_GETOPTS, B_LET, B_MAPFILE, B_READARRAY, B_DECLARE, B_TYPESET, B_READONLY, B_COUNT };

const char *const builtin_names[] = {
    "exit", "return", "break", "continue", "shift", "true", "false", ":", "echo", "printf",
    "test", "[", "read", "source", ".", "eval", "export", "unset", "set", "alias",
    "history", "time", "pwd", "jobs", "fg", "kill", "wait", "local", "cd", "pushd", "popd", "dirs",
    "trap", "getopts", "let", "mapfile", "readarray", "declare", "typeset", "readonly", NULL
};
/* lower case only: an Amiga command of that name in any other case (CD is the ROM's) */
static const char exact[B_COUNT] = { [B_EVAL] = 1, [B_BREAK] = 1, [B_TIME] = 1, [B_WAIT] = 1, [B_CD] = 1 };

int os_pwd(char *buf, int size);
int os_cd(const char *dir);

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ltoa_b(long n, char *b)
{
    char t[24];
    int i = 0, o = 0;
    unsigned long u = n < 0 ? -(unsigned long)n : (unsigned long)n;
    if (n < 0) b[o++] = '-';
    do { t[i++] = '0' + u % 10; u /= 10; } while (u);
    while (i) b[o++] = t[--i];
    b[o] = 0;
    return o;
}

static int ieq(const char *a, const char *b)
{
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

int builtin_find(const char *name)
{
    int i;
    for (i = 0; builtin_names[i]; i++)
        if (exact[i] ? !strcmp(name, builtin_names[i]) : ieq(name, builtin_names[i])) return i;
    return -1;
}

static int num(const char *s, long *v)
{
    long n = 0;
    int neg = 0, any = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        if (n > 214748363L) return 0;          /* past a 32-bit number */
        n = n * 10 + *s++ - '0'; any = 1;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (!any || *s) return 0;
    *v = neg ? -n : n;
    return 1;
}

/* One escape, s just after the backslash: its character in *c and how many
 * characters it took; 0 = not an escape (the \ stays), -1 = \c (stop).
 * octal0: echo -e's and %b's \0nnn; else printf's own \nnn. */
static int esc1(const char *s, char *c, int octal0)
{
    int k, v = 0;
    switch (*s) {
    case 'n': *c = '\n'; return 1;
    case 't': *c = '\t'; return 1;
    case 'r': *c = '\r'; return 1;
    case 'a': *c = 7; return 1;
    case 'b': *c = 8; return 1;
    case 'f': *c = 12; return 1;
    case 'v': *c = 11; return 1;
    case 'e': case 'E': *c = 27; return 1;
    case '\\': *c = '\\'; return 1;
    case 'c': return -1;
    }
    if (octal0 ? *s == '0' : (*s >= '0' && *s <= '7')) {
        const char *p = octal0 ? s + 1 : s;
        for (k = 0; k < 3 && p[k] >= '0' && p[k] <= '7'; k++) v = v * 8 + p[k] - '0';
        *c = v;
        return (p - s) + k;
    }
    if (*s == 'x') {
        for (k = 1; k < 3; k++) {
            int d = s[k] >= '0' && s[k] <= '9' ? s[k] - '0' : (s[k] | 32) >= 'a' && (s[k] | 32) <= 'f' ? (s[k] | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
        }
        if (k == 1) return 0;
        *c = v;
        return k;
    }
    return 0;
}

/* A string with escapes, printed; 1 when a \c stopped it. */
static int escapes(const char *s, int octal0)
{
    char b[256];
    int o = 0, k;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            if ((k = esc1(s + 1, &c, octal0)) < 0) { if (o) sh_printn(b, o); return 1; }
            if (k) s += k; else c = '\\';
        }
        b[o++] = c;
        if (o == (int)sizeof b) { sh_printn(b, o); o = 0; }
    }
    if (o) sh_printn(b, o);
    return 0;
}

/* echo with NOLINE, FIRST n, LEN n or TO file is the Amiga's Echo: its
 * keywords where its template would take them (NOLINE any case; TO in
 * capitals, so "echo I want to go" stays csh's). */
static int amigaecho(int argc, char **argv)
{
    int k;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) return 0;     /* echo -n ...: bash's */
    for (k = 1; k < argc; k++) {
        const char *a = argv[k];
        long v;
        if (ieq(a, "NOLINE")) return 1;
        if ((ieq(a, "FIRST") || ieq(a, "LEN")) && k + 1 < argc && num(argv[k + 1], &v)) return 1;
        if (!strcmp(a, "TO") && k + 1 < argc) return 1;
        if (!strncmp(a, "TO=", 3) || !strncmp(a, "FIRST=", 6) || !strncmp(a, "LEN=", 4)) return 1;
    }
    return 0;
}

static long b_echo(int argc, char **argv)
{
    int i = 1, nl = 1, esc = 0;
    if (amigaecho(argc, argv)) return NOTMINE;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *o = argv[i] + 1;
        int ok = 1, n2 = nl, e2 = esc;
        for (; *o; o++) { if (*o == 'n') n2 = 0; else if (*o == 'e') e2 = 1; else if (*o == 'E') e2 = 0; else ok = 0; }
        if (!ok) break;
        nl = n2; esc = e2; i++;
    }
    for (; i < argc; i++) {
        if (esc) { if (escapes(argv[i], 1)) return 0; }
        else sh_print(argv[i]);
        if (i + 1 < argc) sh_print(" ");
    }
    if (nl) sh_print("\n");
    return 0;
}

static void pad(int n, int zero) { while (n-- > 0) sh_print(zero ? "0" : " "); }

/* ---- printf %f %e %g: worked on the decimal digits (no floating point
 * code linked in: the number is read and rounded as text) ---- */

typedef struct { char d[48]; int nd, x, neg; } Dec;    /* value = 0.d * 10^x */

static int decread(const char *s, Dec *v)
{
    int any = 0, x = 0, seen = 0, ex = 0, eneg = 0;
    memset(v, 0, sizeof *v);
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { v->neg = 1; s++; } else if (*s == '+') s++;
    for (; (*s >= '0' && *s <= '9') || (*s == '.' && !seen); s++) {
        if (*s == '.') { seen = 1; continue; }
        any = 1;
        if (*s == '0' && !v->nd) { if (seen) x--; continue; }          /* leading zeros */
        if (v->nd < (int)sizeof v->d - 1) v->d[v->nd++] = *s;
        if (!seen) x++;
    }
    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') { if (ex < 10000) ex = ex * 10 + *s - '0'; s++; }
        x += eneg ? -ex : ex;
    }
    while (*s == ' ' || *s == '\t') s++;
    v->x = v->nd ? x : 0;
    return any && !*s;
}

/* rounded (half up) to keep digits */
static void decround(Dec *v, int keep)
{
    int i;
    if (v->nd <= keep) return;
    if (keep < 0) { v->nd = 0; return; }
    /* half to even on an exact tie (as C's printf: 2.5 is 2, 0.125 is 0.12) */
    for (i = keep + 1; i < v->nd && v->d[i] == '0'; i++) ;
    if (v->d[keep] > '5' || (v->d[keep] == '5' && (i < v->nd || (keep > 0 && (v->d[keep - 1] - '0') & 1)))) {
        for (i = keep - 1; i >= 0; i--) { if (v->d[i] < '9') { v->d[i]++; break; } v->d[i] = '0'; }
        if (i < 0) { v->d[0] = '1'; v->x++; keep = keep ? keep : 1; }
    }
    v->nd = keep;
    while (v->nd && v->d[v->nd - 1] == '0') v->nd--;
    if (!v->nd) v->x = 0;
}

static char decdigit(Dec *v, int i) { return i >= 0 && i < v->nd ? v->d[i] : '0'; }

/* conv f e E g G; out gets the number without its sign */
static void fmtfloat(Dec v, char conv, int prec, int alt, char *out, int size)
{
    int o = 0, i, g = conv == 'g' || conv == 'G', fixed = conv == 'f' || conv == 'F';
    if (prec < 0) prec = 6;
    if (g) {
        Dec t = v;
        int P = prec ? prec : 1, X;
        decround(&t, P);
        X = t.nd ? t.x - 1 : 0;
        if (P > X && X >= -4) { fixed = 1; prec = P - 1 - X; }
        else prec = P - 1;
    }
    if (prec > size - 40) prec = size - 40;
    if (fixed) {
        decround(&v, v.x + prec);
        if (v.x <= 0 || !v.nd) out[o++] = '0';
        else for (i = 0; i < v.x && o < size - prec - 3; i++) out[o++] = decdigit(&v, i);
        if (prec || alt) out[o++] = '.';
        for (i = 0; i < prec; i++) out[o++] = decdigit(&v, v.x + i);
    } else {
        int ex;
        decround(&v, prec + 1);
        ex = v.nd ? v.x - 1 : 0;
        out[o++] = decdigit(&v, 0);
        if (prec || alt) out[o++] = '.';
        for (i = 1; i <= prec; i++) out[o++] = decdigit(&v, i);
        out[o++] = conv == 'E' || conv == 'G' ? 'E' : 'e';
        out[o++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        if (ex >= 100) out[o++] = '0' + ex / 100;
        out[o++] = '0' + ex / 10 % 10;
        out[o++] = '0' + ex % 10;
    }
    out[o] = 0;
    if (g && !alt && strchr(out, '.')) {      /* %g: no trailing zeros */
        char *e = strpbrk(out, "eE"), tail[8] = "";
        int n;
        if (e) { strcpy(tail, e); *e = 0; }
        n = strlen(out);
        while (n && out[n - 1] == '0') out[--n] = 0;
        if (n && out[n - 1] == '.') out[--n] = 0;
        strcat(out, tail);
    }
}

/* printf [-v var] FORMAT [ARGS]: %s %b %c %d %i %u %x %X %o %f %e %g %%,
 * flags - 0 +, width, .precision; the format again while arguments are left. */
static long b_printf1(int argc, char **argv);

static long b_printf(int argc, char **argv)
{
    long r;
    char *t;
    if (argc > 2 && !strcmp(argv[1], "-v")) {
        if (argc < 4) { sh_error("printf:", "a format is needed"); return 10; }
        sh_hook(1);
        r = b_printf1(argc - 2, argv + 2);
        if ((t = sh_hooked())) { if (!sh_set(argv[2], t, V_ASSIGN)) r = 10; os_free(t); }
        return r;
    }
    return b_printf1(argc, argv);
}

static long b_printf1(int argc, char **argv)
{
    const char *f;
    int a = 2;
    long rc = 0;
    if (argc < 2) { sh_error("printf:", "a format is needed"); return 10; }
    do {
        int used = 0;
        for (f = argv[1]; *f; f++) {
            if (*f == '\\') {
                char c;
                int k;
                if (!f[1]) { sh_print("\\"); break; }
                if ((k = esc1(f + 1, &c, 0)) < 0) return rc;
                if (!k) { sh_print("\\"); continue; }
                sh_printn(&c, 1); f += k;
                continue;
            }
            if (*f != '%') { sh_printn(f, 1); continue; }
            if (f[1] == '%') { sh_print("%"); f++; continue; }
            {
                const char *arg;
                char conv, out[48];
                int w = 0, left = 0, zero = 0, prec = -1, l = 0, plus = 0, alt = 0;
                for (f++; *f && strchr("-+ 0#", *f); f++) { if (*f == '-') left = 1; if (*f == '0') zero = 1; if (*f == '+') plus = 1; if (*f == '#') alt = 1; }
                for (; *f >= '0' && *f <= '9'; f++) if (w < 1000) w = w * 10 + *f - '0';
                if (*f == '.') { prec = 0; for (f++; *f >= '0' && *f <= '9'; f++) if (prec < 1000) prec = prec * 10 + *f - '0'; }
                if (!*f) break;
                conv = *f;
                arg = a < argc ? argv[a] : "";
                if (a < argc) used = 1;
                a++;
                if (conv == 's' || conv == 'b') {
                    l = strlen(arg);
                    if (conv == 's' && prec >= 0 && l > prec) l = prec;
                    if (!left) pad(w - l, 0);
                    if (conv == 's') sh_printn(arg, l);
                    else if (escapes(arg, 1)) return rc;
                    if (left) pad(w - l, 0);
                    continue;
                }
                if (conv && strchr("feEgGF", conv)) {
                    Dec v;
                    char num[200];
                    if (!decread(*arg ? arg : "0", &v)) { sh_error("printf: not a number:", arg); rc = 10; }
                    fmtfloat(v, conv, prec, alt, num, sizeof num);
                    l = strlen(num) + (v.neg || plus);
                    if (!left && !zero) pad(w - l, 0);
                    if (v.neg) sh_print("-"); else if (plus) sh_print("+");
                    if (!left && zero) pad(w - l, 1);
                    sh_print(num);
                    if (left) pad(w - l, 0);
                    continue;
                }
                if (conv == 'c') {
                    l = arg[0] ? 1 : 0;
                    if (!left) pad(w - l, 0);
                    sh_printn(arg, l);
                    if (left) pad(w - l, 0);
                    continue;
                }
                if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    long v = 0;
                    unsigned long u;
                    int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : 10, neg = 0, tl = 0;
                    char t[24];
                    if (*arg && !num(arg, &v)) {
                        if (arg[0] == '\'' || arg[0] == '"') v = (unsigned char)arg[1];
                        else { sh_error("printf: not a number:", arg); rc = 10; }
                    }
                    if ((conv == 'd' || conv == 'i') && v < 0) { neg = 1; u = -(unsigned long)v; } else u = (unsigned long)v;
                    do { int d = u % base; t[tl++] = d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10; u /= base; } while (u);
                    if (neg) out[l++] = '-';
                    /* a precision's zeros: printed, not put in out (%.500d) */
                    if (prec > tl) {          /* the sign, then prec digits; 0 flag ignored, as in C */
                        if (!left) pad(w - (l + prec), 0);
                        sh_printn(out, l);
                        pad(prec - tl, 1);
                        while (tl) { char ch = t[--tl]; sh_printn(&ch, 1); }
                        if (left) pad(w - (l + prec), 0);
                        continue;
                    }
                    while (tl) out[l++] = t[--tl];
                    if (!left && w > l) {
                        if (zero && neg) { sh_print("-"); pad(w - l, 1); sh_printn(out + 1, l - 1); continue; }
                        pad(w - l, zero);
                    }
                    sh_printn(out, l);
                    if (left) pad(w - l, 0);
                    continue;
                }
                sh_error("printf: unknown conversion in", argv[1]);
                return 10;
            }
        }
        if (!used) break;
    } while (a < argc);
    return rc;
}

/* ---- test / [ ---- */

static char **tv;
static int tn, ti, terr;

static int t_or(void);

static int isbinop(const char *op)
{
    static const char *const ops[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", NULL };
    int i;
    for (i = 0; ops[i]; i++) if (!strcmp(op, ops[i])) return 1;
    return 0;
}

static int t_prim(void)
{
    const char *a;
    if (ti >= tn) { terr = 1; return 0; }
    a = tv[ti];
    /* a OP b first, as POSIX reads three words: [ "(" = "(" ], [ "!" = "$x" ] */
    if (ti + 2 < tn && isbinop(tv[ti + 1]) && (strcmp(a, "(") || ti + 3 >= tn || strcmp(tv[ti + 3], ")"))) goto binary;
    if (!strcmp(a, "!")) { ti++; return !t_prim(); }
    if (!strcmp(a, "(") ) {
        int v;
        ti++;
        v = t_or();
        if (ti < tn && !strcmp(tv[ti], ")")) ti++; else terr = 1;
        return v;
    }
binary:
    if (ti + 2 < tn) {
        const char *op = tv[ti + 1], *b = tv[ti + 2];
        long x, y;
        if (!strcmp(op, "=") || !strcmp(op, "==")) { ti += 3; return !strcmp(a, b); }
        if (!strcmp(op, "!=")) { ti += 3; return strcmp(a, b) != 0; }
        if (!strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") ||
            !strcmp(op, "-le") || !strcmp(op, "-gt") || !strcmp(op, "-ge")) {
            ti += 3;
            if (!num(a, &x) || !num(b, &y)) { sh_error("test: not a number:", num(a, &x) ? b : a); terr = 2; return 0; }
            if (!strcmp(op, "-eq")) return x == y;
            if (!strcmp(op, "-ne")) return x != y;
            if (!strcmp(op, "-lt")) return x < y;
            if (!strcmp(op, "-le")) return x <= y;
            if (!strcmp(op, "-gt")) return x > y;
            return x >= y;
        }
    }
    /* unary */
    if (a[0] == '-' && a[1] && !a[2] && ti + 1 < tn) {
        const char *b = tv[ti + 1];
        switch (a[1]) {
        case 'z': ti += 2; return !*b;
        case 'n': ti += 2; return *b != 0;
        case 'e': case 'f': case 'd': case 'r': case 'w': case 'x': case 's':
            ti += 2; return os_test(b, a[1]);
        }
    }
    ti++;
    return *a != 0;                   /* a word alone: true when not empty */
}

static int t_and(void)
{
    int v = t_prim();
    while (ti < tn && !strcmp(tv[ti], "-a")) { int w; ti++; w = t_prim(); v = v && w; }
    return v;
}

static int t_or(void)
{
    int v = t_and();
    while (ti < tn && !strcmp(tv[ti], "-o")) { int w; ti++; w = t_and(); v = v || w; }
    return v;
}

static long b_test(int argc, char **argv, int bracket)
{
    int v;
    if (bracket) {
        if (strcmp(argv[argc - 1], "]")) { sh_error("[:", "the closing ] is missing"); return 10; }
        argc--;
    }
    tv = argv + 1; tn = argc - 1; ti = 0; terr = 0;
    if (tn == 0) return SH_FALSE;
    v = t_or();
    if (ti < tn) terr = 1;
    if (terr == 1) { sh_error(bracket ? "[:" : "test:", "cannot read that"); return 10; }
    if (terr) return 10;
    return v ? 0 : SH_FALSE;
}

static long b_read(int argc, char **argv)
{
    char line[LINE_MAX], *p;
    const char *arr = NULL;
    int i = 1, raw = 0, n, v;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-r")) raw = 1;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) arr = argv[++i];
        else if (!strcmp(argv[i], "-ra") && i + 1 < argc) { raw = 1; arr = argv[++i]; }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) sh_print(argv[++i]);
        else { sh_error("read: unknown option", argv[i]); return 10; }
        i++;
    }
    n = os_readline(line, sizeof line);
    if (n < 0) return SH_FALSE;
    if (n == (int)sizeof line - 1 && line[n - 1] != '\n') {
        /* longer than csh's line: the rest is read and left out (not the next read's) */
        char rest[256];
        int k;
        while ((k = os_readline(rest, sizeof rest)) > 0 && rest[k - 1] != '\n') ;
    }
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!raw) {                         /* \x is x */
        char *d = line, *s = line;
        for (; *s; s++) { if (*s == '\\' && s[1]) s++; *d++ = *s; }
        *d = 0;
    }
    if (arr) {                          /* read -a name: each word an element */
        char *w[LINE_MAX / 2];
        int k = 0;
        for (p = line; *p; ) {
            while (*p == ' ' || *p == '\t') *p++ = 0;
            if (!*p) break;
            w[k++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        sh_setarray(arr, k, w);
        return 0;
    }
    if (i >= argc) { sh_set("REPLY", line, V_ASSIGN); return 0; }
    p = line;
    for (v = i; v < argc; v++) {
        char *start, save;
        while (*p == ' ' || *p == '\t') p++;
        start = p;
        if (v == argc - 1) { char *e = p + strlen(p); while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--; save = *e; *e = 0; sh_set(argv[v], start, V_ASSIGN); *e = save; break; }
        while (*p && *p != ' ' && *p != '\t') p++;
        save = *p; *p = 0;
        sh_set(argv[v], start, V_ASSIGN);
        *p = save;
    }
    return 0;
}

static char *joined(int argc, char **argv)
{
    long n = 1;
    int i;
    char *t;
    for (i = 0; i < argc; i++) n += strlen(argv[i]) + 1;
    if (!(t = os_alloc(n))) return NULL;
    for (i = 0; i < argc; i++) { if (i) strcat(t, " "); strcat(t, argv[i]); }
    return t;
}

/* A word quoted for csh again ('...' with ' as '\''), for time. */
static char *requoted(int argc, char **argv)
{
    long n = 1;
    int i;
    char *t, *o;
    const char *s;
    for (i = 0; i < argc; i++) n += strlen(argv[i]) * 4 + 3;
    if (!(t = os_alloc(n))) return NULL;
    o = t;
    for (i = 0; i < argc; i++) {
        if (i) *o++ = ' ';
        *o++ = '\'';
        for (s = argv[i]; *s; s++) { if (*s == '\'') { memcpy(o, "'\\''", 4); o += 4; } else *o++ = *s; }
        *o++ = '\'';
    }
    *o = 0;
    return t;
}

/* ---- cd, pushd, popd, dirs ---- */

/* cd - , cd (HOME, else SYS:), cd ../x (bash's .. is the Amiga's /) */
static long cdto(const char *to, int say)
{
    char now[256], t[600], *o = t;
    const char *s = to;
    int had = os_pwd(now, sizeof now);
    while (s[0] == '.' && (s[1] == '/' || !s[1] || (s[1] == '.' && (s[2] == '/' || !s[2])))) {
        if (s[1] == '.') { *o++ = '/'; s += s[2] ? 3 : 2; }
        else s += s[1] ? 2 : 1;
    }
    strncpy(o, s, sizeof t - (o - t) - 1);
    t[sizeof t - 1] = 0;
    if (!os_cd(t)) return 10;
    if (had) sh_set("OLDPWD", now, V_LOCAL);
    if (os_pwd(now, sizeof now)) { sh_set("PWD", now, V_LOCAL); if (say) { sh_print(now); sh_print("\n"); } }
    return 0;
}

static long b_cd(int argc, char **argv)
{
    char b[256];
    if (argc > 2) { sh_error("cd:", "too many arguments"); return 10; }
    if (argc < 2 || !strcmp(argv[1], "~")) {
        if (sh_get("HOME", b, sizeof b) <= 0) strcpy(b, "SYS:");
        return cdto(b, 0);
    }
    if (!strcmp(argv[1], "-")) {
        if (sh_get("OLDPWD", b, sizeof b) <= 0) { sh_error("cd:", "OLDPWD not set"); return 10; }
        return cdto(b, 1);
    }
    return cdto(argv[1], 0);
}

#define DMAX 16
static char *dstack[DMAX];
static int ndirs;

static void dirs_print(void)
{
    char b[256];
    int i;
    if (os_pwd(b, sizeof b)) sh_print(b);
    for (i = 0; i < ndirs; i++) { sh_print(" "); sh_print(dstack[i]); }
    sh_print("\n");
}

static long b_pushd(int argc, char **argv)
{
    char now[256];
    char *top;
    if (!os_pwd(now, sizeof now)) return 10;
    if (argc < 2) {                  /* the top two swap */
        if (!ndirs) { sh_error("pushd:", "no other directory"); return 10; }
        top = dstack[0];
        if (cdto(top, 0)) return 10;
        dstack[0] = os_alloc(strlen(now) + 1);
        if (dstack[0]) strcpy(dstack[0], now);
        os_free(top);
        dirs_print();
        return 0;
    }
    if (ndirs == DMAX) { sh_error("pushd:", "the stack is full"); return 10; }
    if (cdto(argv[1], 0)) return 10;
    if ((top = os_alloc(strlen(now) + 1))) {
        strcpy(top, now);
        memmove(dstack + 1, dstack, ndirs * sizeof *dstack);
        dstack[0] = top;
        ndirs++;
    }
    dirs_print();
    return 0;
}

static long b_popd(void)
{
    char *top;
    long r;
    if (!ndirs) { sh_error("popd:", "the directory stack is empty"); return 10; }
    top = dstack[0];
    r = cdto(top, 0);
    os_free(top);
    memmove(dstack, dstack + 1, --ndirs * sizeof *dstack);
    if (!r) dirs_print();
    return r;
}

/* ---- getopts optstring name [args] ---- */

static int optpos;              /* in a bundle (-abc): the next letter's place */
static long lastind = -1;

static long b_getopts(int argc, char **argv)
{
    const char *opts, *a, *p, *var;
    char b[24], c, one[2] = { 0, 0 };
    long ind = 1;
    int silent, nargs;
    if (argc < 3) { sh_error("getopts:", "getopts optstring name [arg...]"); return 10; }
    opts = argv[1]; var = argv[2];
    silent = *opts == ':';
    if (silent) opts++;
    nargs = argc > 3 ? argc - 3 : sh_npos();
    if (sh_get("OPTIND", b, sizeof b) > 0 && !num(b, &ind)) ind = 1;
    if (ind != lastind) optpos = 0;          /* OPTIND=1 by hand: from the start */
    if (ind < 1) ind = 1;
#define ARG(i) (argc > 3 ? argv[2 + (i)] : (sh_getpos((i), (char **)&p), p))
    if (ind > nargs) goto end;
    a = ARG(ind);
    if (!optpos) {
        if (a[0] != '-' || !a[1]) goto end;
        if (!strcmp(a, "--")) { ind++; goto end; }
        optpos = 1;
    }
    c = a[optpos++];
    one[0] = c;
    p = c != ':' ? strchr(opts, c) : NULL;
    if (!p) {
        if (silent) sh_set("OPTARG", one, V_ASSIGN);
        else { sh_error("getopts: illegal option --", one); sh_unset("OPTARG"); }
        sh_set(var, "?", V_ASSIGN);
    } else if (p[1] == ':') {
        if (a[optpos]) { sh_set("OPTARG", a + optpos, V_ASSIGN); ind++; optpos = 0; }
        else if (ind + 1 <= nargs) { sh_set("OPTARG", ARG(ind + 1), V_ASSIGN); ind += 2; optpos = 0; }
        else {
            if (silent) { sh_set("OPTARG", one, V_ASSIGN); sh_set(var, ":", V_ASSIGN); }
            else { sh_error("getopts: option requires an argument --", one); sh_unset("OPTARG"); sh_set(var, "?", V_ASSIGN); }
            ind++; optpos = 0;
            goto out;
        }
        sh_set(var, one, V_ASSIGN);
    } else { sh_set(var, one, V_ASSIGN); sh_unset("OPTARG"); }
    if (optpos && !a[optpos]) { ind++; optpos = 0; }
out:
    ltoa_b(ind, b);
    sh_set("OPTIND", b, V_ASSIGN);
    lastind = ind;
    return 0;
end:
    optpos = 0;
    ltoa_b(ind, b);
    sh_set("OPTIND", b, V_ASSIGN);
    lastind = ind;
    sh_set(var, "?", V_ASSIGN);
    return SH_FALSE;
#undef ARG
}

/* mapfile [-t] [name]: the lines of the input into an array (MAPFILE) */
static long b_mapfile(int argc, char **argv)
{
    char line[LINE_MAX], **v = NULL;
    const char *name = "MAPFILE";
    int i, n = 0, cap = 0, trim = 0, l;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) trim = 1;
        else if (argv[i][0] == '-') { sh_error("mapfile: unknown option", argv[i]); return 10; }
        else name = argv[i];
    }
    while ((l = os_readline(line, sizeof line)) >= 0) {
        if (trim) while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (n == cap) {
            char **nv = os_alloc((cap = cap * 2 + 16) * sizeof *nv);
            if (!nv) break;
            if (v) { memcpy(nv, v, n * sizeof *nv); os_free(v); }
            v = nv;
        }
        if (!(v[n] = os_alloc(l + 1))) break;
        memcpy(v[n++], line, l);
    }
    sh_setarray(name, n, v);
    for (i = 0; i < n; i++) os_free(v[i]);
    os_free(v);
    return 0;
}

long builtin_run(int which, int argc, char **argv)
{
    long n;
    switch (which) {
    case B_EXIT:
        n = sh_rc();
        if (argc > 1 && !num(argv[1], &n)) { sh_error("exit: not a number:", argv[1]); n = 10; }
        sh_exiting = 1; sh_exitrc = n;
        return n;
    case B_RETURN:
        n = sh_rc();
        if (argc > 1 && !num(argv[1], &n)) { sh_error("return: not a number:", argv[1]); n = 10; }
        sh_return = 1;
        return n;
    case B_BREAK: case B_CONTINUE:
        if (!sh_loops) {
            if (which == B_BREAK) return NOTMINE;    /* the Amiga's Break */
            sh_error("continue:", "only in a loop");
            return 10;
        }
        n = 1;
        if (argc > 1 && (!num(argv[1], &n) || n < 1)) { sh_error(which == B_BREAK ? "break: not a count:" : "continue: not a count:", argv[1]); return 10; }
        if (n > sh_loops) n = sh_loops;
        if (which == B_BREAK) sh_break = n; else sh_continue = n;
        return 0;
    case B_SHIFT: {
        int k = sh_npos();
        n = 1;
        if (argc > 1 && (!num(argv[1], &n) || n < 0)) { sh_error("shift: not a count:", argv[1]); return 10; }
        if (n > k) return SH_FALSE;
        {
            char **all = os_alloc((k + 1) * sizeof *all);
            int i;
            if (!all) return 20;
            for (i = 0; i < k; i++) sh_getpos(i + 1, &all[i]);
            sh_setpos(k - n, all + n);
            os_free(all);
        }
        return 0;
    }
    case B_TRUE: case B_COLON: return 0;
    case B_FALSE: return SH_FALSE;
    case B_ECHO: return b_echo(argc, argv);
    case B_PRINTF: return b_printf(argc, argv);
    case B_TEST: return b_test(argc, argv, 0);
    case B_BRACKET: return b_test(argc, argv, 1);
    case B_READ: return b_read(argc, argv);
    case B_SOURCE: case B_DOT:
        if (argc < 2) { sh_error("source:", "a file is needed"); return 10; }
        return lang_source(argv[1], argc - 2, argv + 2);
    case B_EVAL: {
        char *t = joined(argc - 1, argv + 1);
        if (!t) return 20;
        n = lang_eval(t);
        os_free(t);
        return n;
    }
    case B_EXPORT: {
        int i;
        for (i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; os_setvar(argv[i], eq + 1, V_EXPORT); *eq = '='; }
            else {
                char v[LINE_MAX];
                int l = os_getvar(argv[i], v, sizeof v);
                if (l >= 0) { v[l < (int)sizeof v ? l : (int)sizeof v - 1] = 0; os_setvar(argv[i], v, V_EXPORT); }
            }
        }
        return 0;
    }
    case B_UNSET: {
        int i, fn = 0;
        long r = 0;
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-f")) { fn = 1; continue; }
            if (!strcmp(argv[i], "-v")) { fn = 0; continue; }
            if (fn) sh_unsetfunc(argv[i]);
            else if (!sh_unset(argv[i])) r = 1;
        }
        return r;
    }
    case B_SET: {
        int i;
        if (argc < 2) return NOTMINE;            /* the ROM's Set lists them */
        if (argv[1][0] == '-' || argv[1][0] == '+') {
            static const char *const longs[] = { "errexit", "xtrace", "nounset", "noglob", "pipefail", "histexpand", NULL };
            int *const flags[] = { &sh_opt_e, &sh_opt_x, &sh_opt_u, &sh_opt_f, &sh_opt_pipefail, &sh_opt_histexp };
            for (i = 1; i < argc; i++) {
                const char *o = argv[i];
                int on = o[0] == '-';
                if (!strcmp(o, "--")) { sh_setpos(argc - i - 1, argv + i + 1); break; }
                if (o[0] != '-' && o[0] != '+') { sh_setpos(argc - i, argv + i); break; }
                for (o++; *o; o++) {
                    if (*o == 'e') sh_opt_e = on;
                    else if (*o == 'x') sh_opt_x = on;
                    else if (*o == 'u') sh_opt_u = on;
                    else if (*o == 'f') sh_opt_f = on;
                    else if (*o == 'H') sh_opt_histexp = on;
                    else if (*o == 'o') {                   /* -o name, set -euo pipefail */
                        int k;
                        if (i + 1 >= argc) {                /* set -o: what is on */
                            for (k = 0; longs[k]; k++) { sh_print(longs[k]); sh_print(*flags[k] ? "\ton\n" : "\toff\n"); }
                            return 0;
                        }
                        i++;
                        for (k = 0; longs[k] && strcmp(longs[k], argv[i]); k++) ;
                        if (!longs[k]) { sh_error("set: unknown option", argv[i]); return 10; }
                        *flags[k] = on;
                        break;
                    }
                    else { char b[2] = { *o, 0 }; sh_error("set: unknown option", b); return 10; }
                }
            }
            return 0;
        }
        /* Set name value (the Amiga's form; set name value too) */
        {
            char *v = joined(argc - 2, argv + 2);
            if (!v) return 20;
            n = sh_set(argv[1], v, V_ASSIGN) ? 0 : 10;
            os_free(v);
        }
        return n;
    }
    case B_ALIAS: {
        int i;
        /* alias name=text is csh's; alias name text (an = in the text or not) the ROM's */
        if (argc < 2 || !strchr(argv[1], '=')) return NOTMINE;
        for (i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = 0;
            os_setalias(argv[i], eq + 1);
            *eq = '=';
        }
        return 0;
    }
    case B_HISTORY: os_history(); return 0;
    case B_TIME: {
        long t0 = os_ticks(), t1, r;
        char *t = requoted(argc - 1, argv + 1), b[24];
        int l;
        if (!t) return 20;
        r = lang_eval(t);
        os_free(t);
        t1 = os_ticks() - t0;                    /* fiftieths of a second */
        os_tty("real ", 5);
        b[0] = 0;
        {
            long s = t1 / 50, h = (t1 % 50) * 2;
            char *o = b;
            long d = 1;
            while (d * 10 <= s) d *= 10;
            for (; d; d /= 10) *o++ = '0' + (s / d) % 10;
            *o++ = '.'; *o++ = '0' + h / 10; *o++ = '0' + h % 10; *o++ = 's'; *o++ = '\n'; *o = 0;
        }
        l = strlen(b);
        os_tty(b, l);                            /* the console, as bash's goes to stderr */
        return r;
    }
    case B_JOBS: return job_list();
    case B_FG: return job_fg(argc > 1 ? argv[1] : NULL);
    case B_KILL: {
        int i;
        long r = 0;
        if (argc < 2) return job_kill(NULL);
        for (i = 1; i < argc; i++) if (job_kill(argv[i])) r = 10;
        return r;
    }
    case B_WAIT:
        /* wait 5 is the Amiga's Wait (seconds); csh's wait has none or %n */
        if (argc > 1 && argv[1][0] != '%') return NOTMINE;
        if (argc > 1) return job_fg(argv[1]);
        return job_waitall();
    case B_LOCAL: {
        int i;
        for (i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            int r;
            if (eq) { *eq = 0; r = sh_local(argv[i], eq + 1); *eq = '='; }
            else r = sh_local(argv[i], NULL);
            if (r < 0) { sh_error("local:", "only in a function"); return 10; }
        }
        return 0;
    }
    case B_CD: return b_cd(argc, argv);
    case B_PUSHD: return b_pushd(argc, argv);
    case B_POPD: return b_popd();
    case B_DIRS: dirs_print(); return 0;
    case B_TRAP: return sh_trap(argc, argv);
    case B_GETOPTS: return b_getopts(argc, argv);
    case B_LET: return sh_let(argc, argv);
    case B_MAPFILE: case B_READARRAY: return b_mapfile(argc, argv);
    case B_PWD: {
        char b[256];
        if (!os_pwd(b, sizeof b)) return 10;
        sh_print(b); sh_print("\n");
        return 0;
    }
    }
    return NOTMINE;
}
