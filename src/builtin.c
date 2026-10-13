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
       B_HISTORY, B_TIME, B_PWD, B_JOBS, B_FG, B_KILL, B_WAIT, B_COUNT };

const char *const builtin_names[] = {
    "exit", "return", "break", "continue", "shift", "true", "false", ":", "echo", "printf",
    "test", "[", "read", "source", ".", "eval", "export", "unset", "set", "alias",
    "history", "time", "pwd", "jobs", "fg", "kill", "wait", NULL
};
/* lower case only: an Amiga command of that name in any other case */
static const char exact[B_COUNT] = { [B_EVAL] = 1, [B_BREAK] = 1, [B_TIME] = 1, [B_WAIT] = 1 };

long os_ticks(void);
int os_pwd(char *buf, int size);

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

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
    while (*s >= '0' && *s <= '9') { n = n * 10 + *s++ - '0'; any = 1; }
    while (*s == ' ' || *s == '\t') s++;
    if (!any || *s) return 0;
    *v = neg ? -n : n;
    return 1;
}

/* \n \t \\ \e \a \r \0nnn \c (stop) */
static int escapes(const char *s, int stopc)
{
    char b[256];
    int o = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'a': c = 7; break;
            case 'e': case 'E': c = 27; break;
            case '\\': c = '\\'; break;
            case 'c': if (stopc) { if (o) sh_printn(b, o); return 1; } c = 'c'; break;
            case '0': { int v = 0, k; for (k = 0; k < 3 && s[1] >= '0' && s[1] <= '7'; k++) v = v * 8 + *++s - '0'; c = v; break; }
            default: if (o) sh_printn(b, o); o = 0; b[o++] = '\\'; c = *s; break;
            }
        }
        b[o++] = c;
        if (o == (int)sizeof b) { sh_printn(b, o); o = 0; }
    }
    if (o) sh_printn(b, o);
    return 0;
}

static long b_echo(int argc, char **argv)
{
    int i = 1, nl = 1, esc = 0, k;
    for (k = 1; k < argc; k++) {
        const char *a = argv[k];
        if (ieq(a, "NOLINE") || ieq(a, "FIRST") || ieq(a, "LEN") || ieq(a, "TO")) return NOTMINE;
    }
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

static long b_printf(int argc, char **argv)
{
    const char *f;
    int a = 2;
    if (argc < 2) { sh_error("printf:", "a format is needed"); return 10; }
    do {
        int used = 0;
        for (f = argv[1]; *f; f++) {
            char spec[16], out[300];
            int sl = 0;
            if (*f == '\\') {
                char t[3] = { '\\', f[1], 0 };
                if (!f[1]) { sh_print("\\"); break; }
                escapes(t, 0); f++;
                continue;
            }
            if (*f != '%') { sh_printn(f, 1); continue; }
            if (f[1] == '%') { sh_print("%"); f++; continue; }
            spec[sl++] = '%';
            for (f++; *f && strchr("-+ 0#", *f) && sl < 6; f++) spec[sl++] = *f;
            for (; *f >= '0' && *f <= '9' && sl < 10; f++) spec[sl++] = *f;
            if (*f == '.') { spec[sl++] = *f++; for (; *f >= '0' && *f <= '9' && sl < 13; f++) spec[sl++] = *f; }
            if (!*f) break;
            spec[sl] = 0;
            {
                const char *arg = a < argc ? argv[a] : "";
                char conv = *f;
                int w = 0, left = 0, zero = 0, prec = -1, k, l;
                const char *p = spec + 1;
                if (a < argc) used = 1;
                a++;
                for (; *p == '-' || *p == '0' || *p == '+' || *p == ' ' || *p == '#'; p++) { if (*p == '-') left = 1; if (*p == '0') zero = 1; }
                while (*p >= '0' && *p <= '9') w = w * 10 + *p++ - '0';
                if (*p == '.') { prec = 0; p++; while (*p >= '0' && *p <= '9') prec = prec * 10 + *p++ - '0'; }
                l = 0;
                if (conv == 's') {
                    l = strlen(arg);
                    if (prec >= 0 && l > prec) l = prec;
                    if (l > 256) l = 256;
                    memcpy(out, arg, l);
                } else if (conv == 'c') {
                    out[0] = arg[0]; l = arg[0] ? 1 : 0;
                } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    long v = 0;
                    unsigned long u;
                    int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : 10, neg = 0;
                    char t[24];
                    int tl = 0;
                    if (*arg && !num(arg, &v)) {
                        if (arg[0] == '\'' || arg[0] == '"') v = (unsigned char)arg[1];
                        else { sh_error("printf: not a number:", arg); }
                    }
                    if ((conv == 'd' || conv == 'i') && v < 0) { neg = 1; u = -(unsigned long)v; } else u = (unsigned long)v;
                    do { int d = u % base; t[tl++] = d < 10 ? '0' + d : (conv == 'X' ? 'A' : 'a') + d - 10; u /= base; } while (u);
                    if (neg) out[l++] = '-';
                    while (prec > tl) { out[l++] = '0'; prec--; }
                    while (tl) out[l++] = t[--tl];
                } else { sh_error("printf: unknown conversion in", argv[1]); return 10; }
                if (!left && w > l) {
                    for (k = 0; k < w - l; k++) sh_print(zero && conv != 's' && conv != 'c' ? "0" : " ");
                }
                sh_printn(out, l);
                if (left && w > l) for (k = 0; k < w - l; k++) sh_print(" ");
            }
        }
        if (!used) break;
    } while (a < argc);
    return 0;
}

/* ---- test / [ ---- */

static char **tv;
static int tn, ti, terr;

static int t_or(void);

static int t_prim(void)
{
    const char *a;
    if (ti >= tn) { terr = 1; return 0; }
    a = tv[ti];
    if (!strcmp(a, "!")) { ti++; return !t_prim(); }
    if (!strcmp(a, "(") ) {
        int v;
        ti++;
        v = t_or();
        if (ti < tn && !strcmp(tv[ti], ")")) ti++; else terr = 1;
        return v;
    }
    /* binary: a OP b */
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
    int i = 1, raw = 0, n, v;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-r")) raw = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) sh_print(argv[++i]);
        else break;
        i++;
    }
    n = os_readline(line, sizeof line);
    if (n < 0) return SH_FALSE;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!raw) {                         /* \x is x */
        char *d = line, *s = line;
        for (; *s; s++) { if (*s == '\\' && s[1]) s++; *d++ = *s; }
        *d = 0;
    }
    if (i >= argc) { os_setvar("REPLY", line, 0); return 0; }
    p = line;
    for (v = i; v < argc; v++) {
        char *start, save;
        while (*p == ' ' || *p == '\t') p++;
        start = p;
        if (v == argc - 1) { char *e = p + strlen(p); while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--; save = *e; *e = 0; os_setvar(argv[v], start, 0); *e = save; break; }
        while (*p && *p != ' ' && *p != '\t') p++;
        save = *p; *p = 0;
        os_setvar(argv[v], start, 0);
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
        if (argc > 1 && !num(argv[1], &n)) n = 10;
        sh_return = 1;
        return n;
    case B_BREAK: case B_CONTINUE:
        if (!sh_loops) {
            if (which == B_BREAK) return NOTMINE;    /* the Amiga's Break */
            sh_error("continue:", "only in a loop");
            return 10;
        }
        n = 1;
        if (argc > 1 && (!num(argv[1], &n) || n < 1)) n = 1;
        if (n > sh_loops) n = sh_loops;
        if (which == B_BREAK) sh_break = n; else sh_continue = n;
        return 0;
    case B_SHIFT: {
        char *v[1];
        int k = sh_npos();
        n = 1;
        if (argc > 1 && !num(argv[1], &n)) n = 1;
        if (n > k) return SH_FALSE;
        {
            char **all = os_alloc((k + 1) * sizeof *all);
            int i;
            if (!all) return 20;
            for (i = 0; i < k; i++) sh_getpos(i + 1, &all[i]);
            sh_setpos(k - n, all + n);
            os_free(all);
        }
        (void)v;
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
        if (argc < 2) { sh_error(argv[0], "needs a file"); return 10; }
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
            if (eq) { *eq = 0; os_setvar(argv[i], eq + 1, 1); *eq = '='; }
            else {
                char v[LINE_MAX];
                if (os_getvar(argv[i], v, sizeof v) >= 0) os_setvar(argv[i], v, 1);
            }
        }
        return 0;
    }
    case B_UNSET: {
        int i;
        for (i = 1; i < argc; i++) os_unsetvar(argv[i]);
        return 0;
    }
    case B_SET: {
        int i;
        if (argc < 2) return NOTMINE;            /* the ROM's Set lists them */
        if (argv[1][0] == '-' || argv[1][0] == '+') {
            for (i = 1; i < argc; i++) {
                const char *o = argv[i];
                int on = o[0] == '-';
                if (!strcmp(o, "--")) { sh_setpos(argc - i - 1, argv + i + 1); break; }
                if (o[0] != '-' && o[0] != '+') { sh_setpos(argc - i, argv + i); break; }
                for (o++; *o; o++) {
                    if (*o == 'e') sh_opt_e = on;
                    else if (*o == 'x') sh_opt_x = on;
                    else { char b[2] = { *o, 0 }; sh_error("set: unknown option", b); return 10; }
                }
            }
            return 0;
        }
        /* Set name value (the Amiga's form; set name value too) */
        {
            char *v = joined(argc - 2, argv + 2);
            if (!v) return 20;
            os_setvar(argv[1], v, 0);
            os_free(v);
        }
        return 0;
    }
    case B_ALIAS: {
        int i, any = 0;
        for (i = 1; i < argc; i++) if (strchr(argv[i], '=')) any = 1;
        if (!any) return NOTMINE;                /* the ROM's Alias */
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
        sh_print("real ");
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
        sh_printn(b, l);
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
    case B_PWD: {
        char b[256];
        if (!os_pwd(b, sizeof b)) return 10;
        sh_print(b); sh_print("\n");
        return 0;
    }
    }
    return NOTMINE;
}
