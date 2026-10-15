/* lang.c - the csh language: the common part of bash, on Amiga terms
 * (docs/design.md "The csh language"; the rules where it meets the Amiga's
 * own are in its table "Where they clash").
 *
 *   words      'single' quotes, "double" quotes ($ and ` work inside), \x;
 *              $name ${name} ${name:-x} ${name:=x} ${name:+x} ${#name}
 *              ${name#p} ${name%p}; $1.. $# $@ $* $? $$; $(cmd) `cmd`;
 *              $((arithmetic)); an unknown $name is empty
 *   commands   ; & && || | ! { } ( ), if/elif/else/fi, while, until,
 *              for x in ...; do ... done, case ... esac, name() { ... }
 *   redirect   < > >> <> 2> 2>> &> 2>&1 >&2 <<END <<-END <<<word,
 *              and the Amiga's *> *>>
 *   comments   # at a word's start, except an Amiga pattern (#? #( #[)
 *
 * Amiga commands still work as they are: the words are put back into an
 * Amiga command line (a word that had quotes is quoted the Amiga way,
 * with * and " escaped; an unquoted * becomes #?, so the program matches
 * the pattern itself; KEY=value stays as it is) and run by run.c, the
 * same way the ROM Shell runs them. csh's variables are AmigaDOS local
 * variables, so Get, GetVar and Amiga scripts see the same ones.
 *
 * Parsing: a line (or a script) is read into an arena, run, the arena
 * freed. A function keeps its own copy, parsed again from its text. */
#include <string.h>
#include "lang.h"

typedef unsigned char UBYTE_;

static int ltoa_(long n, char *b)
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

/* ---- memory --------------------------------------------------------------- */

typedef struct Block { struct Block *next; long used, size; } Block;
typedef struct { Block *head; } Arena;
static char *dollarq(Arena *ar, const char *s, int n);   /* ${x@E} */

static void *aalloc(Arena *a, long n)
{
    char *p;
    n = (n + (long)sizeof(long) - 1) & ~((long)sizeof(long) - 1);   /* 4 on the Amiga, 8 on the PC */
    if (!a->head || a->head->used + n > a->head->size) {
        long sz = n > 4000 ? n : 4000;
        Block *b = os_alloc(sizeof(Block) + sz);
        if (!b) return NULL;
        b->size = sz; b->used = 0; b->next = a->head; a->head = b;
    }
    p = (char *)(a->head + 1) + a->head->used;   /* cleared already: os_alloc clears, nothing is reused */
    a->head->used += n;
    return p;
}

static void afree(Arena *a)
{
    while (a->head) { Block *n = a->head->next; os_free(a->head); a->head = n; }
}

static char *astr(Arena *a, const char *s, int n)
{
    char *p = aalloc(a, n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

/* A growing string (output of an expansion). */
typedef struct { char *s; int n, cap; Arena *ar; } Str;

static void sput(Str *b, const char *s, int n)
{
    if (b->n + n + 1 > b->cap) {
        int cap = (b->n + n + 1) * 2 + 32;
        char *p = aalloc(b->ar, cap);
        if (!p) return;
        if (b->s) memcpy(p, b->s, b->n);
        b->s = p; b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sputc(Str *b, char c) { sput(b, &c, 1); }

/* ---- tokens --------------------------------------------------------------- */

enum { T_EOF, T_WORD, T_NL, T_SEMI, T_DSEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LP, T_RP, T_REDIR,
       T_SEMIAMP, T_DSEMIAMP };      /* case's ;& and ;;& */
enum { R_IN, R_OUT, R_APP, R_ERR, R_ERRAPP, R_BOTH, R_BOTHAPP, R_ERROUT, R_OUTERR, R_INOUT, R_HERE, R_HERESTR };

typedef struct Redir {
    int op;
    char *word;                 /* the target, raw (expanded when run) */
    char *body;                 /* here-document text */
    int quoted, striptabs;      /* <<'END': no expansion; <<-: tabs off */
    struct Redir *next;
} Redir;

typedef struct {
    int type, op;
    char *text;
    int len;
    long start, end;
} Tok;

typedef struct {
    const char *src;
    long n, pos;
    int incomplete, error;
    Tok peek;
    int have;
    Arena *ar;
    Redir *hd[16];
    int nhd;
} Lex;

static int isspace_(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int isassign(const char *w, int n);
static int isop(char c) { return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>'; }

/* Skips a $( ... ) or $(( ... )) or ${ ... } or `...`; p at its opening.
 * Returns the position after it, or -1 when the text ends first. The one
 * scanner for the lexer and for expanding, so both agree where a quote or
 * a $( ) ends. Inside $( ), a case pattern's ")" (case x in a) ...) does
 * not close it. */
static long scan_nested(const char *src, long n, long p);

static long scan_quote(const char *src, long n, long p)    /* p at the " */
{
    p++;
    while (p < n) {
        char c = src[p];
        if (c == '"') return p + 1;
        if (c == '\\') { p += 2; continue; }
        if (c == '`' || (c == '$' && p + 1 < n && (src[p + 1] == '(' || src[p + 1] == '{'))) {
            p = scan_nested(src, n, p);
            if (p < 0) return -1;
            continue;
        }
        p++;
    }
    return -1;
}

static int wordch(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* The reserved word "w" at p, in a command's place (after ( ; & | { or a
 * line's start, spaces between), not part of a longer word? */
static int keyword_at(const char *src, long n, long p, const char *w)
{
    int l = strlen(w);
    long b = p;
    if (p + l > n || memcmp(src + p, w, l)) return 0;
    if (p + l < n && wordch(src[p + l])) return 0;
    while (b > 0 && (src[b - 1] == ' ' || src[b - 1] == '\t')) b--;
    return b == 0 || strchr("(;&|{\n", src[b - 1]) != NULL;
}

static long scan_nested(const char *src, long n, long p)
{
    char c = src[p];
    if (c == '`') {
        p++;
        while (p < n && src[p] != '`') { if (src[p] == '\\') p++; p++; }
        return p < n ? p + 1 : -1;
    }
    {
        char open = src[p + 1], close = open == '(' ? ')' : '}';
        int depth = 0, cases = 0, casedepth[8];
        p += 1;
        while (p < n) {
            char d = src[p];
            if (open == '(' && d == 'c' && keyword_at(src, n, p, "case")) {
                if (cases < 8) casedepth[cases] = depth;
                cases++; p += 4; continue;
            }
            if (open == '(' && d == 'e' && cases && keyword_at(src, n, p, "esac")) { cases--; p += 4; continue; }
            if (d == open) depth++;
            else if (d == close) {
                /* a case pattern's ")" at the case's own depth closes nothing */
                if (open == '(' && cases && cases <= 8 && depth == casedepth[cases - 1]) { p++; continue; }
                if (--depth == 0) return p + 1;
            }
            else if (d == '\'') {
                p++;
                while (p < n && src[p] != '\'') p++;
                if (p >= n) return -1;
            } else if (d == '"') { p = scan_quote(src, n, p); if (p < 0) return -1; continue; }
            else if (d == '\\') p++;
            else if (d == '`') { p = scan_nested(src, n, p); if (p < 0) return -1; continue; }
            else if (d == '$' && p + 1 < n && src[p + 1] == '{' && open == '(') { p = scan_nested(src, n, p); if (p < 0) return -1; continue; }
            p++;
        }
        return -1;
    }
}

static long skipquote(Lex *L, long p) { return scan_quote(L->src, L->n, p); }
static long skipnested(Lex *L, long p) { return scan_nested(L->src, L->n, p); }

/* Reads the bodies of the here-documents of the line just ended. */
static void heredocs(Lex *L)
{
    int i;
    for (i = 0; i < L->nhd; i++) {
        Redir *r = L->hd[i];
        Str b;
        int dl = strlen(r->word);
        memset(&b, 0, sizeof b); b.ar = L->ar;
        sput(&b, "", 0);
        for (;;) {
            long s = L->pos, e = s;
            const char *line;
            int ll;
            if (s >= L->n) { L->incomplete = 1; return; }
            while (e < L->n && L->src[e] != '\n') e++;
            line = L->src + s; ll = e - s;
            if (r->striptabs) while (ll && *line == '\t') line++, ll--;
            if (e >= L->n) {
                /* the text's last line, no LF after it: the end only if it is the delimiter */
                if (ll == dl && !memcmp(line, r->word, dl)) { L->pos = L->n; break; }
                L->incomplete = 1; return;             /* the line is not finished yet */
            }
            L->pos = e + 1;
            if (ll == dl && !memcmp(line, r->word, dl)) break;
            sput(&b, line, ll);
            sputc(&b, '\n');
        }
        r->body = b.s;
    }
    L->nhd = 0;
}

static int lex_(Lex *L, Tok *t)
{
    long p;
    memset(t, 0, sizeof *t);
again:
    while (L->pos < L->n && isspace_(L->src[L->pos])) L->pos++;
    if (L->pos + 1 < L->n && L->src[L->pos] == '\\' && L->src[L->pos + 1] == '\n') { L->pos += 2; goto again; }
    p = L->pos;
    t->start = p;
    if (p >= L->n) return t->type = T_EOF;
    {
        char c = L->src[p], d = p + 1 < L->n ? L->src[p + 1] : 0, e = p + 2 < L->n ? L->src[p + 2] : 0;
        if (c == '#' && d != '?' && d != '(' && d != '[' && d != '#' && d != '~') {
            while (L->pos < L->n && L->src[L->pos] != '\n') L->pos++;
            goto again;
        }
        if (c == '\n') { L->pos++; t->type = T_NL; if (L->nhd) heredocs(L); return t->type; }
        if (c == ';' && d == '&') { L->pos += 2; return t->type = T_SEMIAMP; }
        if (c == ';' && d == ';' && e == '&') { L->pos += 3; return t->type = T_DSEMIAMP; }
        if (c == ';') { L->pos += d == ';' ? 2 : 1; return t->type = d == ';' ? T_DSEMI : T_SEMI; }
        if (c == '&' && d == '&') { L->pos += 2; return t->type = T_AND; }
        if (c == '&' && d == '>') { L->pos += e == '>' ? 3 : 2; t->op = e == '>' ? R_BOTHAPP : R_BOTH; return t->type = T_REDIR; }
        if (c == '&') { L->pos++; return t->type = T_AMP; }
        if (c == '|' && d == '|') { L->pos += 2; return t->type = T_OR; }
        if (c == '|' && d == '&') { L->pos += 2; t->op = 1; return t->type = T_PIPE; }
        if (c == '|') { L->pos++; return t->type = T_PIPE; }
        if (c == '(') { L->pos++; return t->type = T_LP; }
        if (c == ')') { L->pos++; return t->type = T_RP; }
        if (c == '<' && d != '(') {
            if (d == '<' && e == '<') { L->pos += 3; t->op = R_HERESTR; }
            else if (d == '<' && e == '-') { L->pos += 3; t->op = R_HERE; t->len = 1; }
            else if (d == '<') { L->pos += 2; t->op = R_HERE; }
            else if (d == '>') { L->pos += 2; t->op = R_INOUT; }
            else { L->pos++; t->op = R_IN; }
            return t->type = T_REDIR;
        }
        if (c == '>') {
            if (d == '&' && e == '2') { L->pos += 3; t->op = R_OUTERR; }
            else if (d == '>') { L->pos += 2; t->op = R_APP; }
            else if (d == '|') { L->pos += 2; t->op = R_OUT; }
            else { L->pos++; t->op = R_OUT; }
            return t->type = T_REDIR;
        }
        /* 2> 2>> 2>&1 1> and the Amiga's *> *>> */
        if ((c == '2' || c == '*') && d == '>') {
            if (c == '2' && e == '&' && p + 3 < L->n && L->src[p + 3] == '1') { L->pos += 4; t->op = R_ERROUT; }
            else if (e == '>') { L->pos += 3; t->op = R_ERRAPP; }
            else { L->pos += 2; t->op = R_ERR; }
            return t->type = T_REDIR;
        }
        if (c == '1' && d == '>') {
            if (e == '&' && p + 3 < L->n && L->src[p + 3] == '2') { L->pos += 4; t->op = R_OUTERR; }
            else if (e == '>') { L->pos += 3; t->op = R_APP; } else { L->pos += 2; t->op = R_OUT; }
            return t->type = T_REDIR;
        }
    }
    /* a word */
    while (p < L->n) {
        char c = L->src[p];
        if (c == '(' && p > L->pos && (L->src[p - 1] == '#' || L->src[p - 1] == '~')) {
            /* an Amiga pattern: #(a|b), ~(x) - to its ) on this line */
            long q = p;
            int d = 0;
            for (; q < L->n && L->src[q] != '\n'; q++) {
                if (L->src[q] == '(') d++;
                else if (L->src[q] == ')' && --d == 0) break;
            }
            if (q < L->n && L->src[q] == ')') { p = q + 1; continue; }
        }
        if (c == '<' && p == L->pos && p + 1 < L->n && L->src[p + 1] == '(') {    /* <(cmd): a file of its output */
            p = skipnested(L, p);
            if (p < 0) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
            continue;
        }
        if (c == '(' && p > L->pos && L->src[p - 1] == '=' && isassign(L->src + L->pos, p - L->pos) == p - L->pos - 1) {
            /* a=(x y z): the list is part of the word */
            int d = 0;
            for (; p < L->n; p++) {
                char k = L->src[p];
                if (k == '\\') { p++; continue; }
                if (k == '\'') { while (++p < L->n && L->src[p] != '\'') ; continue; }
                if (k == '"') { p = skipquote(L, p) - 1; if (p < 0) break; continue; }
                if (k == '(') d++;
                else if (k == ')' && --d == 0) break;
            }
            if (p < 0 || p >= L->n) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
            p++;
            continue;
        }
        if (isspace_(c) || c == '\n' || isop(c)) break;
        if (c == '$' && p + 1 < L->n && L->src[p + 1] == '\'') {        /* $'...' */
            p += 2;
            while (p < L->n && L->src[p] != '\'') { if (L->src[p] == '\\') p++; p++; }
            if (p >= L->n) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
            p++;
            continue;
        }
        if (c == '\'') {
            p++;
            while (p < L->n && L->src[p] != '\'') p++;
            if (p >= L->n) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
            p++;
        } else if (c == '"') {
            p = skipquote(L, p);
            if (p < 0) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
        } else if (c == '\\') {
            if (p + 1 >= L->n) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
            p += 2;
        } else if (c == '`' || (c == '$' && p + 1 < L->n && (L->src[p + 1] == '(' || L->src[p + 1] == '{'))) {
            p = skipnested(L, p);
            if (p < 0) { L->incomplete = 1; L->pos = L->n; return t->type = T_EOF; }
        } else p++;
    }
    t->text = astr(L->ar, L->src + L->pos, p - L->pos);
    t->len = p - L->pos;
    L->pos = p;
    t->end = p;
    return t->type = T_WORD;
}

static Tok *peek(Lex *L)
{
    if (!L->have) { lex_(L, &L->peek); L->have = 1; }
    return &L->peek;
}

static Tok next(Lex *L)
{
    Tok t = *peek(L);
    L->have = 0;
    return t;
}

/* An unquoted reserved word in a command's place? */
static int isword(Tok *t, const char *w)
{
    return t->type == T_WORD && t->len == (int)strlen(w) && !memcmp(t->text, w, t->len);
}

static int isstop(Tok *t)
{
    static const char *const stops[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}", NULL };
    int i;
    if (t->type == T_EOF || t->type == T_RP || t->type == T_DSEMI || t->type == T_SEMIAMP || t->type == T_DSEMIAMP) return 1;
    for (i = 0; stops[i]; i++) if (isword(t, stops[i])) return 1;
    return 0;
}

/* ---- the tree --------------------------------------------------------------- */

enum { N_SIMPLE, N_LIST, N_AND, N_OR, N_NOT, N_PIPE, N_BG, N_IF, N_WHILE, N_UNTIL, N_FOR,
       N_CASE, N_GROUP, N_SUB, N_FUNC, N_ARITH, N_CFOR, N_COND, N_SELECT };

/* end: 0 ;; (stop), 1 ;& (the next body runs too), 2 ;;& (the next patterns are tried) */
typedef struct Case { char **pats; int npats; struct Node *body; struct Case *next; int end; } Case;

typedef struct Node {
    int type;
    struct Node *a, *b, *c;
    char **words;
    int nwords;
    char **assigns;
    int nassigns;
    Redir *redirs;
    char *name;
    Case *cases;
    int haslist;
    char *src;                  /* a function's text; a pipe stage's (for a csh of its own) */
} Node;

static Node *mk(Lex *L, int type, Node *a, Node *b)
{
    Node *n = aalloc(L->ar, sizeof *n);
    if (!n) { L->error = 1; return NULL; }
    n->type = type; n->a = a; n->b = b;
    return n;
}

static void fail(Lex *L, Tok *t)
{
    if (L->error || L->incomplete) return;
    if (t->type == T_EOF) { L->incomplete = 1; return; }
    L->error = 1;
    {
        char what[40];
        int n = t->type == T_WORD ? (t->len < 30 ? t->len : 30) : 0;
        if (n) memcpy(what, t->text, n);
        else {
            static const char *const names[] = { "end", "word", "newline", ";", ";;", "&", "&&", "||", "|", "(", ")", "redirection", ";&", ";;&" };
            strcpy(what, names[t->type]); n = strlen(what);
        }
        what[n] = 0;
        sh_error("syntax error near", what);
    }
}

static int expect(Lex *L, const char *w)
{
    Tok *t = peek(L);
    if (isword(t, w)) { next(L); return 1; }
    fail(L, t);
    return 0;
}

static void skipnl(Lex *L) { while (peek(L)->type == T_NL) next(L); }

static Node *parse_list(Lex *L);
static Node *parse_andor(Lex *L);
static Node *parse_command(Lex *L);

static char **growv(Lex *L, char **v, int n, char *s)
{
    char **nv;
    if ((n & 7) == 0) {
        nv = aalloc(L->ar, (n + 8) * sizeof *nv);
        if (!nv) { L->error = 1; return v; }
        if (v) memcpy(nv, v, n * sizeof *nv);
        v = nv;
    }
    v[n] = s;
    return v;
}

/* name=, name+=, name[sub]= : where the = is (0 = not an assignment) */
static int isassign(const char *w, int n)
{
    int i;
    if (!n || !((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= 'A' && w[0] <= 'Z') || w[0] == '_')) return 0;
    for (i = 1; i < n && wordch(w[i]); i++) ;
    if (i < n && w[i] == '[') {
        int d = 0;
        for (; i < n; i++) { if (w[i] == '[') d++; else if (w[i] == ']' && --d == 0) break; }
        if (i >= n) return 0;
        i++;
    }
    if (i + 1 < n && w[i] == '+' && w[i + 1] == '=') return i + 1;
    return i < n && w[i] == '=' ? i : 0;
}

static Redir *parse_redir(Lex *L)
{
    Tok t = next(L);
    Redir *r = aalloc(L->ar, sizeof *r);
    if (!r) { L->error = 1; return NULL; }
    r->op = t.op;
    if (t.op == R_ERROUT || t.op == R_OUTERR) return r;
    {
        Tok w = next(L);
        if (w.type != T_WORD) { fail(L, &w); return NULL; }
        r->word = w.text;
    }
    if (r->op == R_HERE) {
        /* the delimiter: its quotes off; any quote means no expansion */
        char *s = r->word, *d = r->word;
        r->striptabs = t.len == 1;
        for (; *s; s++) {
            if (*s == '\'' || *s == '"' || *s == '\\') { r->quoted = 1; if (*s == '\\' && s[1]) *d++ = *++s; continue; }
            *d++ = *s;
        }
        *d = 0;
        if (L->nhd < 16) L->hd[L->nhd++] = r;
        else { L->error = 1; sh_error("syntax error:", "more than 16 here-documents on a line"); return NULL; }
    }
    return r;
}

static Redir *addredir(Redir *list, Redir *r)
{
    Redir **p = &list;
    if (!r) return list;
    while (*p) p = &(*p)->next;
    *p = r;
    return list;
}

static Node *parse_simple(Lex *L)
{
    Node *n = mk(L, N_SIMPLE, NULL, NULL);
    if (!n) return NULL;
    for (;;) {
        Tok *t = peek(L);
        if (t->type == T_REDIR) { n->redirs = addredir(n->redirs, parse_redir(L)); if (L->error) return NULL; continue; }
        if (t->type != T_WORD) break;
        if (!n->nwords && isassign(t->text, t->len)) { n->assigns = growv(L, n->assigns, n->nassigns++, next(L).text); continue; }
        n->words = growv(L, n->words, n->nwords++, next(L).text);
    }
    if (!n->nwords && !n->nassigns && !n->redirs) { fail(L, peek(L)); return NULL; }
    return n;
}

static Node *parse_compound_redirs(Lex *L, Node *n)
{
    while (n && peek(L)->type == T_REDIR) n->redirs = addredir(n->redirs, parse_redir(L));
    return n;
}

static Node *parse_if(Lex *L)
{
    Node *n = mk(L, N_IF, NULL, NULL);
    if (!n) return NULL;
    n->a = parse_list(L);
    if (!expect(L, "then")) return NULL;
    n->b = parse_list(L);
    if (isword(peek(L), "elif")) { next(L); n->c = parse_if(L); return n; }
    if (isword(peek(L), "else")) { next(L); n->c = parse_list(L); }
    if (!expect(L, "fi")) return NULL;
    return n;
}

/* "()" right after a word (spaces allowed): a function's name */
static int funcparens(Lex *L, long p)
{
    while (p < L->n && isspace_(L->src[p])) p++;
    if (p >= L->n || L->src[p] != '(') return 0;
    p++;
    while (p < L->n && isspace_(L->src[p])) p++;
    return p < L->n && L->src[p] == ')';
}

/* (( ... )) from p (just after the two): the position of the first of the
 * closing two, -1 when it is not arithmetic (( (a); b )), -2 not ended yet. */
static long scan_arith(Lex *L, long p)
{
    int d = 0;
    for (; p < L->n; p++) {
        char c = L->src[p];
        if (c == '(') d++;
        else if (c == ')') {
            if (!d) return p + 1 < L->n ? (L->src[p + 1] == ')' ? p : -1) : -2;
            d--;
        } else if (c == '\'') { while (++p < L->n && L->src[p] != '\'') ; }
        else if (c == '"') { p = skipquote(L, p) - 1; if (p < 0) return -2; }
    }
    return -2;
}

static int arithstart(Lex *L, Tok *t) { return t->type == T_LP && t->start + 1 < L->n && L->src[t->start + 1] == '('; }

/* the body of for/while/select: do ... done (or { ... }) */
static Node *parse_dobody(Lex *L, Node *n)
{
    if (peek(L)->type == T_SEMI || peek(L)->type == T_NL) next(L);
    skipnl(L);
    if (isword(peek(L), "{")) {
        next(L);
        n->b = parse_list(L);
        if (!expect(L, "}")) return NULL;
        return parse_compound_redirs(L, n);
    }
    if (!expect(L, "do")) return NULL;
    n->b = parse_list(L);
    if (!expect(L, "done")) return NULL;
    return parse_compound_redirs(L, n);
}

/* [[ ... ]]: its words and operators, as written (expanded when run) */
static Node *parse_cond(Lex *L)
{
    Node *n = mk(L, N_COND, NULL, NULL);
    if (!n) return NULL;
    for (;;) {
        Tok *q = peek(L);
        char *w;
        if (L->error || L->incomplete) return NULL;
        if (isword(q, "]]")) { next(L); break; }
        if (q->type == T_EOF) { fail(L, q); return NULL; }
        if (q->type == T_NL) { next(L); continue; }
        if (n->nwords && !strcmp(n->words[n->nwords - 1], "=~") && q->type != T_EOF) {
            /* the regex: up to a blank outside ( ) and quotes, its | ( ) its own */
            long a = q->start, e = a;
            int d = 0;
            while (e < L->n) {
                char c = L->src[e];
                if (c == '\\') { e += 2; continue; }
                if (c == '\'') { while (++e < L->n && L->src[e] != '\'') ; e++; continue; }
                if (c == '"') { e = skipquote(L, e); if (e < 0) { L->incomplete = 1; return NULL; } continue; }
                if (c == '(') d++;
                else if (c == ')') { if (!d) break; d--; }
                else if ((c == ' ' || c == '\t' || c == '\n') && !d) break;
                e++;
            }
            n->words = growv(L, n->words, n->nwords++, astr(L->ar, L->src + a, e - a));
            L->pos = e; L->have = 0;
            continue;
        }
        if (q->type == T_WORD) w = q->text;
        else if (q->type == T_AND) w = "&&";
        else if (q->type == T_OR) w = "||";
        else if (q->type == T_LP) w = "(";
        else if (q->type == T_RP) w = ")";
        else if (q->type == T_REDIR && q->op == R_IN) w = "<";
        else if (q->type == T_REDIR && q->op == R_OUT) w = ">";
        else { fail(L, q); return NULL; }
        next(L);
        n->words = growv(L, n->words, n->nwords++, w);
    }
    if (!n->nwords) { L->error = 1; sh_error("syntax error:", "[[ ]] with nothing inside"); return NULL; }
    return parse_compound_redirs(L, n);
}

static Node *parse_command(Lex *L)
{
    Tok *t = peek(L);
    Node *n;
    if (L->error || L->incomplete) return NULL;
    if (arithstart(L, t)) {             /* (( expression )) */
        long e = scan_arith(L, t->start + 2);
        if (e == -2) { L->incomplete = 1; return NULL; }
        if (e >= 0) {
            n = mk(L, N_ARITH, NULL, NULL);
            if (!n) return NULL;
            n->name = astr(L->ar, L->src + t->start + 2, e - t->start - 2);
            L->pos = e + 2; L->have = 0;
            return parse_compound_redirs(L, n);
        }
    }
    if (isword(t, "[[")) { next(L); return parse_cond(L); }
    if (t->type == T_LP) {
        next(L);
        n = mk(L, N_SUB, parse_list(L), NULL);
        if (peek(L)->type != T_RP) { fail(L, peek(L)); return NULL; }
        next(L);
        return parse_compound_redirs(L, n);
    }
    if (t->type != T_WORD && t->type != T_REDIR) { fail(L, t); return NULL; }
    if (isstop(t)) { L->error = 1; sh_error("syntax error near", t->text); return NULL; }   /* fi, done... alone */
    if (isword(t, "if")) { next(L); return parse_compound_redirs(L, parse_if(L)); }
    if (isword(t, "while") || isword(t, "until")) {
        int until = isword(t, "until");
        next(L);
        n = mk(L, until ? N_UNTIL : N_WHILE, parse_list(L), NULL);
        if (!expect(L, "do")) return NULL;
        n->b = parse_list(L);
        if (!expect(L, "done")) return NULL;
        return parse_compound_redirs(L, n);
    }
    if (isword(t, "for") || isword(t, "select")) {
        Tok v;
        int sel = isword(t, "select");
        next(L);
        if (!sel && arithstart(L, peek(L))) {     /* for ((init; cond; step)) */
            long a = peek(L)->start + 2, e = scan_arith(L, a), k, from = a;
            int part = 0, d = 0;
            if (e == -2) { L->incomplete = 1; return NULL; }
            if (e < 0) { fail(L, peek(L)); return NULL; }
            n = mk(L, N_CFOR, NULL, NULL);
            if (!n) return NULL;
            for (k = a; k <= e; k++) {
                char c = k < e ? L->src[k] : ';';
                if (c == '(') d++; else if (c == ')') d--;
                if (c == ';' && !d) {
                    if (part < 3) n->words = growv(L, n->words, n->nwords++, astr(L->ar, L->src + from, k - from));
                    part++;
                    from = k + 1;
                }
            }
            if (part != 3) { L->error = 1; sh_error("syntax error:", "for (( init; condition; step ))"); return NULL; }
            L->pos = e + 2; L->have = 0;
            return parse_dobody(L, n);
        }
        v = next(L);
        if (v.type != T_WORD) { fail(L, &v); return NULL; }
        n = mk(L, sel ? N_SELECT : N_FOR, NULL, NULL);
        n->name = v.text;
        if (peek(L)->type == T_NL) skipnl(L);
        if (isword(peek(L), "in")) {
            next(L);
            n->haslist = 1;
            while (peek(L)->type == T_WORD) n->words = growv(L, n->words, n->nwords++, next(L).text);
        }
        return parse_dobody(L, n);
    }
    if (isword(t, "case")) {
        Tok w;
        Case **cp;
        next(L);
        w = next(L);
        if (w.type != T_WORD) { fail(L, &w); return NULL; }
        n = mk(L, N_CASE, NULL, NULL);
        n->name = w.text;
        skipnl(L);
        if (!expect(L, "in")) return NULL;
        skipnl(L);
        cp = &n->cases;
        while (!isword(peek(L), "esac")) {
            Case *c = aalloc(L->ar, sizeof *c);
            if (!c) { L->error = 1; return NULL; }
            if (peek(L)->type == T_LP) next(L);
            for (;;) {
                Tok p = next(L);
                if (p.type != T_WORD) { fail(L, &p); return NULL; }
                c->pats = growv(L, c->pats, c->npats++, p.text);
                if (peek(L)->type == T_PIPE) { next(L); continue; }
                break;
            }
            if (peek(L)->type != T_RP) { fail(L, peek(L)); return NULL; }
            next(L);
            c->body = parse_list(L);
            if (L->error || L->incomplete) return NULL;
            *cp = c; cp = &c->next;
            if (peek(L)->type == T_DSEMI || peek(L)->type == T_SEMIAMP || peek(L)->type == T_DSEMIAMP) {
                c->end = peek(L)->type == T_SEMIAMP ? 1 : peek(L)->type == T_DSEMIAMP ? 2 : 0;
                next(L); skipnl(L); continue;
            }
            if (!isword(peek(L), "esac")) { fail(L, peek(L)); return NULL; }
        }
        next(L);
        return parse_compound_redirs(L, n);
    }
    if (isword(t, "{")) {
        next(L);
        n = mk(L, N_GROUP, parse_list(L), NULL);
        if (!expect(L, "}")) return NULL;
        return parse_compound_redirs(L, n);
    }
    if (t->type == T_WORD && (isword(t, "function") || (!isassign(t->text, t->len) && funcparens(L, t->end)))) {
        /* name() compound, or function name [()] compound */
        Tok name = next(L);
        long bodystart;
        if (isword(&name, "function")) {
            name = next(L);
            if (name.type != T_WORD) { fail(L, &name); return NULL; }
            if (funcparens(L, name.end)) { next(L); next(L); }
        } else { next(L); next(L); }               /* ( ) */
        skipnl(L);
        bodystart = peek(L)->start;
        n = mk(L, N_FUNC, NULL, NULL);
        if (!n) return NULL;
        n->name = name.text;
        n->a = parse_command(L);
        if (!n->a) return NULL;
        n->src = astr(L->ar, L->src + bodystart, (L->have ? L->peek.start : L->pos) - bodystart);
        return n;
    }
    return parse_simple(L);
}

static Node *parse_stage(Lex *L)
{
    long a = peek(L)->start;
    Node *n = parse_command(L);
    if (n && !n->src) n->src = astr(L->ar, L->src + a, (L->have ? L->peek.start : L->pos) - a);
    return n;
}

static Node *parse_pipeline(Lex *L)
{
    int bang = 0;
    Node *n;
    if (isword(peek(L), "!")) { next(L); bang = 1; }
    n = parse_stage(L);
    while (n && peek(L)->type == T_PIPE) {
        int err = peek(L)->op;          /* |& : the error stream into the pipe too */
        next(L);
        skipnl(L);
        n = mk(L, N_PIPE, n, parse_stage(L));
        if (n) n->haslist = err;
        if (n && !n->b) return NULL;
    }
    if (n && bang) n = mk(L, N_NOT, n, NULL);
    return n;
}

static Node *parse_andor(Lex *L)
{
    Node *n = parse_pipeline(L);
    while (n && (peek(L)->type == T_AND || peek(L)->type == T_OR)) {
        int type = next(L).type == T_AND ? N_AND : N_OR;
        skipnl(L);
        n = mk(L, type, n, parse_pipeline(L));
        if (n && !n->b) return NULL;
    }
    return n;
}

/* Commands up to a stop word (then, do, fi...), ")" or ";;" or the end. */
static Node *parse_list(Lex *L)
{
    Node *list = NULL;
    skipnl(L);
    while (!isstop(peek(L)) && !L->error && !L->incomplete) {
        Node *n = parse_andor(L);
        int t;
        if (!n) return NULL;
        t = peek(L)->type;
        if (t == T_AMP) { next(L); n = mk(L, N_BG, n, NULL); }
        else if (t == T_SEMI || t == T_NL) next(L);
        else if (!isstop(peek(L))) { fail(L, peek(L)); return NULL; }
        list = list ? mk(L, N_LIST, list, n) : n;
        skipnl(L);
    }
    if (!list && !L->error) {
        /* "then fi" with nothing between: bash says no; csh lets it be empty */
        list = mk(L, N_GROUP, NULL, NULL);
    }
    return list;
}

/* One line's commands: up to a newline (or the end) at the top. */
static Node *parse_line(Lex *L)
{
    Node *list = NULL;
    while (peek(L)->type == T_NL) next(L);
    while (peek(L)->type != T_NL && peek(L)->type != T_EOF && !L->error && !L->incomplete) {
        Node *n = parse_andor(L);
        int t;
        if (!n) return NULL;
        t = peek(L)->type;
        if (t == T_AMP) { next(L); n = mk(L, N_BG, n, NULL); }
        else if (t == T_SEMI) next(L);
        else if (t != T_NL && t != T_EOF) { fail(L, peek(L)); return NULL; }
        list = list ? mk(L, N_LIST, list, n) : n;
    }
    if (peek(L)->type == T_NL) next(L);      /* here-documents read with it */
    return list;
}

/* ---- running: state ----------------------------------------------------------- */

int sh_opt_e, sh_opt_x, sh_opt_u, sh_opt_f, sh_opt_pipefail, sh_opt_histexp = 1;
int sh_break, sh_continue, sh_return, sh_exiting, sh_loops;
long sh_exitrc;
static long rc;
static int aborted;             /* Ctrl-C: everything stops back to the prompt */
static int incond;              /* in an if/while condition, or left of && || ! (set -e looks away) */
static int depth;               /* $(...) and functions inside each other */
static int sourcing;            /* inside a script (set -e counts there) */
static char **pos;              /* $1 ... */
static int npos;
static char *pos0 = "csh";

/* busy: calls of it running; dead: replaced while running, freed when the
 * last call ends (f() { f() { ...; }; ...; } redefines itself) */
typedef struct Func { struct Func *next; char *name, *src; Node *body; Arena ar; int busy, dead; } Func;

/* local: the values a function's call put aside, given back at its end */
typedef struct Saved { struct Saved *next; char *name, *val; int had; struct Arr *arr; } Saved;
static Saved **frame;
struct Arr;
static struct Arr *arr_unlink(const char *name);
static void arr_relink(struct Arr *a);
static void arr_remove(const char *name);

int sh_local(const char *name, const char *val)
{
    Saved *s;
    if (!frame) return -1;
    for (s = *frame; s; s = s->next) if (!strcmp(s->name, name)) break;
    if (!s && (s = os_alloc(sizeof *s + strlen(name) + 1))) {
        char *v = os_alloc(LINE_MAX);
        s->name = (char *)(s + 1);
        strcpy(s->name, name);
        int l;
        if (v && (l = os_getlocal(name, v, LINE_MAX)) >= 0) { v[l < LINE_MAX ? l : LINE_MAX - 1] = 0; s->had = 1; s->val = v; }
        else if (v) os_free(v);
        s->arr = arr_unlink(name);      /* an array of that name: put aside too */
        s->next = *frame;
        *frame = s;
    }
    if (val) os_setvar(name, val, V_LOCAL); else os_unsetvar(name, 0);
    return 0;
}

static void unframe(Saved *s)
{
    while (s) {
        Saved *n = s->next;
        arr_remove(s->name);
        if (s->arr) arr_relink(s->arr);
        if (s->had) os_setvar(s->name, s->val, V_LOCAL); else os_unsetvar(s->name, 0);
        if (s->val) os_free(s->val);
        os_free(s);
        s = n;
    }
}
static Func *funcs;

long sh_rc(void) { return rc; }
int sh_npos(void) { return npos; }
int sh_getpos(int i, char **s) { if (i < 1 || i > npos) return 0; *s = pos[i - 1]; return 1; }

/* set -- a b c / shift: the new list lives here. A function's call and a
 * sourced script have their own (put aside and back around them), so a
 * shift inside one never frees the caller's $1... */
static Arena posar;

void sh_setpos(int argc, char **argv)
{
    Arena old = posar;
    char **v;
    int i;
    posar.head = NULL;
    v = aalloc(&posar, (argc + 1) * sizeof *v);
    for (i = 0; v && i < argc; i++) v[i] = astr(&posar, argv[i], strlen(argv[i]));
    pos = v; npos = v ? argc : 0;
    afree(&old);
}

static char *hookbuf;
static int hookn, hookcap, hooking;

void sh_printn(const char *s, int n)
{
    if (hooking) {
        if (hookn + n + 1 > hookcap) {
            int cap = (hookn + n + 1) * 2;
            char *p = os_alloc(cap);
            if (!p) return;
            if (hookbuf) { memcpy(p, hookbuf, hookn); os_free(hookbuf); }
            hookbuf = p; hookcap = cap;
        }
        memcpy(hookbuf + hookn, s, n);
        hookn += n;
        hookbuf[hookn] = 0;
        return;
    }
    os_write(s, n);
}
void sh_print(const char *s) { sh_printn(s, strlen(s)); }

/* printf -v: what is printed goes into a string (os_alloc'ed) instead */
void sh_hook(int on) { if (on) { hooking = 1; hookn = 0; if (hookbuf) hookbuf[0] = 0; } else hooking = 0; }
char *sh_hooked(void)
{
    char *t = os_alloc(hookn + 1);
    if (t && hookbuf) memcpy(t, hookbuf, hookn);
    hooking = 0;
    return t;
}

/* "csh: a b" on the error stream (not into a pipe, a > file or a $( )) */
void sh_error(const char *a, const char *b)
{
    char t[300];
    int n = 0, l;
    l = strlen(a); if (l > 200) l = 200;
    memcpy(t, "csh: ", 5); n = 5;
    memcpy(t + n, a, l); n += l;
    if (b && *b) {
        l = strlen(b); if (l > (int)sizeof t - n - 2) l = sizeof t - n - 2;
        t[n++] = ' ';
        memcpy(t + n, b, l); n += l;
    }
    t[n++] = '\n';
    os_err(t, n);
}

static void setrc(long r)
{
    rc = r;
    os_setrc(r);
}

static long exec(Node *n);
static long run_text(const char *text, int len);

/* ---- patterns --------------------------------------------------------------- */

static int lc(int c)
{
    c &= 0xff;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c += 32;
    return c;
}

/* One pattern element at *pp (? [..] \x or a character) against *sp:
 * both moved on when it matches. */
static int gone(const char **pp, const char **sp, int nocase)
{
    const char *p = *pp, *s = *sp;
    if (*p == '?') { *pp = p + 1; *sp = s + 1; return 1; }
    if (*p == '[') {
        const char *q = p + 1;
        int neg = 0, ok = 0;
        if (*q == '!' || *q == '^') { neg = 1; q++; }
        while (*q && (*q != ']' || q == p + 1 + neg)) {
            if (q[1] == '-' && q[2] && q[2] != ']') {
                int c = nocase ? lc(*s) : (UBYTE_)*s, lo = nocase ? lc(q[0]) : (UBYTE_)q[0], hi = nocase ? lc(q[2]) : (UBYTE_)q[2];
                if (c >= lo && c <= hi) ok = 1;
                q += 3;
            } else { if (nocase ? lc(*q) == lc(*s) : *q == *s) ok = 1; q++; }
        }
        if (*q) {
            if (ok == neg) return 0;
            *pp = q + 1; *sp = s + 1;
            return 1;
        }
        /* no ']': a plain [ */
    }
    if (*p == '\\' && p[1]) p++;
    if (nocase ? lc(*p) != lc(*s) : *p != *s) return 0;
    *pp = p + 1; *sp = s + 1;
    return 1;
}

/* sh pattern: * ? [...] [!...] \x. Only the last * is ever gone back to,
 * so many stars cost no more than one (no backtracking blow-up). */
int gmatch(const char *p, const char *s, int nocase)
{
    const char *star = NULL, *from = NULL;
    while (*s) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            star = p; from = s;
            continue;
        }
        if (*p && gone(&p, &s, nocase)) continue;
        if (!star) return 0;
        p = star; s = ++from;
    }
    while (*p == '*') p++;
    return !*p;
}

/* ---- variables: csh's arrays and read-only names, over the Amiga's ------------- */

/* An array lives in csh (AmigaDOS variables are plain strings): indexed
 * (a sparse list kept in order, a[-1] the last) or associative (declare -A,
 * in the order the keys came). $a alone is a[0], as in bash. */
typedef struct AEnt { struct AEnt *next; long idx; char *key, *val; } AEnt;
typedef struct Arr { struct Arr *next; int assoc, n; AEnt *head; char *name; } Arr;
static Arr *arrs;
/* a name's attributes (declare -r -i -l -u): read-only, an integer (what
 * is put in is worked out as $(( )) does), lower or upper case */
#define AT_RO  1
#define AT_INT 2
#define AT_LOW 4
#define AT_UP  8
typedef struct RO { struct RO *next; int at; char name[1]; } RO;
static RO *ros;

static int veq(const char *a, const char *b)       /* names: any case, as the Amiga's */
{
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return !*a && !*b;
}

static Arr *arr_make(const char *name, int assoc);

static Arr *arr_find(const char *name)
{
    Arr *a;
    for (a = arrs; a; a = a->next) if (veq(a->name, name)) return a;
    return NULL;
}

int sh_isarray(const char *name) { Arr *a = arr_find(name); return a ? 1 + a->assoc : 0; }

static AEnt *ent_new(long idx, const char *key, const char *val)
{
    int kl = key ? strlen(key) : 0, vl = strlen(val);
    AEnt *e = os_alloc(sizeof *e + kl + vl + 2);
    if (!e) return NULL;
    e->idx = idx;
    e->key = (char *)(e + 1);
    memcpy(e->key, key ? key : "", kl);
    e->val = e->key + kl + 1;
    memcpy(e->val, val, vl);
    return e;
}

static void arr_drop(Arr *a)
{
    AEnt *e, *n;
    for (e = a->head; e; e = n) { n = e->next; os_free(e); }
    os_free(a);
}

static Arr *arr_unlink(const char *name)
{
    Arr **p;
    for (p = &arrs; *p; p = &(*p)->next)
        if (veq((*p)->name, name)) { Arr *a = *p; *p = a->next; return a; }
    return NULL;
}

static void arr_relink(Arr *a) { a->next = arrs; arrs = a; }

static void arr_remove(const char *name)
{
    Arr **p;
    for (p = &arrs; *p; p = &(*p)->next)
        if (veq((*p)->name, name)) { Arr *a = *p; *p = a->next; arr_drop(a); return; }
}

/* An array of csh's own (PIPESTATUS, BASH_REMATCH) emptied for new values:
 * the one there reused, no Amiga variable looked at (a pipe's cost) */
static Arr *arr_fresh(const char *name)
{
    Arr *a;
    AEnt *e, *n;
    for (a = arrs; a; a = a->next) if (!strcmp(a->name, name)) break;
    if (!a || a->assoc) return arr_make(name, 0);
    for (e = a->head; e; e = n) { n = e->next; os_free(e); }
    a->head = NULL;
    a->n = 0;
    return a;
}

static Arr *arr_make(const char *name, int assoc)
{
    Arr *a;
    int l = strlen(name);
    arr_remove(name);
    os_unsetvar(name, 0);
    if (!(a = os_alloc(sizeof *a + l + 1))) return NULL;
    a->name = (char *)(a + 1);
    memcpy(a->name, name, l);
    a->assoc = assoc;
    a->next = arrs;
    arrs = a;
    return a;
}

static long arr_last(Arr *a)
{
    AEnt *e;
    long last = -1;
    for (e = a->head; e; e = e->next) last = e->idx;
    return last;
}

/* An indexed array's subscript: arithmetic, below 0 counted from the end. */
static int arr_index(Arr *a, const char *key, long *idx)
{
    int err = 0;
    long i = *key ? arith(key, &err) : 0;
    if (err) { sh_error("bad array subscript:", key); return 0; }
    if (i < 0) i += arr_last(a) + 1;
    if (i < 0) { sh_error("bad array subscript:", key); return 0; }
    *idx = i;
    return 1;
}

static void arr_put(Arr *a, const char *key, const char *val)
{
    long idx = 0;
    AEnt **pp, *e;
    if (!a->assoc && !arr_index(a, key, &idx)) return;
    for (pp = &a->head; *pp; pp = &(*pp)->next) {
        if (a->assoc ? !strcmp((*pp)->key, key) : (*pp)->idx == idx) {
            if (!(e = ent_new(idx, a->assoc ? key : NULL, val))) return;
            e->next = (*pp)->next;
            os_free(*pp);
            *pp = e;
            return;
        }
        if (!a->assoc && (*pp)->idx > idx) break;
    }
    if (!(e = ent_new(idx, a->assoc ? key : NULL, val))) return;
    e->next = *pp;
    *pp = e;
    a->n++;
}

static void arr_push(Arr *a, const char *val)
{
    AEnt **pp, *e;
    long idx = arr_last(a) + 1;
    for (pp = &a->head; *pp; pp = &(*pp)->next) ;
    if (!(e = ent_new(idx, NULL, val))) return;
    *pp = e;
    a->n++;
}

static AEnt *arr_at(Arr *a, const char *key)
{
    AEnt *e;
    long idx = 0;
    if (!a->assoc) {
        int err = 0;
        idx = *key ? arith(key, &err) : 0;
        if (err) return NULL;
        if (idx < 0) idx += arr_last(a) + 1;
    }
    for (e = a->head; e; e = e->next) if (a->assoc ? !strcmp(e->key, key) : e->idx == idx) return e;
    return NULL;
}

static void arr_del(Arr *a, const char *key)
{
    AEnt **pp, *e = arr_at(a, key);
    if (!e) return;
    for (pp = &a->head; *pp != e; pp = &(*pp)->next) ;
    *pp = e->next;
    os_free(e);
    a->n--;
}

static int attr_of(const char *name)
{
    RO *r;
    for (r = ros; r; r = r->next) if (veq(r->name, name)) return r->at;
    return 0;
}

static int ro_has(const char *name) { return attr_of(name) & AT_RO; }

/* on: attributes given, off: taken away (-l takes -u away and the other way) */
static void attr_set(const char *name, int on, int off)
{
    RO *r;
    for (r = ros; r; r = r->next) if (veq(r->name, name)) break;
    if (!r) {
        if (!on || !(r = os_alloc(sizeof *r + strlen(name)))) return;
        strcpy(r->name, name);
        r->next = ros;
        ros = r;
    }
    r->at = (r->at & ~off) | on;
}

void sh_readonly(const char *name) { attr_set(name, AT_RO, 0); }

long arith(const char *expr, int *err);

/* A value as the name's attributes want it: an integer worked out, the case
 * changed; in buf (or val itself). NULL: not a number (said). */
static const char *attr_val(const char *name, const char *val, char *buf, int size)
{
    int at = ros ? attr_of(name) : 0, i;
    if (!(at & (AT_INT | AT_LOW | AT_UP))) return val;
    if (at & AT_INT) {
        int err = 0;
        long v = *val ? arith(val, &err) : 0;
        if (err) { if (err != 3) sh_error("not a number:", val); return NULL; }
        ltoa_(v, buf);
        return buf;
    }
    for (i = 0; val[i] && i < size - 1; i++) {
        int c = (unsigned char)val[i];
        if (at & AT_LOW) { if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c += 32; }
        else if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) c -= 32;
        buf[i] = c;
    }
    buf[i] = 0;
    return buf;
}

static int ro_refuse(const char *name)
{
    if (!ros || !ro_has(name)) return 0;
    sh_error("readonly variable:", name);
    return 1;
}

static long t0;                 /* $SECONDS counts from here */
static unsigned long seed;

/* A variable's value: $RANDOM, $SECONDS, an array's a[0], else the Amiga
 * variable. Length, -1 = not set. */
int sh_get(const char *name, char *buf, int size)
{
    Arr *a;
    if (name[0] == 'R' && !strcmp(name, "RANDOM") && size >= 8) {
        if (!seed) seed = (unsigned long)os_ticks() * 2654435761UL + 1;
        seed = seed * 1103515245UL + 12345;
        return ltoa_((long)((seed >> 16) & 0x7fff), buf);
    }
    if (name[0] == 'S' && !strcmp(name, "SECONDS") && size >= 12) {
        if (!t0) t0 = os_ticks() | 1;
        return ltoa_((long)((unsigned long)(os_ticks() - t0) / 50), buf);
    }
    a = arrs ? arr_find(name) : NULL;
    if (a) {
        AEnt *e = arr_at(a, "0");
        int l;
        if (!e) return -1;
        l = strlen(e->val);
        if (l >= size) l = size - 1;
        memcpy(buf, e->val, l); buf[l] = 0;
        return l;
    }
    return os_getvar(name, buf, size);
}

/* Every assignment of the language: refused for a read-only name (said, 0). */
int sh_set(const char *name, const char *val, int how)
{
    Arr *a;
    char ab[LINE_MAX];
    if (ro_refuse(name)) return 0;
    if (!(val = attr_val(name, val, ab, sizeof ab))) return 0;
    if (arrs && (a = arr_find(name)) && how != V_EXPORT) { arr_put(a, a->assoc ? "0" : "0", val); return 1; }
    os_setvar(name, val, how);
    return 1;
}

static int elem_get(const char *name, const char *key, char *buf, int size)
{
    Arr *a = arr_find(name);
    AEnt *e;
    int l;
    if (!a) {                    /* x[0] of a plain variable is the variable */
        int err = 0;
        long i = *key ? arith(key, &err) : 0;
        return !err && i == 0 ? os_getvar(name, buf, size) : -1;
    }
    if (!(e = arr_at(a, key))) return -1;
    l = strlen(e->val);
    if (l >= size) l = size - 1;
    memcpy(buf, e->val, l); buf[l] = 0;
    return l;
}

static int elem_set(const char *name, const char *key, const char *val)
{
    Arr *a = arr_find(name);
    char ab[LINE_MAX];
    if (ro_refuse(name)) return 0;
    if (!(val = attr_val(name, val, ab, sizeof ab))) return 0;
    if (!a) {                    /* a[1]=x makes a an array; a plain value becomes a[0] */
        char old[LINE_MAX];
        int l = os_getvar(name, old, sizeof old);
        if (!(a = arr_make(name, 0))) return 0;
        if (l >= 0) arr_put(a, "0", old);
    }
    arr_put(a, key, val);
    return 1;
}

/* unset name, unset 'name[key]' */
int sh_unset(const char *name)
{
    const char *br = strchr(name, '[');
    if (br && name[strlen(name) - 1] == ']') {
        char nm[128], key[256];
        int nl = br - name, kl = strlen(br + 1) - 1;
        Arr *a;
        if (nl >= (int)sizeof nm || kl >= (int)sizeof key) return 0;
        memcpy(nm, name, nl); nm[nl] = 0;
        memcpy(key, br + 1, kl); key[kl] = 0;
        if (ro_refuse(nm)) return 0;
        if ((a = arr_find(nm))) arr_del(a, key);
        return 1;
    }
    if (ro_refuse(name)) return 0;
    arr_remove(name);
    os_unsetvar(name, 1);
    return 1;
}

/* ---- arithmetic: $(( )), (( )), let ----------------------------------------------- */

/* bash's operators and their order: , = op= ?: || && | ^ & == != < <= > >=
 * << >> + - * / % ** unary(! ~ - + ++ --) postfix(++ --). Where a side is
 * not taken (0 && x, c ? a : b) its assignments are not made (noeval). */
typedef struct { const char *p; int err, depth, noeval; } Ar;

static long a_comma(Ar *a);
static long a_assign(Ar *a);
static long a_unary(Ar *a);

static void a_sp(Ar *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }

typedef struct { char name[64], key[64]; int haskey; } LVal;

/* name or name[subscript] */
static void a_lval(Ar *a, LVal *v)
{
    int n = 0;
    while (wordch(*a->p) && n < 63) v->name[n++] = *a->p++;
    v->name[n] = 0;
    v->haskey = 0;
    if (*a->p == '[') {
        const char *s = ++a->p;
        int d = 1, kl;
        char raw[64];
        while (*a->p && !(*a->p == ']' && --d == 0)) { if (*a->p == '[') d++; a->p++; }
        if (*a->p != ']') { a->err = 1; return; }
        kl = a->p - s;
        if (kl > 63) kl = 63;
        memcpy(raw, s, kl); raw[kl] = 0;
        a->p++;
        v->haskey = 1;
        if (sh_isarray(v->name) == 2) strcpy(v->key, raw);
        else {
            int err = 0;
            long i = arith(raw, &err);
            if (err) a->err = err;
            ltoa_(i, v->key);
        }
    }
}

/* A variable's value, itself arithmetic (x=y+1); its own frame, so a_prim's
 * stays small (it calls itself for each ( - ! ~). */
static long __attribute__((noinline)) a_valof(Ar *a, LVal *v)
{
    static int nest;            /* x=x: a variable naming itself */
    char buf[64];
    long r = 0;
    int l = v->haskey ? elem_get(v->name, v->key, buf, sizeof buf) : sh_get(v->name, buf, sizeof buf);
    if (l > 0) {
        int err = 0;
        if (nest >= 8) { a->err = 1; return 0; }
        nest++;
        r = arith(buf, &err);
        nest--;
        if (err) a->err = err;
    } else if (l < 0 && sh_opt_u && !a->noeval) { sh_error("unbound variable:", v->name); a->err = 3; }
    return r;
}

static void a_store(Ar *a, LVal *v, long x)
{
    char b[24];
    if (a->noeval || a->err) return;
    ltoa_(x, b);
    if (!(v->haskey ? elem_set(v->name, v->key, b) : sh_set(v->name, b, V_ASSIGN))) a->err = 3;
}

static long a_div(Ar *a, long x, long y, int mod)
{
    if (!y) { if (!a->noeval) a->err = 2; return 0; }
    return mod ? x % y : x / y;
}

static long a_binop(Ar *a, long x, char op, long y)
{
    switch (op) {
    case '+': return x + y;
    case '-': return x - y;
    case '*': return x * y;
    case '/': return a_div(a, x, y, 0);
    case '%': return a_div(a, x, y, 1);
    case '&': return x & y;
    case '|': return x | y;
    case '^': return x ^ y;
    case '<': return x << y;
    case '>': return x >> y;
    }
    return y;
}

static long a_number(Ar *a)
{
    long v = 0;
    int base = 10;
    if (a->p[0] == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) { base = 16; a->p += 2; }
    else if (a->p[0] == '0' && a->p[1] >= '0' && a->p[1] <= '7') base = 8;
    else {                      /* base#digits: 2#101, 16#ff */
        const char *q = a->p;
        long b = 0;
        while (*q >= '0' && *q <= '9') b = b * 10 + *q++ - '0';
        if (*q == '#' && b >= 2 && b <= 36) { base = b; a->p = q + 1; }
    }
    for (;;) {
        char c = *a->p;
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
        a->p++;
    }
    if (wordch(*a->p)) a->err = 1;      /* 09, 12abc */
    return v;
}

static long a_prim1(Ar *a)
{
    a_sp(a);
    if (*a->p == '(') {
        long v;
        a->p++;
        v = a_comma(a);
        a_sp(a);
        if (*a->p == ')') a->p++; else a->err = 1;
        return v;
    }
    if (*a->p >= '0' && *a->p <= '9') return a_number(a);
    if ((*a->p >= 'a' && *a->p <= 'z') || (*a->p >= 'A' && *a->p <= 'Z') || *a->p == '_') {
        LVal lv;
        char op = 0;
        int ol = 0;
        a_lval(a, &lv);
        if (a->err) return 0;
        a_sp(a);
        if ((a->p[0] == '+' && a->p[1] == '+') || (a->p[0] == '-' && a->p[1] == '-')) {   /* x++ x-- */
            long v = a_valof(a, &lv);
            a_store(a, &lv, a->p[0] == '+' ? v + 1 : v - 1);
            a->p += 2;
            return v;
        }
        if (a->p[0] == '=' && a->p[1] != '=') { op = '='; ol = 1; }
        else if (a->p[1] == '=' && a->p[0] && strchr("+-*/%&|^", a->p[0])) { op = a->p[0]; ol = 2; }
        else if ((a->p[0] == '<' || a->p[0] == '>') && a->p[1] == a->p[0] && a->p[2] == '=') { op = a->p[0]; ol = 3; }
        if (op) {
            long r;
            a->p += ol;
            r = a_assign(a);
            if (op != '=') r = a_binop(a, a_valof(a, &lv), op, r);
            a_store(a, &lv, r);
            return r;
        }
        return a_valof(a, &lv);
    }
    a->err = 1;
    return 0;
}

static long a_unary(Ar *a)
{
    long v;
    if (++a->depth > 100) { a->err = 1; return 0; }   /* ((((... or - - - -...: the stack */
    a_sp(a);
    if ((a->p[0] == '+' && a->p[1] == '+') || (a->p[0] == '-' && a->p[1] == '-')) {   /* ++x --x */
        int inc = a->p[0] == '+' ? 1 : -1;
        const char *q = a->p + 2;
        while (*q == ' ') q++;
        if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_') {
            LVal lv;
            a->p = q;
            a_lval(a, &lv);
            v = a_valof(a, &lv) + inc;
            a_store(a, &lv, v);
            a->depth--;
            return v;
        }
    }
    if (*a->p == '-') { a->p++; v = -a_unary(a); }
    else if (*a->p == '+') { a->p++; v = a_unary(a); }
    else if (*a->p == '!') { a->p++; v = !a_unary(a); }
    else if (*a->p == '~') { a->p++; v = ~a_unary(a); }
    else v = a_prim1(a);
    a->depth--;
    return v;
}

static long a_pow(Ar *a)
{
    long v = a_unary(a), e, r = 1;
    a_sp(a);
    if (a->p[0] != '*' || a->p[1] != '*') return v;
    a->p += 2;
    e = a_pow(a);                       /* right to left: 2**3**2 is 2**9 */
    if (e < 0) { if (!a->noeval) a->err = 4; return 0; }
    while (e-- > 0 && r) r *= v;
    return r;
}

static long a_mul(Ar *a)
{
    long v = a_pow(a);
    for (;;) {
        char c;
        a_sp(a);
        c = *a->p;
        if ((c == '*' || c == '/' || c == '%') && a->p[1] != '=' && a->p[1] != '*') { a->p++; v = a_binop(a, v, c, a_pow(a)); }
        else return v;
    }
}

static long a_add(Ar *a)
{
    long v = a_mul(a);
    for (;;) {
        char c;
        a_sp(a);
        c = *a->p;
        if ((c == '+' || c == '-') && a->p[1] != '=' && a->p[1] != c) { a->p++; v = a_binop(a, v, c, a_mul(a)); }
        else if ((c == '+' || c == '-') && a->p[1] == c) { a->p++; v = a_binop(a, v, c, a_mul(a)); }   /* 1--1, 1++x */
        else return v;
    }
}

static long a_shift(Ar *a)
{
    long v = a_add(a);
    for (;;) {
        a_sp(a);
        if ((a->p[0] == '<' || a->p[0] == '>') && a->p[1] == a->p[0] && a->p[2] != '=') {
            char c = a->p[0];
            a->p += 2;
            v = a_binop(a, v, c, a_add(a));
        } else return v;
    }
}

static long a_rel(Ar *a)
{
    long v = a_shift(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '<' && a->p[1] == '=') { a->p += 2; v = v <= a_shift(a); }
        else if (a->p[0] == '>' && a->p[1] == '=') { a->p += 2; v = v >= a_shift(a); }
        else if (a->p[0] == '<' && a->p[1] != '<') { a->p++; v = v < a_shift(a); }
        else if (a->p[0] == '>' && a->p[1] != '>') { a->p++; v = v > a_shift(a); }
        else return v;
    }
}

static long a_eq(Ar *a)
{
    long v = a_rel(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; v = v == a_rel(a); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; v = v != a_rel(a); }
        else return v;
    }
}

static long a_band(Ar *a)
{
    long v = a_eq(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') { a->p++; v &= a_eq(a); }
        else return v;
    }
}

static long a_bxor(Ar *a)
{
    long v = a_band(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '^' && a->p[1] != '=') { a->p++; v ^= a_band(a); }
        else return v;
    }
}

static long a_bor(Ar *a)
{
    long v = a_bxor(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') { a->p++; v |= a_bxor(a); }
        else return v;
    }
}

static long a_land(Ar *a)
{
    long v = a_bor(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '&' && a->p[1] == '&') {
            long w;
            a->p += 2;
            if (!v) a->noeval++;
            w = a_bor(a);
            if (!v) a->noeval--;
            v = v && w;
        } else return v;
    }
}

static long a_lor(Ar *a)
{
    long v = a_land(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '|' && a->p[1] == '|') {
            long w;
            a->p += 2;
            if (v) a->noeval++;
            w = a_land(a);
            if (v) a->noeval--;
            v = v || w;
        } else return v;
    }
}

static long a_assign(Ar *a)                /* c ? x : y (assignments are made in a_prim1) */
{
    long c = a_lor(a), x, y;
    a_sp(a);
    if (*a->p != '?') return c;
    a->p++;
    if (!c) a->noeval++;
    x = a_comma(a);
    if (!c) a->noeval--;
    a_sp(a);
    if (*a->p != ':') { a->err = 1; return 0; }
    a->p++;
    if (c) a->noeval++;
    y = a_assign(a);
    if (c) a->noeval--;
    return c ? x : y;
}

static long a_comma(Ar *a)
{
    long v = a_assign(a);
    for (;;) {
        a_sp(a);
        if (*a->p == ',') { a->p++; v = a_assign(a); }
        else return v;
    }
}

/* err: 1 bad, 2 division by zero, 3 said already (readonly, unbound), 4 negative exponent */
long arith(const char *expr, int *err)
{
    Ar a;
    long v;
    a.p = expr; a.err = 0; a.depth = 0; a.noeval = 0;
    a_sp(&a);
    if (!*a.p) { *err = 0; return 0; }          /* $(( )) is 0 */
    v = a_comma(&a);
    a_sp(&a);
    if (*a.p && !a.err) a.err = 1;
    *err = a.err;
    return v;
}

static void arith_error(int err, const char *text)
{
    if (err == 3) return;
    sh_error(err == 2 ? "division by zero in" : err == 4 ? "exponent less than 0 in" : "bad arithmetic:", text);
}

/* ---- expanding words ------------------------------------------------------------- */

typedef struct {
    Arena *ar;
    struct shfield *f;
    int nf, cap;
    Str cur;
    int quoted, pattern, have;
    int split;                  /* unquoted expansions split into words */
    int patmode;                /* a pattern: quoted * ? [ \ get a \ (literal for gmatch) */
    int sqlit;                  /* ${x:-'a'} inside "...": the ' are kept, as bash does */
    int heredoc;                /* a here-document's body: \" stays \" */
    int atempty;                /* "${a[@]}" had no elements: no word at all */
    int remode;                 /* a regex: quoted characters lose their meaning there */
} Exp;

static void endfield(Exp *e)
{
    struct shfield *nf;
    if (!e->have) return;
    if (e->nf == e->cap) {
        int cap = e->cap * 2 + 8;
        nf = aalloc(e->ar, cap * sizeof *nf);
        if (!nf) return;
        if (e->f) memcpy(nf, e->f, e->nf * sizeof *nf);
        e->f = nf; e->cap = cap;
    }
    e->f[e->nf].s = e->cur.s ? e->cur.s : astr(e->ar, "", 0);
    e->f[e->nf].len = e->cur.n;
    e->f[e->nf].quoted = e->quoted;
    e->f[e->nf].pattern = e->pattern;
    e->nf++;
    memset(&e->cur, 0, sizeof e->cur);
    e->cur.ar = e->ar;
    e->quoted = e->pattern = e->have = 0;
}

/* A character that was quoted: in a pattern its meaning as a wildcard goes. */
static void qput(Exp *e, char c)
{
    if (e->patmode && (c == '*' || c == '?' || c == '[' || c == '\\')) sputc(&e->cur, '\\');
    if (e->remode && c && strchr(".[]()*+?{}|^$\\", c)) sputc(&e->cur, '\\');
    sputc(&e->cur, c);
}

static void qputs(Exp *e, const char *s, int n)
{
    int i;
    if (!e->patmode && !e->remode) { sput(&e->cur, s, n); return; }
    for (i = 0; i < n; i++) qput(e, s[i]);
}

/* Text an expansion gave: quoted it is kept whole, else split on white space. */
static void addtext(Exp *e, const char *s, int n, int inquotes)
{
    int i;
    if (inquotes) { qputs(e, s, n); e->have = 1; return; }
    if (!e->split) { sput(&e->cur, s, n); e->have = 1; return; }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            endfield(e);
            continue;
        }
        sputc(&e->cur, c);
        e->have = 1;
    }
}

/* the rc of the last $( ): a=$(false) gives it to $? */
static long caprc;
static int capcount;
static long subshell(Node *n, const char *text, int len);

static char *capture2(Arena *ar, const char *text, int len, int trim)
{
    void *save = os_capture();
    char *out = NULL, *r;
    int n;
    if (!save) return astr(ar, "", 0);
    caprc = subshell(NULL, text, len);
    capcount++;
    n = os_captured(save, &out);
    if (!out) return astr(ar, "", 0);
    while (trim && n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    r = astr(ar, out, n);
    os_free(out);
    return r;
}

static char *capture(Arena *ar, const char *text, int len) { return capture2(ar, text, len, 1); }


static int getparam(Exp *e, const char *name, int n, Str *val)
{
    char b[24];
    (void)e;
    val->n = 0;
    if (n == 1 && name[0] == '?') { sput(val, b, ltoa_(rc, b)); return 1; }
    if (n == 1 && name[0] == '#') { sput(val, b, ltoa_(npos, b)); return 1; }
    if (n == 1 && name[0] == '$') { sput(val, b, ltoa_(os_number(), b)); return 1; }
    if (n == 1 && name[0] == '!') {             /* $!: the last background job, as %n (kill $!, wait $!) */
        int k = job_lastbg();
        if (k) { sputc(val, '%'); sput(val, b, ltoa_(k, b)); }
        return 1;
    }
    if (n == 1 && name[0] == '0') { sput(val, pos0, strlen(pos0)); return 1; }
    if (n == 1 && name[0] == '-') {             /* the options on: $- */
        if (sh_opt_e) sputc(val, 'e');
        if (sh_opt_f) sputc(val, 'f');
        if (sh_opt_u) sputc(val, 'u');
        if (sh_opt_x) sputc(val, 'x');
        if (os_interactive()) sputc(val, 'i');
        return 1;
    }
    if (n == 1 && (name[0] == '*' || name[0] == '@')) {
        int i;
        for (i = 0; i < npos; i++) { if (i) sputc(val, ' '); sput(val, pos[i], strlen(pos[i])); }
        return 1;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        int i = 0, k;
        for (k = 0; k < n; k++) { if (name[k] < '0' || name[k] > '9') return 0; i = i * 10 + name[k] - '0'; }
        if (i >= 1 && i <= npos) { sput(val, pos[i - 1], strlen(pos[i - 1])); return 1; }
        return 0;
    }
    {
        char nm[128], buf[LINE_MAX];
        int l;
        if (n >= (int)sizeof nm) return 0;
        memcpy(nm, name, n); nm[n] = 0;
        if ((l = sh_get(nm, buf, sizeof buf)) < 0) return 0;
        sput(val, buf, l);
        return 1;
    }
}

static void expand_into(Exp *e, const char *s, int n, int inquotes);

/* A part of a ${ } expanded as one string (a subscript, an offset, a pattern). */
static char *subexp(Exp *e, const char *s, int n, int patmode)
{
    Exp sub;
    memset(&sub, 0, sizeof sub); sub.ar = e->ar; sub.cur.ar = e->ar;
    sub.patmode = patmode;
    expand_into(&sub, s, n, !patmode);
    return sub.cur.s ? sub.cur.s : astr(e->ar, "", 0);
}

/* The values a parameter stands for: one, or a list ($@, ${a[@]}, ${!a[@]}). */
typedef struct { char **v; int n, set, list, star; } Vals;

static void vadd(Exp *e, Vals *v, const char *s, int n)
{
    if ((v->n & 15) == 0) {
        char **nv = aalloc(e->ar, (v->n + 16) * sizeof *nv);
        if (!nv) return;
        if (v->v) memcpy(nv, v->v, v->n * sizeof *nv);
        v->v = nv;
    }
    v->v[v->n++] = astr(e->ar, s, n);
}

static void getvals(Exp *e, const char *name, int nl, const char *sub, int sl, int keys, Vals *v)
{
    char nm[128];
    Arr *a;
    memset(v, 0, sizeof *v);
    if (nl >= (int)sizeof nm) return;
    memcpy(nm, name, nl); nm[nl] = 0;
    if (sub && sl == 1 && (sub[0] == '@' || sub[0] == '*')) {        /* ${a[@]} ${!a[@]} */
        AEnt *x;
        v->list = 1; v->star = sub[0] == '*';
        if ((a = arr_find(nm))) {
            for (x = a->head; x; x = x->next) {
                char b[24];
                if (keys) { if (a->assoc) vadd(e, v, x->key, strlen(x->key)); else vadd(e, v, b, ltoa_(x->idx, b)); }
                else vadd(e, v, x->val, strlen(x->val));
            }
        } else {
            Str s;
            memset(&s, 0, sizeof s); s.ar = e->ar;
            if (getparam(e, nm, nl, &s)) { if (keys) vadd(e, v, "0", 1); else vadd(e, v, s.s ? s.s : "", s.n); }
        }
        v->set = v->n > 0;
        return;
    }
    if (!sub && nl == 1 && (nm[0] == '@' || nm[0] == '*')) {        /* $@ $* */
        int i;
        v->list = 1; v->star = nm[0] == '*';
        for (i = 0; i < npos; i++) vadd(e, v, pos[i], strlen(pos[i]));
        v->set = 1;
        return;
    }
    if (sub) {
        char *key = subexp(e, sub, sl, 0), buf[LINE_MAX];
        int l = elem_get(nm, key, buf, sizeof buf);
        if (l >= 0) { vadd(e, v, buf, l); v->set = 1; }
        return;
    }
    {
        Str s;
        memset(&s, 0, sizeof s); s.ar = e->ar;
        if ((v->set = getparam(e, nm, nl, &s))) vadd(e, v, s.s ? s.s : "", s.n);
    }
}

static int uc(int c)
{
    c &= 0xff;
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) c -= 32;
    return c;
}

/* ${x/pat/rep} ${x//...} ${x/#...} ${x/%...} on one value */
static char *replace(Exp *e, char *v, const char *pat, const char *rep, char how)
{
    Str o;
    int len = strlen(v), k = 0, m, done = 0;
    memset(&o, 0, sizeof o); o.ar = e->ar;
    sput(&o, "", 0);
    if (!*pat) return v;
    while (k <= len) {
        int hit = -1;
        if (!done && (how != '#' || k == 0)) {
            for (m = len - k; m >= 0; m--) {           /* the longest match here */
                char c = v[k + m];
                if (how == '%' && k + m != len) continue;
                v[k + m] = 0;
                if (gmatch(pat, v + k, 0)) hit = m;
                v[k + m] = c;
                if (hit >= 0) break;
            }
        }
        if (hit >= 0 && (hit > 0 || how == '#' || how == '%')) {
            sput(&o, rep, strlen(rep));
            k += hit;
            if (how != '/') done = 1;
            if (hit == 0) { if (k < len) sputc(&o, v[k]); k++; }
            continue;
        }
        if (k < len) sputc(&o, v[k]);
        k++;
    }
    return o.s;
}

/* ${...}: the inside, without the braces */
static void brace(Exp *e, const char *s, int n, int inquotes)
{
    int i = 0, length = 0, indirect = 0, nl, sl = 0, k;
    const char *name, *sub = NULL;
    Vals v;
    if (n > 1 && s[0] == '#' && s[1] != ':' && !(n > 2 && strchr("-=+?#%", s[1]) && s[1] != '#')) { length = 1; i = 1; }
    else if (n > 1 && s[0] == '!') { indirect = 1; i = 1; }
    name = s + i;
    if (i < n && wordch(s[i])) while (i < n && wordch(s[i])) i++;
    else if (i < n && strchr("?#$@*-!0", s[i])) i++;
    nl = s + i - name;
    if (!nl) goto bad;
    if (i < n && s[i] == '[') {
        int d = 0, j;
        for (j = i; j < n; j++) { if (s[j] == '[') d++; else if (s[j] == ']' && --d == 0) break; }
        if (j >= n) goto bad;
        sub = s + i + 1; sl = j - i - 1;
        i = j + 1;
    }
    if (indirect && !(sub && sl == 1 && (sub[0] == '@' || sub[0] == '*'))) {
        /* ${!ref}: the value of the variable whose name ref holds (a[1] too) */
        Vals r;
        char *t, *br;
        getvals(e, name, nl, sub, sl, 0, &r);
        if (!r.set || !r.n) { if (sh_opt_u) { sh_error("unbound variable:", "${!...}"); aborted = 1; } return; }
        t = r.v[0];
        br = strchr(t, '[');
        if (br && t[strlen(t) - 1] == ']') getvals(e, t, br - t, br + 1, strlen(br + 1) - 1, 0, &v);
        else getvals(e, t, strlen(t), NULL, 0, 0, &v);
    } else getvals(e, name, nl, sub, sl, indirect, &v);
    if (length) {
        char b[24];
        long c = v.list ? v.n : v.n ? (long)strlen(v.v[0]) : 0;
        if (i != n) goto bad;
        if (!v.set && !v.list && sh_opt_u) { sh_error("unbound variable:", name); aborted = 1; return; }
        addtext(e, b, ltoa_(c, b), 1);
        return;
    }
    if (i < n) {
        int colon = s[i] == ':' && i + 1 < n && strchr("-=+?", s[i + 1]);
        char op = s[i + colon];
        const char *word = s + i + colon + 1;
        int wl = n - (i + colon + 1);
        int empty = !v.set || (colon && (v.list ? v.n == 0 : (v.n == 0 || !*v.v[0])));
        Exp sub2;
        if (op == '-' || op == '=' || op == '+' || op == '?') {
            if ((op == '+') ? !empty : empty) {
                /* the word's own quotes come off: ${u:-'a b'} is a b */
                memset(&sub2, 0, sizeof sub2); sub2.ar = e->ar; sub2.cur.ar = e->ar;
                sub2.sqlit = inquotes;
                expand_into(&sub2, word, wl, 0);
                if (op == '=') {
                    char nm[128];
                    if (nl < (int)sizeof nm) { memcpy(nm, name, nl); nm[nl] = 0; sh_set(nm, sub2.cur.s ? sub2.cur.s : "", V_ASSIGN); }
                }
                if (op == '?') { sh_error(sub2.cur.n ? sub2.cur.s : "parameter not set", ""); aborted = 1; return; }
                addtext(e, sub2.cur.s ? sub2.cur.s : "", sub2.cur.n, inquotes || sub2.quoted);
                return;
            }
            if (op == '+') return;
            goto out;
        }
        if (s[i] == ':') {                     /* ${x:offset} ${x:offset:length} */
            const char *c2 = memchr(s + i + 1, ':', n - i - 1);
            int err = 0, haslen = c2 != NULL;
            long off, ln = 0;
            char *t = subexp(e, s + i + 1, (c2 ? c2 : s + n) - (s + i + 1), 0);
            off = arith(t, &err);
            if (!err && haslen) ln = arith(subexp(e, c2 + 1, s + n - c2 - 1, 0), &err);
            if (err) { arith_error(err, t); aborted = 1; return; }
            if (v.list) {
                /* elements: $@ counts from 1 ($0 at 0), an array from its first */
                int base = !sub && !indirect ? 1 : 0, total = v.n + base, from, to;
                Vals w;
                if (off < 0) off += total;
                if (off < 0) off = total;
                from = off; to = haslen ? (ln < 0 ? total + ln : off + ln) : total;
                if (to > total) to = total;
                memset(&w, 0, sizeof w); w.list = 1; w.star = v.star; w.set = 1;
                for (k = from; k < to; k++) {
                    if (k < base) vadd(e, &w, pos0, strlen(pos0));
                    else vadd(e, &w, v.v[k - base], strlen(v.v[k - base]));
                }
                v = w;
            } else if (v.n) {
                long L = strlen(v.v[0]), to;
                if (off < 0) off += L;
                if (off < 0 || off > L) off = L;
                to = haslen ? (ln < 0 ? L + ln : off + ln) : L;
                if (to > L) to = L;
                if (to < off) { if (ln < 0) { sh_error("substring expression < 0:", t); aborted = 1; return; } to = off; }
                v.v[0] = astr(e->ar, v.v[0] + off, to - off);
            }
            goto out;
        }
        if (op == '#' || op == '%') {
            int longest = s[i + 1] == op;
            const char *pat = s + i + 1 + longest;
            char *p = subexp(e, pat, n - (pat - s), 1);
            for (k = 0; k < v.n; k++) {
                char *x = v.v[k];
                int len = strlen(x), j, hit = -1;
                if (op == '#') {
                    for (j = longest ? len : 0; longest ? j >= 0 : j <= len; j += longest ? -1 : 1) {
                        char c = x[j];
                        x[j] = 0;
                        if (gmatch(p, x, 0)) hit = j;
                        x[j] = c;
                        if (hit >= 0) break;
                    }
                    if (hit >= 0) v.v[k] = x + hit;
                } else {
                    for (j = longest ? 0 : len; longest ? j <= len : j >= 0; j += longest ? 1 : -1)
                        if (gmatch(p, x + j, 0)) { x[j] = 0; break; }
                }
            }
            goto out;
        }
        if (op == '/') {
            char how = '1', *p, *r = "";
            const char *ps = s + i + 1, *slash;
            int depth2 = 0;
            if (*ps == '/' ) { how = '/'; ps++; }
            else if (*ps == '#') { how = '#'; ps++; }
            else if (*ps == '%') { how = '%'; ps++; }
            for (slash = ps; slash < s + n; slash++) {   /* the pattern ends at a / not in ${ } or quotes */
                if (*slash == '\\') { slash++; continue; }
                if (*slash == '{') depth2++;
                else if (*slash == '}') depth2--;
                else if (*slash == '/' && !depth2) break;
            }
            p = subexp(e, ps, (slash < s + n ? slash : s + n) - ps, 1);
            if (slash < s + n) {
                Exp sub3;
                memset(&sub3, 0, sizeof sub3); sub3.ar = e->ar; sub3.cur.ar = e->ar;
                sub3.sqlit = inquotes;
                expand_into(&sub3, slash + 1, s + n - slash - 1, 0);
                r = sub3.cur.s ? sub3.cur.s : "";
            }
            for (k = 0; k < v.n; k++) v.v[k] = replace(e, v.v[k], p, r, how);
            goto out;
        }
        if (op == '@' && i + 2 == n && strchr("QULuE", s[i + 1])) {   /* ${x@Q} ${x@U} ${x@L} ${x@u} ${x@E} */
            char how = s[i + 1];
            for (k = 0; k < v.n; k++) {
                char *x = v.v[k], *o;
                int j, m = 0;
                if (how == 'Q') {          /* 'a b', a ' as '\'' */
                    for (j = 0; x[j]; j++) m += x[j] == '\'' ? 4 : 1;
                    if (!(o = aalloc(e->ar, m + 3))) continue;
                    m = 0; o[m++] = '\'';
                    for (j = 0; x[j]; j++) {
                        if (x[j] == '\'') { memcpy(o + m, "'\\''", 4); m += 4; }
                        else o[m++] = x[j];
                    }
                    o[m++] = '\''; o[m] = 0;
                    v.v[k] = o;
                } else if (how == 'E') {   /* the escapes of $'...' */
                    v.v[k] = dollarq(e->ar, x, strlen(x));
                } else
                    for (j = 0; x[j]; j++) x[j] = how == 'L' ? lc(x[j]) : how == 'U' || j == 0 ? uc(x[j]) : x[j];
            }
            goto out;
        }
        if (op == '^' || op == ',') {          /* ${x^} ${x^^} ${x,} ${x,,} */
            int all = i + 1 < n && s[i + 1] == op;
            for (k = 0; k < v.n; k++) {
                char *x = v.v[k];
                int j;
                for (j = 0; x[j] && (all || j == 0); j++) x[j] = op == '^' ? uc(x[j]) : lc(x[j]);
            }
            goto out;
        }
        goto bad;
    }
out:
    if (!v.set && sh_opt_u && !(nl == 1 && (*name == '@' || *name == '*'))) {
        char nm[64];
        int l = nl < 63 ? nl : 63;
        memcpy(nm, name, l); nm[l] = 0;
        sh_error("unbound variable:", nm);
        aborted = 1;
        return;
    }
    if (v.list && inquotes && !v.star) {      /* "${a[@]}": one word each */
        for (k = 0; k < v.n; k++) {
            if (k) endfield(e);
            sput(&e->cur, v.v[k], strlen(v.v[k]));
            e->have = 1; e->quoted = 1;
        }
        if (!v.n) e->atempty = 1;
        return;
    }
    for (k = 0; k < v.n; k++) {
        if (k) addtext(e, " ", 1, inquotes);
        addtext(e, v.v[k], strlen(v.v[k]), inquotes);
    }
    if (inquotes && !v.n) e->have = 1;
    return;
bad:
    sh_error("bad substitution:", "${...}");
    aborted = 1;
}

/* ${x@E}: a value with its $'...' escapes worked out, in the arena */
static int ansic1(const char *s, int i, int n, char *c);
static char *dollarq(Arena *ar, const char *s, int n)
{
    char *o = aalloc(ar, n + 1);
    int i = 0, m = 0;
    if (!o) return (char *)s;
    while (i < n) i = ansic1(s, i, n, &o[m++]);
    o[m] = 0;
    return o;
}

/* One character of $'...' at s[i] (a \ escape, or itself): in *c; the
 * index after it. bash's C escapes (\n \t \e \x41 \101 \' \\). */
static int ansic1(const char *s, int i, int n, char *c)
{
    *c = s[i];
    if (s[i] == '\\' && i + 1 < n) {
        char d = s[++i];
        int k, v = 0;
        switch (d) {
        case 'n': *c = '\n'; break; case 't': *c = '\t'; break; case 'r': *c = '\r'; break;
        case 'a': *c = 7; break; case 'b': *c = 8; break; case 'f': *c = 12; break; case 'v': *c = 11; break;
        case 'e': case 'E': *c = 27; break;
        case 'x':
            for (k = 0; k < 2 && i + 1 < n && ((s[i + 1] >= '0' && s[i + 1] <= '9') || ((s[i + 1] | 32) >= 'a' && (s[i + 1] | 32) <= 'f')); k++) {
                char h = s[++i];
                v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            }
            *c = k ? v : 'x';
            break;
        default:
            if (d >= '0' && d <= '7') {
                v = d - '0';
                for (k = 0; k < 2 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7'; k++) v = v * 8 + s[++i] - '0';
                *c = v;
            } else *c = d;               /* \' \" \\ \? */
        }
    }
    return i + 1;
}

/* $'...' */
static int ansic(Exp *e, const char *s, int n)
{
    int i = 2;
    while (i < n && s[i] != '\'') {
        char c;
        i = ansic1(s, i, n, &c);
        qput(e, c);
    }
    e->have = 1; e->quoted = 1;
    return i + 1;
}

/* $ at s: what it stands for; returns how much of s it took. */
static int dollar(Exp *e, const char *s, int n, int inquotes)
{
    if (n >= 3 && s[1] == '(' && s[2] == '(') {          /* $(( )) */
        long end = scan_nested(s, n, 0);
        int i = (end < 0 ? n + 1 : end) - 1;            /* at the last ) */
        {
            Exp sub;
            int err;
            long v;
            char b[24];
            memset(&sub, 0, sizeof sub); sub.ar = e->ar; sub.cur.ar = e->ar;
            expand_into(&sub, s + 3, i - 4 > 0 ? i - 4 : 0, 1);
            v = arith(sub.cur.s ? sub.cur.s : "0", &err);
            if (err) { arith_error(err, sub.cur.s ? sub.cur.s : ""); aborted = 1; }
            addtext(e, b, ltoa_(v, b), inquotes);
        }
        return i + 1;
    }
    if (n >= 2 && s[1] == '(') {                          /* $( ) */
        long end = scan_nested(s, n, 0);
        int i = (end < 0 ? n + 1 : end) - 1;            /* at the ) */
        {
            char *out = capture(e->ar, s + 2, i - 2);
            addtext(e, out, strlen(out), inquotes);
        }
        return i + 1;
    }
    if (n >= 2 && s[1] == '{') {
        long end = scan_nested(s, n, 0);
        int i = (end < 0 ? n + 1 : end) - 1;            /* at the } */
        if (i <= 2) { sh_error("bad substitution:", "${}"); aborted = 1; return i + 1; }
        brace(e, s + 2, i - 2, inquotes);
        return i + 1;
    }
    if (n >= 2 && !inquotes && s[1] == '\'') return ansic(e, s, n);
    if (n >= 2 && inquotes && s[1] == '@') {             /* "$@": one word each */
        int i;
        for (i = 0; i < npos; i++) {
            if (i) endfield(e);
            sput(&e->cur, pos[i], strlen(pos[i]));
            e->have = 1; e->quoted = 1;
        }
        return 2;
    }
    if (n >= 2 && (s[1] == '?' || s[1] == '#' || s[1] == '$' || s[1] == '*' || s[1] == '@' || s[1] == '-' || s[1] == '!' || (s[1] >= '0' && s[1] <= '9'))) {
        Str v;
        memset(&v, 0, sizeof v); v.ar = e->ar;
        if (getparam(e, s + 1, 1, &v)) addtext(e, v.s ? v.s : "", v.n, inquotes);
        else if (sh_opt_u && s[1] >= '1' && s[1] <= '9') { char nm[2] = { s[1], 0 }; sh_error("unbound variable: $", nm); aborted = 1; }
        return 2;
    }
    {
        int i = 1;
        Str v;
        while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) i++;
        if (i == 1) { sputc(&e->cur, '$'); e->have = 1; return 1; }   /* a lone $ */
        memset(&v, 0, sizeof v); v.ar = e->ar;
        if (getparam(e, s + 1, i - 1, &v)) addtext(e, v.s ? v.s : "", v.n, inquotes);
        else {
            if (sh_opt_u) {
                char nm[64];
                int l = i - 1 < 63 ? i - 1 : 63;
                memcpy(nm, s + 1, l); nm[l] = 0;
                sh_error("unbound variable:", nm);
                aborted = 1;
            }
            if (inquotes) e->have = 1;
        }
        return i;
    }
}

/* <(cmd): the T: files made, deleted when the command is done (run_simple) */
static char procfiles[4][40];
static int nproc;

static void procsub(Exp *e, const char *text, int len)
{
    char *out = capture2(e->ar, text, len, 0);
    if (nproc >= 4 || !os_tempfile(out, strlen(out), procfiles[nproc], 40)) { sh_error("cannot make a file for", "<( )"); aborted = 1; return; }
    sput(&e->cur, procfiles[nproc], strlen(procfiles[nproc]));
    nproc++;
    e->have = 1;
}

static void procdone(void)
{
    while (nproc > 0) os_delete(procfiles[--nproc]);
}

static void expand_into(Exp *e, const char *s, int n, int inquotes)
{
    int i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '$') { i += dollar(e, s + i, n - i, inquotes); continue; }
        if (c == '`') {
            int j = i + 1;
            Str in;
            memset(&in, 0, sizeof in); in.ar = e->ar;
            while (j < n && s[j] != '`') {
                if (s[j] == '\\' && j + 1 < n && (s[j + 1] == '`' || s[j + 1] == '\\' || s[j + 1] == '$')) j++;
                sputc(&in, s[j]);
                j++;
            }
            {
                char *out = capture(e->ar, in.s ? in.s : "", in.n);
                addtext(e, out, strlen(out), inquotes);
            }
            i = j + 1;
            continue;
        }
        if (inquotes) {
            if (c == '\\' && i + 1 < n && (s[i + 1] == '$' || s[i + 1] == '`' || (s[i + 1] == '"' && !e->heredoc) || s[i + 1] == '\\' || s[i + 1] == '\n')) {
                if (s[i + 1] != '\n') qput(e, s[i + 1]);
                i += 2; e->have = 1; continue;
            }
            qput(e, c); e->have = 1; i++;
            continue;
        }
        if (c == '\'' && !e->sqlit) {
            int j = i + 1;
            while (j < n && s[j] != '\'') j++;
            qputs(e, s + i + 1, j - i - 1);
            e->have = 1; e->quoted = 1;
            i = j + 1;
            continue;
        }
        if (c == '"') {
            /* to the closing quote, $( ) ${ } and ` ` inside skipped whole */
            long end = scan_quote(s, n, i);
            int j = (end < 0 ? n + 1 : end) - 1, had = e->have;
            /* "$@" with no arguments is no word at all */
            if (j - i - 1 == 2 && s[i + 1] == '$' && s[i + 2] == '@' && !npos) { i = j + 1; if (!had) e->have = 0; continue; }
            e->have = 1; e->quoted = 1;
            e->atempty = 0;
            expand_into(e, s + i + 1, j - i - 1, 1);
            if (e->atempty && !had && s[i + 1] == '$' && s[i + 2] == '{' && scan_nested(s, n, i + 1) == j) e->have = 0;
            e->atempty = 0;
            i = j + 1;
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            if (s[i + 1] != '\n') { qput(e, s[i + 1]); e->quoted = 1; }
            e->have = 1;
            i += 2;
            continue;
        }
        if (c == '<' && i + 1 < n && s[i + 1] == '(') {     /* <(cmd): its output in a T: file, the file's name */
            long end = scan_nested(s, n, i);
            int j = (end < 0 ? n + 1 : end) - 1;
            procsub(e, s + i + 2, j - i - 2);
            i = j + 1;
            continue;
        }
        if (c == '*' || (c == '?' && (i || n > 1))) e->pattern = 1;      /* ? alone is ReadArgs' help */
        if (c == '[' && memchr(s + i + 1, ']', n - i - 1)) e->pattern = 1;
        sputc(&e->cur, c);
        e->have = 1;
        i++;
    }
}

/* A word as fields (split = word splitting of unquoted expansions). */
/* ~ at a word's start: $HOME, else SYS: (the Amiga has no home drawer of
 * its own), then the rest of the word; "~/x" and HOME "Work:" give Work:x. */
static void expand_word(Exp *e, const char *w)
{
    if (w[0] == '~' && (w[1] == 0 || w[1] == '/')) {
        char home[256];
        int n = os_getvar("HOME", home, sizeof home);
        if (n <= 0) { strcpy(home, "SYS:"); n = 4; }
        sput(&e->cur, home, n);
        e->have = 1;
        w++;
        if (*w == '/' && (home[n - 1] == ':' || home[n - 1] == '/')) w++;
    }
    expand_into(e, w, strlen(w), 0);
    endfield(e);
}

/* The first {a,b} or {1..5} of a word outside quotes (not ${...}): its
 * braces' places, 1 = a list, 2 = a sequence; 0 = none. */
static int bracefind(const char *w, int *open, int *close)
{
    int i, q = 0;
    for (i = 0; w[i]; i++) {
        char c = w[i];
        if (q) { if (c == q) q = 0; else if (c == '\\' && q == '"' && w[i + 1]) i++; continue; }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == '\\' && w[i + 1]) { i++; continue; }
        if (c == '{' && (i == 0 || w[i - 1] != '$')) {
            int d = 0, j, comma = 0, q2 = 0;
            for (j = i; w[j]; j++) {
                char k = w[j];
                if (q2) { if (k == q2) q2 = 0; continue; }
                if (k == '\'' || k == '"') { q2 = k; continue; }
                if (k == '\\' && w[j + 1]) { j++; continue; }
                if (k == '{') d++;
                else if (k == '}' && --d == 0) break;
                else if (k == ',' && d == 1) comma = 1;
            }
            if (!w[j]) return 0;
            *open = i; *close = j;
            if (comma) return 1;
            {   /* {a..b}: numbers, or one letter each */
                const char *b = w + i + 1, *dots = strstr(b, "..");
                if (dots && dots < w + j && dots > b && dots + 2 < w + j) return 2;
            }
            i = j;              /* {x} alone: left as it is */
        }
    }
    return 0;
}

static void braces(Exp *e, const char *w, int depth)
{
    int o, c, kind = depth < 8 && e->nf < 2000 ? bracefind(w, &o, &c) : 0;
    Str t;
    if (!kind) { expand_word(e, w); return; }
    memset(&t, 0, sizeof t); t.ar = e->ar;
    if (kind == 1) {
        int start = o + 1, i, d = 0, q = 0;
        for (i = o + 1; i <= c; i++) {
            char k = w[i];
            if (q) { if (k == q) q = 0; continue; }
            if (k == '\'' || k == '"') { q = k; continue; }
            if (k == '\\' && i < c) { i++; continue; }
            if (k == '{') d++;
            else if (k == '}' && d) d--;
            else if ((k == ',' && !d) || i == c) {
                t.n = 0;
                sput(&t, w, o);
                sput(&t, w + start, i - start);
                sput(&t, w + c + 1, strlen(w + c + 1));
                braces(e, t.s, depth + 1);
                start = i + 1;
            }
        }
        return;
    }
    {
        const char *b = w + o + 1, *dots = strstr(b, "..");
        int la = dots - b, lb = (w + c) - (dots + 2);
        long x, y, step;
        char ab[24], bb[24], num[24];
        int err1 = 0, err2 = 0, letters;
        if (la >= (int)sizeof ab || lb >= (int)sizeof bb) { expand_word(e, w); return; }
        memcpy(ab, b, la); ab[la] = 0;
        memcpy(bb, dots + 2, lb); bb[lb] = 0;
        letters = la == 1 && lb == 1 && !(ab[0] >= '0' && ab[0] <= '9') && !(bb[0] >= '0' && bb[0] <= '9');
        if (letters) { x = (UBYTE_)ab[0]; y = (UBYTE_)bb[0]; }
        else {
            x = arith(ab, &err1); y = arith(bb, &err2);
            if (err1 || err2) { expand_word(e, w); return; }
        }
        step = x <= y ? 1 : -1;
        for (;; x += step) {
            t.n = 0;
            sput(&t, w, o);
            if (letters) { num[0] = (char)x; sput(&t, num, 1); }
            else sput(&t, num, ltoa_(x, num));
            sput(&t, w + c + 1, strlen(w + c + 1));
            braces(e, t.s, depth + 1);
            if (x == y || e->nf >= 2000) break;
        }
    }
}

static void expand(Exp *e, const char *word, int split)
{
    e->split = split;
    e->cur.ar = e->ar;
    if (split) braces(e, word, 0);      /* braces only where words are made */
    else expand_word(e, word);
}

/* A word as one string (assignments, redirection targets, case). */
static char *expand1(Arena *ar, const char *word, int *quoted)
{
    Exp e;
    memset(&e, 0, sizeof e);
    e.ar = ar;
    expand(&e, word, 0);
    if (quoted) *quoted = e.nf ? e.f[0].quoted : 0;
    return e.nf ? e.f[0].s : astr(ar, "", 0);
}

/* ---- back to an Amiga command line ----------------------------------------------- */

static void amigaarg(Str *b, struct shfield *f)
{
    int i, needq = f->quoted || f->len == 0;
    if (f->pattern && !f->quoted) {             /* a pattern csh did not match: *.txt -> #?.txt */
        for (i = 0; i < f->len; i++) {
            if (f->s[i] == '*') sput(b, "#?", 2);
            else sputc(b, f->s[i]);
        }
        return;
    }
    for (i = 0; i < f->len && !needq; i++) {
        char c = f->s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == ';' || c == 27) needq = 1;
    }
    if (!needq) { sput(b, f->s, f->len); return; }
    sputc(b, '"');
    for (i = 0; i < f->len; i++) {
        char c = f->s[i];
        if (c == '"') sput(b, "*\"", 2);
        else if (c == '*') sput(b, "**", 2);
        else if (c == '\n') sput(b, "*N", 2);
        else if (c == 27) sput(b, "*E", 2);
        else sputc(b, c);
    }
    sputc(b, '"');
}

/* Redirections into c (for an Amiga command), here-documents as T: files.
 * Returns 0 when one fails (said). */
static int redirs(Arena *ar, Redir *r, struct cmdline *c, char tmp[][40], int *ntmp)
{
    for (; r; r = r->next) {
        char *w = NULL;
        if (r->word && r->op != R_HERE) {
            w = expand1(ar, r->word, NULL);
            if ((int)strlen(w) > 250) { sh_error("redirection too long:", w); return 0; }
            if (!*w && r->op != R_HERESTR) { sh_error("redirection to an empty name", ""); return 0; }
            if (!strcmp(w, "/dev/null")) w = "NIL:";       /* bash scripts' 2>/dev/null */
        }
        switch (r->op) {
        case R_IN: strcpy(c->in, w); break;
        case R_OUT: strcpy(c->out, w); c->out_append = 0; break;
        case R_APP: strcpy(c->out, w); c->out_append = 1; break;
        case R_ERR: strcpy(c->err, w); c->err_append = 0; break;
        case R_ERRAPP: strcpy(c->err, w); c->err_append = 1; break;
        case R_BOTH: case R_BOTHAPP:
            strcpy(c->out, w); c->out_append = r->op == R_BOTHAPP; c->errtoout = 1; break;
        case R_ERROUT: c->errtoout = 1; break;
        case R_OUTERR: strcpy(c->out, "*"); c->out_append = 0; break;
        case R_INOUT: strcpy(c->in, w); strcpy(c->out, w); c->inout = 1; break;
        case R_HERE: case R_HERESTR: {
            Str b;
            memset(&b, 0, sizeof b); b.ar = ar;
            if (r->op == R_HERESTR) { sput(&b, w, strlen(w)); sputc(&b, '\n'); }
            else if (r->quoted) sput(&b, r->body ? r->body : "", r->body ? strlen(r->body) : 0);
            else {
                /* the body as if in double quotes ($, ` and \ work) */
                Exp e;
                memset(&e, 0, sizeof e); e.ar = ar; e.cur.ar = ar; e.heredoc = 1;
                expand_into(&e, r->body ? r->body : "", r->body ? strlen(r->body) : 0, 1);
                if (e.cur.s) sput(&b, e.cur.s, e.cur.n);
            }
            if (*ntmp >= 4 || !os_tempfile(b.s ? b.s : "", b.n, tmp[*ntmp], 40)) { sh_error("cannot make a here-document", ""); return 0; }
            strcpy(c->in, tmp[(*ntmp)++]);
            break;
        }
        }
    }
    return 1;
}

static Func *findfunc(const char *name)
{
    Func *f;
    for (f = funcs; f; f = f->next) if (!strcmp(f->name, name)) return f;
    return NULL;
}

/* A tree's here-documents in the order they were written (at most 16). */
static void heredocs_of(Node *n, Redir **v, int *k)
{
    Redir *r;
    Case *c;
    if (!n) return;
    for (r = n->redirs; r; r = r->next) if (r->op == R_HERE && *k < 16) v[(*k)++] = r;
    heredocs_of(n->a, v, k);
    heredocs_of(n->b, v, k);
    heredocs_of(n->c, v, k);
    for (c = n->cases; c; c = c->next) heredocs_of(c->body, v, k);
}

static void funcfree(Func *f)
{
    Arena old = f->ar;          /* the arena's record lives inside the arena: copied out first */
    afree(&old);
}

static void funcdone(Func *f)
{
    if (--f->busy == 0 && f->dead) funcfree(f);
}

/* ---- traps ------------------------------------------------------------------------ */

/* EXIT (the script or csh ends), INT (Ctrl-C: the trap runs instead of the
 * stop; '' ignores it), ERR (a command failed), RETURN (a function ended).
 * The Amiga has no other signals: HUP, TERM... are taken and never come. */
enum { TR_EXIT, TR_INT, TR_ERR, TR_RETURN, TR_COUNT };
static char *traps[TR_COUNT];
static int intrap;
static const char *const trapnames[] = { "EXIT", "INT", "ERR", "RETURN" };

static void runtrap(int k)
{
    long orc = rc;
    int ob = aborted;
    char *t;
    if (!traps[k] || !*traps[k] || intrap) return;
    if (!(t = os_alloc(strlen(traps[k]) + 1))) return;
    strcpy(t, traps[k]);                 /* the trap may change itself */
    intrap++;
    aborted = 0;
    run_text(t, strlen(t));
    aborted = ob || aborted;
    intrap--;
    os_free(t);
    setrc(orc);
}

/* a command failed: the ERR trap, set -e */
static void failed(long r)
{
    if (traps[TR_ERR]) runtrap(TR_ERR);
    if (sh_opt_e && (sourcing || !os_interactive())) { sh_exiting = 1; sh_exitrc = r; }
}

static int trapnum(const char *s)
{
    static const char *const quiet[] = { "HUP", "QUIT", "TERM", "KILL", "USR1", "USR2", "PIPE", "ALRM", "CHLD", "TSTP", "CONT", "STOP", "WINCH", NULL };
    int k;
    if (!strncmp(s, "SIG", 3)) s += 3;
    if (!strcmp(s, "0")) return TR_EXIT;
    if (!strcmp(s, "2")) return TR_INT;
    for (k = 0; k < TR_COUNT; k++) if (veq(trapnames[k], s)) return k;
    for (k = 0; quiet[k]; k++) if (veq(quiet[k], s)) return TR_COUNT;
    if (*s >= '1' && *s <= '9') return TR_COUNT;
    return -1;
}

long sh_trap(int argc, char **argv)
{
    int i = 1, k;
    long r = 0;
    if (argc > 1 && !strcmp(argv[1], "--")) i++;
    if (i >= argc || !strcmp(argv[i], "-p") || !strcmp(argv[i], "-l")) {
        for (k = 0; k < TR_COUNT; k++) if (traps[k]) {
            const char *t;
            sh_print("trap -- '");
            for (t = traps[k]; *t; t++) { if (*t == '\'') sh_print("'\\''"); else sh_printn(t, 1); }
            sh_print("' "); sh_print(trapnames[k]); sh_print("\n");
        }
        return 0;
    }
    {
        const char *action = argv[i];
        int reset = !strcmp(action, "-");
        if (i + 1 >= argc) {            /* trap INT: back to normal */
            if (trapnum(action) < 0) { sh_error("trap: unknown signal", action); return 10; }
            reset = 1; i--;
        }
        for (i++; i < argc; i++) {
            if ((k = trapnum(argv[i])) < 0) { sh_error("trap: unknown signal", argv[i]); r = 10; continue; }
            if (k == TR_COUNT) continue;
            os_free(traps[k]);
            traps[k] = NULL;
            if (!reset && (traps[k] = os_alloc(strlen(action) + 1))) strcpy(traps[k], action);
        }
    }
    return r;
}

/* The EXIT trap, once (csh or a script ends). */
void lang_exittrap(void)
{
    char *t = traps[TR_EXIT];
    long orc = rc;
    if (!t) return;
    traps[TR_EXIT] = NULL;
    aborted = 0; sh_exiting = 0; sh_return = sh_break = sh_continue = 0;
    if (*t) run_text(t, strlen(t));
    os_free(t);
    aborted = 0; sh_exiting = 0;
    setrc(orc);
}

static void traps_free(void)
{
    int k;
    for (k = 0; k < TR_COUNT; k++) { os_free(traps[k]); traps[k] = NULL; }
}

/* ---- ( ) and $( ): a subshell without fork ------------------------------------------ */

/* What the commands inside change is put back after them, as if they had
 * run in a copy of csh: the variables and aliases (the Amiga's local ones),
 * the current dir, the functions, the arrays, $1..., the options. A
 * variable exported to ENV: inside stays (that is a file). */
typedef struct {
    void *vars;
    Func **fv;
    int nf;
    Arr *arrs;
    char **pos, *pos0;
    int npos;
    Arena posar;
    int o[5];
} Snap;

static Arr *arr_copyall(void)
{
    Arr *head = NULL, **tail = &head, *a;
    for (a = arrs; a; a = a->next) {
        int l = strlen(a->name);
        Arr *c = os_alloc(sizeof *c + l + 1);
        AEnt *x, **et;
        if (!c) break;
        c->name = (char *)(c + 1);
        memcpy(c->name, a->name, l);
        c->assoc = a->assoc; c->n = a->n;
        et = &c->head;
        for (x = a->head; x; x = x->next) {
            AEnt *y = ent_new(x->idx, a->assoc ? x->key : NULL, x->val);
            if (!y) break;
            *et = y; et = &y->next;
        }
        *tail = c; tail = &c->next;
    }
    return head;
}

static void snap_take(Snap *s)
{
    Func *f;
    int i;
    memset(s, 0, sizeof *s);
    s->vars = os_snap();
    for (f = funcs; f; f = f->next) s->nf++;
    if (s->nf && !(s->fv = os_alloc(s->nf * sizeof *s->fv))) s->nf = 0;
    for (f = funcs, i = 0; f && i < s->nf; f = f->next, i++) { s->fv[i] = f; f->busy++; }
    s->arrs = arrs ? arr_copyall() : NULL;
    s->pos = pos; s->npos = npos; s->pos0 = pos0; s->posar = posar;
    posar.head = NULL;
    s->o[0] = sh_opt_e; s->o[1] = sh_opt_x; s->o[2] = sh_opt_u; s->o[3] = sh_opt_f; s->o[4] = sh_opt_pipefail;
}

static void snap_give(Snap *s)
{
    Func *f, *nx;
    Arr *a, *an;
    int i;
    os_restore(s->vars);
    for (f = funcs; f; f = nx) {        /* made inside: gone; replaced inside: back */
        nx = f->next;
        for (i = 0; i < s->nf && s->fv[i] != f; i++) ;
        if (i == s->nf) { if (f->busy) f->dead = 1; else funcfree(f); }
    }
    funcs = NULL;
    for (i = s->nf - 1; i >= 0; i--) { f = s->fv[i]; f->next = funcs; funcs = f; f->dead = 0; f->busy--; }
    os_free(s->fv);
    for (a = arrs; a; a = an) { an = a->next; arr_drop(a); }
    arrs = s->arrs;
    afree(&posar);
    posar = s->posar; pos = s->pos; npos = s->npos; pos0 = s->pos0;
    sh_opt_e = s->o[0]; sh_opt_x = s->o[1]; sh_opt_u = s->o[2]; sh_opt_f = s->o[3]; sh_opt_pipefail = s->o[4];
}


static int hasredir(struct cmdline *c) { return c->in[0] || c->out[0] || c->err[0] || c->errtoout; }

static void deffunc(Node *n)
{
    Func *f = findfunc(n->name), *nf;
    Lex L;
    Arena ar = { NULL };
    memset(&L, 0, sizeof L);
    L.src = n->src; L.n = strlen(n->src); L.ar = &ar;
    nf = aalloc(&ar, sizeof *nf);
    if (!nf) return;
    nf->body = parse_command(&L);
    if (!nf->body || L.error) { afree(&ar); return; }
    {   /* f() { cat <<E; } with the body on the next lines: it is not in the
         * function's text; taken from the tree parsed with the line */
        Redir *was[16], *now[16];
        int nw = 0, nn = 0, i;
        heredocs_of(n->a, was, &nw);
        heredocs_of(nf->body, now, &nn);
        for (i = 0; i < nw && i < nn; i++)
            if (!now[i]->body && was[i]->body) now[i]->body = astr(&ar, was[i]->body, strlen(was[i]->body));
    }
    nf->name = astr(&ar, n->name, strlen(n->name));
    nf->src = astr(&ar, n->src, strlen(n->src));
    if (f) {           /* replaced: the old one goes */
        Func **p = &funcs;
        while (*p != f) p = &(*p)->next;
        *p = f->next;
        if (f->busy) f->dead = 1; else funcfree(f);
    }
    nf->ar = ar;
    nf->next = funcs;
    funcs = nf;
}

/* A word for csh's own parser again (an alias's arguments): plain, or in
 * '...' with ' as '\''; an unquoted pattern stays a pattern. */
static void cshquote(Str *b, struct shfield *f)
{
    int i, plain = f->len > 0;
    if (f->pattern && !f->quoted) { sput(b, f->s, f->len); return; }
    for (i = 0; i < f->len && plain; i++) {
        char c = f->s[i];
        if (!(wordch(c) || ((UBYTE_)c >= 0xa0) || strchr("-./:+,=@%^", c))) plain = 0;
    }
    if (plain) { sput(b, f->s, f->len); return; }
    sputc(b, '\'');
    for (i = 0; i < f->len; i++) {
        if (f->s[i] == '\'') sput(b, "'\\''", 4); else sputc(b, f->s[i]);
    }
    sputc(b, '\'');
}

static int ieq_(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return !*a && !*b;
}

/* The aliases being expanded now: alias ls='ls -F' runs the command ls
 * inside, not itself again (bash's rule). */
static const char *aliasing[8];
static int naliasing;

static int inalias(const char *name)
{
    int i;
    for (i = 0; i < naliasing; i++) if (ieq_(aliasing[i], name)) return 1;
    return 0;
}

/* A value a prefix assignment (X=1 cmd) or local put aside: only a local
 * one; one from ENV: stays where it is and shows again when the local
 * copy goes. */
typedef struct Old { struct Old *next; char *name, *val; } Old;

static Old *putaside(Arena *ar, Old *list, const char *name)
{
    Old *o = aalloc(ar, sizeof *o);
    char *v = aalloc(ar, LINE_MAX);
    int l;
    if (!o || !v) return list;
    o->name = astr(ar, name, strlen(name));
    if ((l = os_getlocal(name, v, LINE_MAX)) >= 0) { v[l < LINE_MAX ? l : LINE_MAX - 1] = 0; o->val = v; }
    o->next = list;
    return o;
}

static void giveback(Old *o)
{
    for (; o; o = o->next) { if (o->val) os_setvar(o->name, o->val, V_LOCAL); else os_unsetvar(o->name, 0); }
}

/* A field list grown by one (globall's copy). */
static void fieldadd(Exp *e, struct shfield *f)
{
    if (e->nf == e->cap) {
        int cap = e->cap * 2 + 8;
        struct shfield *nf = aalloc(e->ar, cap * sizeof *nf);
        if (!nf) return;
        if (e->f) memcpy(nf, e->f, e->nf * sizeof *nf);
        e->f = nf; e->cap = cap;
    }
    e->f[e->nf++] = *f;
}

static void globto(Exp *e, const char *pat);

/* Each unquoted pattern from field from on: the files it matches, in order
 * (bash), or itself when none does. */
static void globall(Exp *e, int from)
{
    int i, nf = e->nf, any = 0;
    struct shfield *old = e->f;
    for (i = from; i < nf; i++) if (old[i].pattern && !old[i].quoted) any = 1;
    if (!any || sh_opt_f) return;
    e->f = NULL; e->nf = e->cap = 0;
    for (i = 0; i < nf; i++) {
        if (i >= from && old[i].pattern && !old[i].quoted) {
            int before = e->nf;
            globto(e, old[i].s);
            if (e->nf > before) continue;
        }
        fieldadd(e, &old[i]);
    }
}

/* An Amiga command's line from the fields. * matches files in csh, as in
 * bash - except for a command whose template takes one name only (Dir,
 * CD: the pattern goes to it, as the Amiga does, unless it matches one
 * file), and when the files make the line too long (the program gets the
 * pattern, as #?, and matches it itself). */
static int amigaline(Arena *ar, Exp *e, struct cmdline *c)
{
    Str a;
    int i, pass;
    for (pass = 0; pass < 2; pass++) {
        Exp g = *e;
        int globbed = 0;
        if (pass == 0 && !sh_opt_f) {
            for (i = 1; i < e->nf; i++) if (e->f[i].pattern && !e->f[i].quoted) globbed = 1;
            if (globbed && os_globok(e->f[0].s)) globall(&g, 1);
            else globbed = 0;
        }
        memset(&a, 0, sizeof a); a.ar = ar;
        for (i = 1; i < g.nf; i++) { if (i > 1) sputc(&a, ' '); amigaarg(&a, &g.f[i]); }
        if (a.n + 2 < (int)sizeof c->args && g.f[0].len < (int)sizeof c->cmd) {
            memcpy(c->cmd, g.f[0].s, g.f[0].len); c->cmd[g.f[0].len] = 0;
            if (a.s) memcpy(c->args, a.s, a.n);
            c->args[a.n] = '\n'; c->args[a.n + 1] = 0;
            c->arglen = a.n + 1;
            return 1;
        }
        if (!globbed) break;
    }
    sh_error("line too long", "");
    return 0;
}

/* A subscript or other part of a word, expanded as in quotes. */
static char *partexp(Arena *ar, const char *s, int n)
{
    Exp t;
    memset(&t, 0, sizeof t); t.ar = ar;
    return subexp(&t, s, n, 0);
}

/* a=(x y [5]=z): the list, its words split and matched as a command's */
static int assignlist(Arena *ar, const char *name, const char *s, int n, int plus)
{
    Arr *a = arr_find(name);
    Lex L;
    int assoc = a && a->assoc;
    if (ro_refuse(name)) return 0;
    if (!plus || !a) {
        char old[LINE_MAX];
        int l = plus ? os_getvar(name, old, sizeof old) : -1;
        if (!(a = arr_make(name, assoc))) return 0;
        if (l >= 0) arr_put(a, "0", old);
    }
    memset(&L, 0, sizeof L); L.src = s; L.n = n; L.ar = ar;
    for (;;) {
        Tok t = next(&L);
        Exp e;
        int i;
        if (t.type == T_NL) continue;
        if (t.type != T_WORD) break;
        if (t.text[0] == '[') {
            char *close = strstr(t.text, "]=");
            if (close) {
                char *key = partexp(ar, t.text + 1, close - t.text - 1);
                arr_put(a, key, expand1(ar, close + 2, NULL));
                continue;
            }
        }
        if (assoc) { sh_error("an associative array needs [key]=value:", t.text); continue; }
        memset(&e, 0, sizeof e); e.ar = ar;
        expand(&e, t.text, 1);
        globall(&e, 0);
        for (i = 0; i < e.nf; i++) arr_push(a, e.f[i].s);
    }
    return 1;
}

/* name=v, name+=v, name[k]=v, name=(list): 0 when refused (said) */
static int assign(Arena *ar, const char *w, int how)
{
    int eq = isassign(w, strlen(w)), plus = eq > 0 && w[eq - 1] == '+', nl = 0;
    char name[128], *v;
    const char *val = w + eq + 1, *br = NULL;
    while (wordch(w[nl]) && nl < 127) nl++;
    memcpy(name, w, nl); name[nl] = 0;
    if (w[nl] == '[') br = w + nl;
    if (val[0] == '(' && !br && val[strlen(val) - 1] == ')') return assignlist(ar, name, val + 1, strlen(val) - 2, plus);
    v = expand1(ar, val, NULL);
    if (aborted) return 0;
    if (br) {
        char *key = partexp(ar, br + 1, (w + eq - plus) - br - 2), old[LINE_MAX];
        int l;
        if (plus && (l = elem_get(name, key, old, sizeof old)) > 0) {
            char *j = aalloc(ar, l + strlen(v) + 1);
            if (j) { strcpy(j, old); strcat(j, v); v = j; }
        }
        return elem_set(name, key, v);
    }
    if (plus && ros && (attr_of(name) & AT_INT)) {     /* declare -i: += adds */
        char old[LINE_MAX], *j;
        int l = sh_get(name, old, sizeof old);
        if (l > 0 && (j = aalloc(ar, l + strlen(v) + 6))) {
            strcpy(j, old); strcat(j, "+("); strcat(j, *v ? v : "0"); strcat(j, ")");
            v = j;
        }
    } else if (plus) {
        char old[LINE_MAX];
        int l = sh_get(name, old, sizeof old);
        if (l > 0) {
            char *j = aalloc(ar, l + strlen(v) + 1);
            if (j) { strcpy(j, old); strcat(j, v); v = j; }
        }
    }
    return sh_set(name, v, how);
}

static void decl_quote(const char *v)       /* "..." with \ " $ ` escaped */
{
    sh_print("\"");
    for (; *v; v++) { if (strchr("\\\"$`", *v)) sh_print("\\"); sh_printn(v, 1); }
    sh_print("\"");
}

static void decl_print(const char *name)
{
    Arr *a = arr_find(name);
    char v[LINE_MAX], fl[12];
    int at = attr_of(name), k = 0;
    /* the flags in bash's order: declare -ar, -ir, -- for none */
    if (a) fl[k++] = a->assoc ? 'A' : 'a';
    if (at & AT_INT) fl[k++] = 'i';
    if (at & AT_LOW) fl[k++] = 'l';
    if (at & AT_RO) fl[k++] = 'r';
    if (at & AT_UP) fl[k++] = 'u';
    if (!k) fl[k++] = '-';
    fl[k] = 0;
    if (!a && os_getvar(name, v, sizeof v) < 0) { sh_error("declare: not found:", name); return; }
    sh_print("declare -");
    sh_print(fl);
    sh_print(" ");
    if (a) {
        AEnt *x;
        sh_print(a->name);
        sh_print("=(");
        for (x = a->head; x; x = x->next) {
            char b[24];
            sh_print("[");
            if (a->assoc) sh_print(x->key); else { ltoa_(x->idx, b); sh_print(b); }
            sh_print("]=");
            decl_quote(x->val);
            if (x->next) sh_print(" ");
        }
        sh_print(")\n");
        return;
    }
    sh_print(name);
    sh_print("=");
    decl_quote(v);
    sh_print("\n");
}

static void func_print(Func *f)
{
    sh_print(f->name);
    sh_print(" () ");
    sh_print(f->src ? f->src : "{ ...; }");
    sh_print("\n");
}

/* declare/typeset (local in a function), local, readonly: the words are
 * assignments as they stand (x=$y is not split), arrays made here */
static long __attribute__((noinline)) decl(Node *n, int kind)
{
    Arena ar = { NULL };
    int i, fa = 0, fA = 0, fr = kind == 2, fx = 0, fp = 0, ff = 0, fF = 0, fg = 0, any = 0, loc, aton = 0, atoff = 0;
    long r = 0;
    if (kind == 1 && !frame) { sh_error("local:", "only in a function"); return 10; }
    for (i = 1; i < n->nwords && !aborted; i++) {
        char *raw = n->words[i];
        int eq = isassign(raw, strlen(raw));
        loc = (kind == 1 || (kind == 0 && frame && !fg)) && !fp && !ff && !fF;
        if (!eq && (raw[0] == '-' || raw[0] == '+') && raw[1]) {
            const char *o;
            int plus = raw[0] == '+';
            for (o = raw + 1; *o; o++) switch (*o) {
            case 'i': if (plus) atoff |= AT_INT; else aton |= AT_INT; break;
            case 'l': if (plus) atoff |= AT_LOW; else { aton = (aton | AT_LOW) & ~AT_UP; atoff |= AT_UP; } break;
            case 'u': if (plus) atoff |= AT_UP; else { aton = (aton | AT_UP) & ~AT_LOW; atoff |= AT_LOW; } break;
            case 'a': fa = 1; break;
            case 'A': fA = 1; break;
            case 'r': fr = 1; break;
            case 'x': fx = 1; break;
            case 'p': fp = 1; break;
            case 'f': ff = 1; break;
            case 'F': fF = 1; break;
            case 'g': fg = 1; break;
            case 't': case 'n': break;     /* taken, not kept */
            default: { char b[3] = { '-', *o, 0 }; sh_error("declare: unknown option", b); afree(&ar); return 10; }
            }
            continue;
        }
        any = 1;
        if (eq) {
            char name[128];
            int nl = 0;
            while (wordch(raw[nl]) && nl < 127) nl++;
            memcpy(name, raw, nl); name[nl] = 0;
            if (ro_refuse(name)) { r = 1; continue; }
            if (loc) sh_local(name, NULL);
            if (aton | atoff) attr_set(name, aton, atoff);      /* before the value: declare -i x=2+3 is 5 */
            if (fA && !arr_find(name)) arr_make(name, 1);
            else if (fa && !arr_find(name)) arr_make(name, 0);
            if (!assign(&ar, raw, fx ? V_EXPORT : loc ? V_LOCAL : V_ASSIGN)) r = 1;
            if (fr) sh_readonly(name);
        } else {
            Exp e;
            int k;
            memset(&e, 0, sizeof e); e.ar = &ar;
            expand(&e, raw, 1);
            for (k = 0; k < e.nf; k++) {
                char *name = e.f[k].s, v[LINE_MAX];
                if (fp) { decl_print(name); continue; }
                if (ff || fF) {
                    Func *f = findfunc(name);
                    if (!f) { r = 1; continue; }
                    if (fF) { sh_print(name); sh_print("\n"); } else func_print(f);
                    continue;
                }
                if (loc) sh_local(name, NULL);
                if (aton | atoff) {     /* the value there now: made to fit */
                    int l = os_getvar(name, v, sizeof v);
                    attr_set(name, aton, atoff);
                    if (l >= 0 && !arr_find(name) && !sh_set(name, v, loc ? V_LOCAL : V_ASSIGN)) r = 1;
                }
                if (fA && !arr_find(name)) arr_make(name, 1);
                else if (fa && !arr_find(name)) arr_make(name, 0);
                if (fx) { int l = os_getvar(name, v, sizeof v); if (l >= 0) os_setvar(name, v, V_EXPORT); }
                if (fr) sh_readonly(name);
            }
        }
    }
    if (!any) {                 /* declare -p, -f, -F, readonly alone: list */
        if (ff || fF) { Func *f; for (f = funcs; f; f = f->next) { if (fF) { sh_print(f->name); sh_print("\n"); } else func_print(f); } }
        else {
            Arr *a;
            RO *o;
            for (a = arrs; a; a = a->next) decl_print(a->name);
            for (o = ros; o; o = o->next) if (!arr_find(o->name)) decl_print(o->name);
        }
    }
    afree(&ar);
    return r;
}

/* exec >file: kept until csh's language ends (lang_end gives them back,
 * the last first) */
#define MAXEXECREDIR 8
static void *execredirs[MAXEXECREDIR];
static int nexecredir;

static void execredir_keep(void *s)
{
    if (nexecredir < MAXEXECREDIR) execredirs[nexecredir++] = s;
    else os_unredir(s);
}

static void execredir_end(void)
{
    while (nexecredir) os_unredir(execredirs[--nexecredir]);
}

/* command -v / -V names: what each would run; rc 1 when one is nothing */
static int declkind(const char *w);
static long __attribute__((noinline)) cmd_v(Exp *e, int verbose)     /* its buffers not in run_simple's frame */
{
    char ab[LINE_MAX], path[300];
    int i;
    long r = 0;
    for (i = 2; i < e->nf; i++) {
        const char *n = e->f[i].s;
        if (os_getalias(n, ab, sizeof ab) >= 0) {
            if (verbose) { sh_print(n); sh_print(" is aliased to `"); sh_print(ab); sh_print("'\n"); }
            else { sh_print("alias "); sh_print(n); sh_print("='"); sh_print(ab); sh_print("'\n"); }
        } else if (findfunc(n)) {
            sh_print(n); sh_print(verbose ? " is a function\n" : "\n");
        } else if (builtin_find(n) >= 0 || declkind(n) >= 0) {
            sh_print(n); sh_print(verbose ? " is a shell builtin\n" : "\n");
        } else if (os_which(n, path, sizeof path)) {
            if (verbose) { sh_print(n); sh_print(" is "); }
            sh_print(path); sh_print("\n");
        } else {
            if (verbose) sh_error("command: not found:", n);
            r = 1;
        }
    }
    return r;
}

static int declkind(const char *w)
{
    if (veq(w, "declare") || veq(w, "typeset")) return 0;
    if (veq(w, "local")) return 1;
    if (veq(w, "readonly")) return 2;
    return -1;
}

static long __attribute__((noinline)) run_simple(Node *n)
{
    static char ab[LINE_MAX];   /* an alias's text: used up before anything runs */
    Arena ar = { NULL };
    Exp e;
    struct cmdline *c;
    char tmp[4][40];
    int ntmp = 0, i, b, caps, k, isexec = 0;      /* exec cmd: csh leaves after it */
    Old *old = NULL;
    long r = 0;

    memset(&e, 0, sizeof e);
    e.ar = &ar;
    if (n->nwords && (k = declkind(n->words[0])) >= 0 && !findfunc(n->words[0]) && os_getalias(n->words[0], ab, sizeof ab) < 0) {
        void *s = NULL;
        if (n->redirs && (!(c = aalloc(&ar, sizeof *c)) || !redirs(&ar, n->redirs, c, tmp, &ntmp) || (hasredir(c) && !(s = os_redir(c))))) { r = 20; goto done; }
        r = decl(n, k);
        if (s) os_unredir(s);
        goto done;
    }
    for (i = 0; i < n->nwords && !aborted; i++) expand(&e, n->words[i], 1);
    if (aborted) { procdone(); afree(&ar); return 1; }

    /* name=value alone: set; before a command: only for it */
    caps = capcount;
    for (i = 0; i < n->nassigns && !aborted; i++) {
        char *w = n->assigns[i], name[128];
        int nl = 0;
        while (wordch(w[nl]) && nl < 127) nl++;
        memcpy(name, w, nl); name[nl] = 0;
        if (e.nf) old = putaside(&ar, old, name);
        if (!assign(&ar, w, e.nf ? V_LOCAL : V_ASSIGN)) { r = 1; goto done0; }   /* X=1 cmd: only for cmd */
    }
    if (aborted) { r = 1; goto done0; }
    if (!(c = aalloc(&ar, sizeof *c))) { r = 20; goto done0; }
    if (!redirs(&ar, n->redirs, c, tmp, &ntmp)) { r = 20; goto done; }

    if (!e.nf) {
        /* only redirections: the files are made (or read), nothing runs */
        void *s = NULL;
        if (hasredir(c) && !(s = os_redir(c))) { r = 20; goto done; }
        if (s) os_unredir(s);
        /* a=$(cmd): the rc is the command's, as in bash */
        r = capcount != caps ? caprc : 0;
        goto done;
    }

    {
        char *name = e.f[0].s, **argv;
        Func *f;
        int plain = 0;                  /* command, exec: no alias or function */

        while (e.nf && (!strcmp(e.f[0].s, "command") || !strcmp(e.f[0].s, "exec")) && !findfunc(e.f[0].s)) {
            int cmd = e.f[0].s[0] == 'c';
            if (cmd && e.nf > 1 && (!strcmp(e.f[1].s, "-v") || !strcmp(e.f[1].s, "-V"))) {
                r = cmd_v(&e, e.f[1].s[1] == 'V');
                goto done;
            }
            if (!cmd && e.nf == 1) {    /* exec >file: csh's own streams stay so */
                void *s;
                if (hasredir(c)) {
                    if (!(s = os_redir(c))) { r = 20; goto done; }
                    execredir_keep(s);
                }
                r = 0;
                goto done;
            }
            if (e.nf == 1) { r = 0; goto done; }       /* command alone */
            if (cmd) plain = 1; else isexec = plain = 1;
            memmove(e.f, e.f + 1, (e.nf - 1) * sizeof *e.f);
            e.nf--;
            name = e.f[0].s;
        }

        /* an alias (the ROM's Alias, or alias x=...): its text, the rest after it */
        if (!plain && !e.f[0].quoted && naliasing < 8 && !inalias(name) && os_getalias(name, ab, sizeof ab) >= 0) {
            Str t;
            char *br = strstr(ab, "[]");
            void *s = NULL;
            memset(&t, 0, sizeof t); t.ar = &ar;
            sput(&t, ab, br ? br - ab : (int)strlen(ab));
            for (i = 1; i < e.nf; i++) { sputc(&t, ' '); cshquote(&t, &e.f[i]); }
            if (br) sput(&t, br + 2, strlen(br + 2));
            if (hasredir(c) && !(s = os_redir(c))) { r = 20; goto done; }
            aliasing[naliasing++] = name;
            r = run_text(t.s ? t.s : "", t.n);
            naliasing--;
            if (s) os_unredir(s);
            goto done;
        }
        f = plain ? NULL : findfunc(name);
        b = f ? -1 : builtin_find(name);
        if (sh_opt_x || f || b >= 0) globall(&e, 1);      /* csh's own see the files (an Amiga command: amigaline) */
        if (sh_opt_x) {
            Str t;
            memset(&t, 0, sizeof t); t.ar = &ar;
            sputc(&t, '+');
            for (i = 0; i < e.nf; i++) { sputc(&t, ' '); sput(&t, e.f[i].s, e.f[i].len); }
            sputc(&t, '\n');
            os_err(t.s, t.n);
        }
        argv = aalloc(&ar, (e.nf + 1) * sizeof *argv);
        if (!argv) { r = 20; goto done; }
        for (i = 0; i < e.nf; i++) argv[i] = e.f[i].s;
        if (f) {
            char **opos = pos, *opos0 = pos0;
            int onpos = npos;
            Arena oposar = posar;
            void *s = NULL;
            if (depth > 100 || os_stackleft() < STACK_NEEDED) { sh_error("too deep:", name); r = 20; goto done; }
            if (hasredir(c) && !(s = os_redir(c))) { r = 20; goto done; }
            pos = argv + 1; npos = e.nf - 1; pos0 = name;
            posar.head = NULL;
            depth++;
            f->busy++;
            {
                Saved *mine = NULL, **outer = frame;
                frame = &mine;
                r = exec(f->body);
                if (sh_return) { sh_return = 0; r = rc; }
                runtrap(TR_RETURN);
                frame = outer;
                unframe(mine);
            }
            funcdone(f);
            depth--;
            afree(&posar);
            posar = oposar;
            pos = opos; npos = onpos; pos0 = opos0;
            if (s) os_unredir(s);
            goto done;
        }
        if (b >= 0) {
            void *s = NULL;
            if (hasredir(c) && !(s = os_redir(c))) { r = 20; goto done; }
            r = builtin_run(b, e.nf, argv);
            if (s) os_unredir(s);
            if (r != -999) goto done;
        }
        /* an Amiga command */
        if (!amigaline(&ar, &e, c)) { r = 10; goto done; }
        {   /* the names in the arena, not on the stack: run_simple is in every level of a
             * function calling itself (its frame is how deep functions can go) */
            char *before = aalloc(&ar, 512), *now = before + 256;
            int had = before && os_pwd(before, 256);
            r = os_run(c);
            if (had) {          /* CD, a drawer typed alone: cd - comes back */
                if (os_pwd(now, 256) && strcmp(now, before)) os_setvar("OLDPWD", before, V_LOCAL);
            }
        }
    }
done:
    for (i = 0; i < ntmp; i++) os_delete(tmp[i]);
done0:
    if (isexec && !aborted) { sh_exiting = 1; sh_exitrc = r; }     /* exec cmd: csh is done after it */
    procdone();
    giveback(old);
    afree(&ar);
    return r;
}

/* Redirections around a block (if, while, { }, a function's body). */
static void *blockredir(Arena *ar, Node *n, char tmp[][40], int *ntmp)
{
    struct cmdline *c = aalloc(ar, sizeof *c);
    if (!c || !redirs(ar, n->redirs, c, tmp, ntmp)) return (void *)-1;
    return os_redir(c);
}

static void globadd(const char *name, void *u)
{
    Exp *e = u;
    sput(&e->cur, name, strlen(name));
    e->have = 1;
    endfield(e);
}

static int fieldcmp(const struct shfield *x, const struct shfield *y)
{
    const char *a = x->s, *b = y->s;
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return lc(*a) - lc(*b);
}

/* The files a pattern matches, in order, as fields (csh's own for loop):
 * sorted case blind, a merge sort (a big drawer is no slower than n log n). */
static void globto(Exp *e, const char *pat)
{
    int before = e->nf, n, w, i;
    struct shfield *a, *t;
    os_glob(pat, globadd, e);
    n = e->nf - before;
    if (n < 2 || !(t = aalloc(e->ar, n * sizeof *t))) return;
    a = e->f + before;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            int l = i, m = i + w < n ? i + w : n, r = m, end = i + 2 * w < n ? i + 2 * w : n, k = i;
            while (l < m && r < end) t[k++] = fieldcmp(&a[l], &a[r]) <= 0 ? a[l++] : a[r++];
            while (l < m) t[k++] = a[l++];
            while (r < end) t[k++] = a[r++];
        }
        memcpy(a, t, n * sizeof *t);
    }
}

static long cond(Node *n)
{
    long r;
    incond++;
    r = exec(n);
    incond--;
    return r;
}

static int stopping(void)
{
    int b;
    if (!aborted && (b = os_break())) {
        if (b == 1 && traps[TR_INT]) runtrap(TR_INT);   /* '' : ignored; Ctrl-D stops all the same */
        else { os_err("***Break\n", 8); aborted = 1; }
    }
    return aborted || sh_exiting || sh_return || sh_break || sh_continue;
}

static long loopbody(Node *body)
{
    long r = exec(body);
    if (sh_continue) { if (--sh_continue) return r; }
    return r;
}

/* ---- pipes and jobs --------------------------------------------------------------- */

/* The text of a command, near enough, for "jobs". */
static void nodetext(Node *n, Str *b)
{
    int i;
    if (!n) return;
    switch (n->type) {
    case N_SIMPLE:
        for (i = 0; i < n->nwords; i++) { if (i) sputc(b, ' '); sput(b, n->words[i], strlen(n->words[i])); }
        break;
    case N_PIPE: nodetext(n->a, b); sput(b, " | ", 3); nodetext(n->b, b); break;
    case N_FOR: sput(b, "for ...", 7); break;
    case N_WHILE: sput(b, "while ...", 9); break;
    case N_IF: sput(b, "if ...", 6); break;
    default: sput(b, "{ ... }", 7); break;
    }
}

/* The stages of a pipe in order; -1 when there are more than max. */
static int flatten(Node *n, Node **st, int *errto, int max)
{
    int k;
    if (n->type != N_PIPE) { st[0] = n; return 1; }
    k = flatten(n->a, st, errto, max);
    if (k < 0 || k >= max) return -1;
    errto[k - 1] = n->haslist;          /* |& */
    st[k++] = n->b;
    return k;
}

/* Is a stage csh's own (a built-in, function, alias, block)? */
static int internal(Arena *ar, Node *n)
{
    char *w, ab[8];
    int q;
    if (n->type != N_SIMPLE) return 1;
    if (!n->nwords) return 1;
    /* $(cmd) | ...: expanding it to see would run cmd twice; csh runs it */
    if (strchr(n->words[0], '`') || strstr(n->words[0], "$(")) return 1;
    w = expand1(ar, n->words[0], &q);
    if (findfunc(w) || builtin_find(w) >= 0) return 1;
    if (!q && os_getalias(w, ab, sizeof ab) >= 0) return 1;
    return 0;
}

/* An Amiga command's stage: its words and redirections as a cmdline. */
/* (its here-documents' T: files into tmp, *ntmp: the job deletes them) */
static int stagecmd(Arena *ar, Node *n, struct cmdline *c, char (*tmp)[40], int *ntmp)
{
    Exp e;
    int i;
    memset(&e, 0, sizeof e); e.ar = ar;
    for (i = 0; i < n->nwords && !aborted; i++) expand(&e, n->words[i], 1);
    if (aborted || !e.nf) return 0;
    if (!redirs(ar, n->redirs, c, tmp, ntmp)) return 0;
    return amigaline(ar, &e, c);
}

/* A word in '...' for csh's own parser (the state handed to a csh of its own) */
static void shq(Str *b, const char *s)
{
    sputc(b, '\'');
    for (; *s; s++) { if (*s == '\'') sput(b, "'\\''", 4); else sputc(b, *s); }
    sputc(b, '\'');
}

/* csh's own stage in a process of its own (& or the middle of a pipe): a
 * second csh runs it, from a T: script that brings what the Amiga does not
 * copy - the functions, arrays, $1..., the options. The local variables and
 * aliases come with the process (NP_Cli copies them), so it is bash's
 * subshell: what it changes stays there. */
static int ownstage(Arena *ar, Node *n, struct cmdline *c, void *job)
{
    Str t;
    Func *f;
    Arr *a;
    int i;
    char name[40];
    const char *self = NULL;
    int viashell = os_shell_is_csh();     /* the system Shell runs the #!csh script: no C:csh started */
    if (!viashell) {
        self = os_self();
        if (!*self) return 0;                         /* said (cshell without its C:csh) */
        if (strlen(self) >= sizeof c->cmd || !os_exists(self)) { sh_error("csh's own command in a process of its own needs C:csh:", n->src ? n->src : ""); return 0; }
    }
    memset(&t, 0, sizeof t); t.ar = ar;
    sput(&t, "#!csh\n", 6);
    if (sh_opt_e) sput(&t, "set -e\n", 7);
    if (sh_opt_u) sput(&t, "set -u\n", 7);
    if (sh_opt_f) sput(&t, "set -f\n", 7);
    if (sh_opt_pipefail) sput(&t, "set -o pipefail\n", 16);
    for (f = funcs; f; f = f->next) {
        sput(&t, f->name, strlen(f->name));
        sput(&t, " () ", 4);
        sput(&t, f->src, strlen(f->src));
        sputc(&t, '\n');
    }
    for (a = arrs; a; a = a->next) {
        AEnt *x;
        if (veq(a->name, "PIPESTATUS") || veq(a->name, "BASH_REMATCH")) continue;
        if (a->assoc) sput(&t, "declare -A ", 11);
        sput(&t, a->name, strlen(a->name));
        sput(&t, "=(", 2);
        for (x = a->head; x; x = x->next) {
            char b[24];
            sputc(&t, '[');
            if (a->assoc) shq(&t, x->key); else sput(&t, b, ltoa_(x->idx, b));
            sput(&t, "]=", 2);
            shq(&t, x->val);
            sputc(&t, ' ');
        }
        sput(&t, ")\n", 2);
    }
    sput(&t, "set --", 6);
    for (i = 0; i < npos; i++) { sputc(&t, ' '); shq(&t, pos[i]); }
    sputc(&t, '\n');
    if (n->src) sput(&t, n->src, strlen(n->src));
    sputc(&t, '\n');
    if (!os_tempfile(t.s, t.n, name, sizeof name)) { sh_error("cannot write the script for", n->src ? n->src : ""); return 0; }
    os_job_tmp(job, name);
    if (viashell) {             /* the script itself, s bit on: run_prepare gives it to the system Shell */
        if (!os_mkscript(name)) { sh_error("cannot mark as a script:", name); return 0; }
        strcpy(c->cmd, name);
        strcpy(c->args, "\n");
    } else {
        strcpy(c->cmd, self);
        strcpy(c->args, "FROM ");
        strcat(c->args, name);
        strcat(c->args, "\n");
    }
    c->arglen = strlen(c->args);
    return 1;
}

/* A stage that is one of csh's own commands with nothing that could go on
 * for ever (echo, printf...): in the middle of a pipe it writes a T: file. */
static int quickstage(Arena *ar, Node *n)
{
    int q;
    char *w;
    if (n->type != N_SIMPLE || !n->nwords) return 0;
    if (strchr(n->words[0], '`') || strstr(n->words[0], "$(")) return 1;
    w = expand1(ar, n->words[0], &q);
    return !findfunc(w) && builtin_find(w) >= 0;
}

/* a | b | c, and anything with &: an Amiga command runs in a process of
 * its own, joined to the next by PIPE:; csh's own stage runs in csh when it
 * is the last (as zsh: a | read x sets x), in the middle a T: file when it
 * is a quick built-in, else a csh of its own (ownstage). The rc is the last
 * stage's (set -o pipefail: the last that failed); PIPESTATUS gets each. */
static long __attribute__((noinline)) pipeline(Node *n, int bg)
{
    Arena ar = { NULL };
    Node *st[32];
    int errto[32], fromjob[32], own[32];
    long src[32], jobrcs[32];
    struct cmdline *cs[32];
    void *pre[32];
    char (*tmp)[40];
    Str text;
    void *job;
    long in, r = 0;
    int ns, i, lastint = 0, njob = 0, ntmp = 0;
    memset(&text, 0, sizeof text); text.ar = &ar;
    memset(errto, 0, sizeof errto);
    memset(fromjob, 0, sizeof fromjob);
    memset(pre, 0, sizeof pre);
    if ((ns = flatten(n, st, errto, 32)) < 0) { sh_error("a pipe of more than 32 commands", ""); afree(&ar); setrc(10); return 10; }
    if (!(tmp = aalloc(&ar, 4 * 32 * sizeof *tmp))) { afree(&ar); return 20; }
    /* the Amiga commands' words first, then each found and loaded once.
     * One after the first missing: none starts (the ROM Shell's rule; a
     * stage writing into it would wait for ever on a pipe nobody reads -
     * 3.2's PIPE: has no SIGPIPE). A missing first one is bash's case: the
     * others run, reading nothing. */
    for (i = 0; i < ns && !aborted; i++) {
        own[i] = internal(&ar, st[i]);
        cs[i] = NULL;
        if (!own[i] && (cs[i] = aalloc(&ar, sizeof *cs[i])) && !stagecmd(&ar, st[i], cs[i], tmp + ntmp, &ntmp)) cs[i] = NULL;
        if (ntmp > 4 * 31) break;
    }
    os_flush();                         /* csh's own output before a "not found" */
    for (i = ns - 1; i >= 0 && !aborted; i--) {
        if (!cs[i]) continue;
        if (!(pre[i] = os_prepare(cs[i])) && i) {
            int k;
            for (k = 0; k < ns; k++) if (pre[k]) os_unprepare(pre[k]);
            for (k = 0; k < ntmp; k++) os_delete(tmp[k]);
            afree(&ar); setrc(10); return 10;
        }
        if (!pre[i]) cs[i] = NULL;                  /* the first: said, its rc 10 */
    }
    nodetext(n, &text);
    if (aborted || !(job = os_job_new(text.s ? text.s : "", bg))) {
        for (i = 0; i < ns; i++) if (pre[i]) os_unprepare(pre[i]);
        for (i = 0; i < ntmp; i++) os_delete(tmp[i]);
        afree(&ar);
        return aborted ? 1 : 20;
    }
    for (i = 0; i < ntmp; i++) os_job_tmp(job, tmp[i]);
    in = os_job_in(job);
    for (i = 0; i < ns && !aborted; i++) {
        int last = i == ns - 1;
        src[i] = 0;
        if (own[i] && !bg && last) {
            void *s = os_select(in, 0);
            r = src[i] = exec(st[i]);
            os_unselect(s);
            os_close(job, in);
            lastint = 1;
        } else if (own[i] && !bg && quickstage(&ar, st[i])) {
            long out = os_tempout(job);
            void *s;
            Snap snap;
            if (!out) { os_close(job, in); r = 20; break; }
            s = os_select(in, out);
            snap_take(&snap);
            src[i] = exec(st[i]);
            snap_give(&snap);
            os_unselect(s);
            os_close(job, out);
            os_close(job, in);
            in = os_tempin(job);
        } else {
            struct cmdline *c = cs[i];
            long out, next = 0;
            if (own[i] && (!(c = aalloc(&ar, sizeof *c)) || !ownstage(&ar, st[i], c, job))) c = NULL;
            /* a stage that cannot start: the next one reads nothing (in 0 = NIL:) */
            if (!c) {
                os_close(job, in); in = 0; r = src[i] = 10;
                if (last) break;
                continue;
            }
            if (errto[i]) c->errtoout = 1;
            if (sh_opt_x) {
                os_err("+ ", 2); os_err(c->cmd, strlen(c->cmd)); os_err(" ", 1);
                os_err(c->args, c->arglen);
            }
            if (last && !bg && !own[i]) {   /* in csh itself, as the ROM Shell does */
                r = src[i] = os_lastrun(job, c, in, pre[i]);
                pre[i] = NULL;
                lastint = 1;
                break;
            }
            if (last) out = os_job_out(job);
            else if (!os_pipe(job, &next, &out)) { os_close(job, in); r = 20; break; }
            if (!os_spawn(job, c, in, out, pre[i])) r = sh_rc();
            pre[i] = NULL;
            fromjob[i] = 1;
            in = next;
        }
    }
    for (i = 0; i < ns; i++) if (pre[i]) os_unprepare(pre[i]);   /* a break or Ctrl-C before they ran */
    r = os_job_end(job, r, lastint, jobrcs, &njob);
    if (!bg && ns > 1) {
        Arr *a = arr_fresh("PIPESTATUS");
        int k = 0;
        long fail = 0;
        for (i = 0; i < ns; i++) {
            char b[24];
            if (fromjob[i]) src[i] = k < njob ? jobrcs[k++] : 0;
            if (a) arr_push(a, (ltoa_(src[i], b), b));
            if (src[i]) fail = src[i];
        }
        if (sh_opt_pipefail) r = fail;
    }
    afree(&ar);
    setrc(r);
    return r;
}

/* A block with redirections (if, while, { }, ( )): opened around it. */
static long __attribute__((noinline)) exec_redirected(Node *n)
{
    Arena ar = { NULL };
    char tmp[4][40];
    int ntmp = 0, i;
    long r;
    void *s = blockredir(&ar, n, tmp, &ntmp);
    Redir *keep = n->redirs;
    if (!s || s == (void *)-1) {
        /* a failed open was said (os_redir or redirs): the block does not run */
        for (i = 0; i < ntmp; i++) os_delete(tmp[i]);
        afree(&ar);
        setrc(20);
        return 20;
    }
    n->redirs = NULL;
    r = exec(n);
    n->redirs = keep;
    os_unredir(s);
    for (i = 0; i < ntmp; i++) os_delete(tmp[i]);
    afree(&ar);
    return r;
}

static long __attribute__((noinline)) exec_for(Node *n)
{
    Arena ar = { NULL };
    Exp e;
    int i;
    long r = 0;
    memset(&e, 0, sizeof e); e.ar = &ar;
    if (n->haslist) {
        for (i = 0; i < n->nwords && !aborted; i++) {
            int before = e.nf;
            expand(&e, n->words[i], 1);
            /* csh's own loop matches patterns itself: *.txt -> the files */
            if (e.nf == before + 1 && e.f[before].pattern && !e.f[before].quoted) {
                struct shfield pat = e.f[before];
                e.nf = before;
                globto(&e, pat.s);
                if (e.nf == before) { e.f[e.nf++] = pat; }
            }
        }
    } else {
        for (i = 0; i < npos; i++) { sput(&e.cur, pos[i], strlen(pos[i])); e.have = 1; endfield(&e); }
    }
    sh_loops++;
    for (i = 0; i < e.nf && !aborted; i++) {
        os_setvar(n->name, e.f[i].s, V_ASSIGN);
        r = loopbody(n->b);
        if (sh_break) { sh_break--; break; }
        if (stopping()) break;
    }
    sh_loops--;
    afree(&ar);
    setrc(r);
    return r;
}

static long __attribute__((noinline)) exec_case(Node *n)
{
    Arena ar = { NULL };
    char *w = expand1(&ar, n->name, NULL);
    Case *c;
    long r = 0;
    int any = 0, fall = 0;
    for (c = n->cases; c; c = c->next) {
        int i, hit = fall;
        for (i = 0; i < c->npats && !hit; i++) {
            Exp pe;
            memset(&pe, 0, sizeof pe); pe.ar = &ar; pe.patmode = 1;
            expand(&pe, c->pats[i], 0);
            hit = gmatch(pe.nf ? pe.f[0].s : "", w, 0);
        }
        fall = 0;
        if (!hit) continue;
        any = 1;
        r = c->body ? exec(c->body) : 0;
        if (c->end == 1) fall = 1;          /* ;& the next body too */
        else if (c->end != 2) break;        /* ;; done; ;;& tries the next patterns */
        if (aborted || sh_exiting || sh_return || sh_break || sh_continue) break;
    }
    afree(&ar);
    if (!any) setrc(0);
    return r;
}

/* (( )), for (( )), let: the text expanded as in double quotes, then arithmetic */
static long arith_text(const char *s, int *err)
{
    Arena ar = { NULL };
    Exp sub;
    long v;
    memset(&sub, 0, sizeof sub); sub.ar = &ar; sub.cur.ar = &ar;
    expand_into(&sub, s, strlen(s), 1);
    v = arith(sub.cur.s ? sub.cur.s : "", err);
    if (*err) arith_error(*err, sub.cur.s ? sub.cur.s : "");
    afree(&ar);
    return v;
}

long sh_let(int argc, char **argv)
{
    long v = 0;
    int i, err = 0;
    if (argc < 2) { sh_error("let:", "an expression is needed"); return 10; }
    for (i = 1; i < argc && !err; i++) v = arith_text(argv[i], &err);
    return err ? SH_FALSE : v ? 0 : SH_FALSE;
}

static long __attribute__((noinline)) exec_cfor(Node *n)
{
    long r = 0;
    int err = 0;
    if (*n->words[0]) arith_text(n->words[0], &err);
    if (err) { setrc(SH_FALSE); return SH_FALSE; }
    sh_loops++;
    for (;;) {
        if (n->words[1][strspn(n->words[1], " \t\n")]) {
            long c = arith_text(n->words[1], &err);
            if (err) { r = SH_FALSE; break; }
            if (!c) break;
        }
        r = loopbody(n->b);
        if (sh_break) { sh_break--; break; }
        if (stopping()) break;
        if (*n->words[2]) { arith_text(n->words[2], &err); if (err) { r = SH_FALSE; break; } }
    }
    sh_loops--;
    setrc(r);
    return r;
}

/* select name in words: a numbered menu, PS3 asks, REPLY gets the line */
static long __attribute__((noinline)) exec_select(Node *n)
{
    Arena ar = { NULL };
    Exp e;
    int i;
    long r = 0;
    char line[LINE_MAX], ps3[128];
    memset(&e, 0, sizeof e); e.ar = &ar;
    if (n->haslist) for (i = 0; i < n->nwords && !aborted; i++) expand(&e, n->words[i], 1);
    else for (i = 0; i < npos; i++) { sput(&e.cur, pos[i], strlen(pos[i])); e.have = 1; endfield(&e); }
    sh_loops++;
    for (i = 0; i < e.nf; i++) {
        char b[24];
        int l = ltoa_(i + 1, b);
        os_err(b, l); os_err(") ", 2); os_err(e.f[i].s, e.f[i].len); os_err("\n", 1);
    }
    while (!aborted) {
        int l = sh_get("PS3", ps3, sizeof ps3), k;
        long num;
        if (l < 0) { strcpy(ps3, "#? "); l = 3; }
        os_err(ps3, l);
        if ((l = os_readline(line, sizeof line)) < 0) { os_err("\n", 1); break; }
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        sh_set("REPLY", line, V_ASSIGN);
        if (!l) {
            for (k = 0; k < e.nf; k++) {
                char b[24];
                int bl = ltoa_(k + 1, b);
                os_err(b, bl); os_err(") ", 2); os_err(e.f[k].s, e.f[k].len); os_err("\n", 1);
            }
            continue;
        }
        for (num = 0, k = 0; line[k] >= '0' && line[k] <= '9'; k++) num = num * 10 + line[k] - '0';
        sh_set(n->name, !line[k] && num >= 1 && num <= e.nf ? e.f[num - 1].s : "", V_ASSIGN);
        r = loopbody(n->b);
        if (sh_break) { sh_break--; break; }
        if (stopping()) break;
    }
    sh_loops--;
    afree(&ar);
    setrc(r);
    return r;
}

/* ---- [[ ]] ---- */

typedef struct { char **w; int n, i, err, skip; Arena *ar; } Cond;

static int c_or(Cond *c);

static char *cword(Cond *c, const char *w)
{
    return c->skip ? "" : expand1(c->ar, w, NULL);
}

static int c_isbin(const char *op)
{
    static const char *const ops[] = { "=", "==", "!=", "=~", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", NULL };
    int i;
    for (i = 0; ops[i]; i++) if (!strcmp(op, ops[i])) return 1;
    return 0;
}

static int c_binary(Cond *c, const char *lhs, const char *op, const char *rw)
{
    if (c->skip) return 0;
    if (!strcmp(op, "=") || !strcmp(op, "==") || !strcmp(op, "!=")) {
        Exp pe;
        int hit;
        memset(&pe, 0, sizeof pe); pe.ar = c->ar; pe.patmode = 1;
        expand(&pe, rw, 0);
        hit = gmatch(pe.nf ? pe.f[0].s : "", lhs, 0);
        return op[0] == '!' ? !hit : hit;
    }
    if (!strcmp(op, "=~")) {
        Exp re;
        int caps[2 * RE_GROUPS], ng = 0, r, k;
        Arr *a;
        memset(&re, 0, sizeof re); re.ar = c->ar; re.cur.ar = c->ar; re.remode = 1;
        expand_into(&re, rw, strlen(rw), 0);
        r = re_match(re.cur.s ? re.cur.s : "", lhs, 0, caps, &ng);
        if (r < 0) { sh_error("bad regular expression:", re.cur.s ? re.cur.s : ""); c->err = 2; return 0; }
        if ((a = arr_fresh("BASH_REMATCH")) && r)
            for (k = 0; k <= ng; k++) {
                char *t = caps[2 * k] >= 0 && caps[2 * k + 1] >= caps[2 * k] ? astr(c->ar, lhs + caps[2 * k], caps[2 * k + 1] - caps[2 * k]) : "";
                arr_push(a, t ? t : "");
            }
        return r;
    }
    {
        char *rhs = cword(c, rw);
        if (!strcmp(op, "<")) return strcmp(lhs, rhs) < 0;
        if (!strcmp(op, ">")) return strcmp(lhs, rhs) > 0;
        if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
            long a = os_mtime(lhs), b = os_mtime(rhs);
            return op[1] == 'n' ? a > b : (a >= 0 && a < b) || (a < 0 && b >= 0);
        }
        {
            int e1 = 0, e2 = 0;
            long x = arith(lhs, &e1), y = arith(rhs, &e2);
            if (e1 || e2) { arith_error(e1 ? e1 : e2, e1 ? lhs : rhs); c->err = 2; return 0; }
            if (!strcmp(op, "-eq")) return x == y;
            if (!strcmp(op, "-ne")) return x != y;
            if (!strcmp(op, "-lt")) return x < y;
            if (!strcmp(op, "-le")) return x <= y;
            if (!strcmp(op, "-gt")) return x > y;
            return x >= y;
        }
    }
}

static int c_prim(Cond *c)
{
    const char *a;
    if (c->i >= c->n) { c->err = 1; return 0; }
    a = c->w[c->i];
    if (!strcmp(a, "!")) { c->i++; return !c_prim(c); }
    if (!strcmp(a, "(")) {
        int v;
        c->i++;
        v = c_or(c);
        if (c->i < c->n && !strcmp(c->w[c->i], ")")) c->i++; else c->err = 1;
        return v;
    }
    if (c->i + 2 < c->n + 1 && c->i + 1 < c->n && c_isbin(c->w[c->i + 1])) {
        const char *op = c->w[c->i + 1];
        char *lhs;
        if (c->i + 2 >= c->n) { c->err = 1; return 0; }
        lhs = cword(c, a);
        c->i += 3;
        return c_binary(c, lhs, op, c->w[c->i - 1]);
    }
    if (a[0] == '-' && a[1] && !a[2] && c->i + 1 < c->n && strcmp(c->w[c->i + 1], "&&") && strcmp(c->w[c->i + 1], "||") && strcmp(c->w[c->i + 1], ")")) {
        char *b;
        c->i += 2;
        b = cword(c, c->w[c->i - 1]);
        if (c->skip) return 0;
        switch (a[1]) {
        case 'z': return !*b;
        case 'n': return *b != 0;
        case 'e': case 'a': return os_test(b, 'e');
        case 'f': case 'd': case 'r': case 'w': case 'x': case 's': return os_test(b, a[1]);
        case 'v': {
            char t[4], *br = strchr(b, '[');
            if (br && b[strlen(b) - 1] == ']') {
                char nm[128], *key;
                int nl = br - b;
                if (nl >= (int)sizeof nm) return 0;
                memcpy(nm, b, nl); nm[nl] = 0;
                key = astr(c->ar, br + 1, strlen(br + 1) - 1);
                return elem_get(nm, key, t, sizeof t) >= 0;
            }
            return sh_get(b, t, sizeof t) >= 0 || sh_isarray(b);
        }
        case 'L': case 'h': return 0;          /* soft links: Lock follows them */
        }
        sh_error("[[: unknown test", a);
        c->err = 2;
        return 0;
    }
    c->i++;
    return *cword(c, a) != 0;
}

static int c_and(Cond *c)
{
    int v = c_prim(c);
    while (c->i < c->n && !strcmp(c->w[c->i], "&&")) {
        int w, sk = c->skip;
        c->i++;
        if (!v) c->skip = 1;
        w = c_prim(c);
        c->skip = sk;
        v = v && w;
    }
    return v;
}

static int c_or(Cond *c)
{
    int v = c_and(c);
    while (c->i < c->n && !strcmp(c->w[c->i], "||")) {
        int w, sk = c->skip;
        c->i++;
        if (v) c->skip = 1;
        w = c_and(c);
        c->skip = sk;
        v = v || w;
    }
    return v;
}

static long __attribute__((noinline)) exec_cond(Node *n)
{
    Arena ar = { NULL };
    Cond c;
    int v;
    memset(&c, 0, sizeof c);
    c.w = n->words; c.n = n->nwords; c.ar = &ar;
    v = c_or(&c);
    if (!c.err && c.i < c.n) c.err = 1;
    afree(&ar);
    if (c.err == 1) { sh_error("[[:", "cannot read that"); return 2; }
    if (c.err || aborted) return 2;
    return v ? 0 : SH_FALSE;
}

/* ( ... ) and $( ... ): not a subshell (no fork on the Amiga), but exit,
 * return, break and continue end only it, as they would end bash's
 * subshell; its exit code is its rc. */
static long subshell(Node *n, const char *text, int len)
{
    int ob = sh_break, oc = sh_continue, oloops = sh_loops;
    long r;
    Snap snap;
    snap_take(&snap);
    sh_break = sh_continue = 0;
    sh_loops = 0;
    r = n ? exec(n) : run_text(text, len);
    if (sh_exiting) { sh_exiting = 0; r = sh_exitrc; }
    sh_return = 0;
    sh_break = ob; sh_continue = oc; sh_loops = oloops;
    snap_give(&snap);
    setrc(r);
    return r;
}

static long exec(Node *n)
{
    long r = 0;
    if (!n || stopping()) return rc;
    if (os_stackleft() < STACK_NEEDED / 2) {      /* $( ) in $( ) in ..., aliases, sourced scripts */
        sh_error("too deep:", "the stack is nearly full");
        aborted = 1;
        return 20;
    }
    if (n->redirs && n->type != N_SIMPLE) return exec_redirected(n);
    switch (n->type) {
    case N_SIMPLE: case N_ARITH: case N_COND:
        if (n->type == N_SIMPLE) r = run_simple(n);
        else if (n->type == N_COND) r = exec_cond(n);
        else {
            int err = 0;
            r = arith_text(n->name, &err) ? 0 : SH_FALSE;
            if (err) r = SH_FALSE;
        }
        setrc(r);
        if (r && !incond) failed(r);
        return r;
    case N_LIST:
        exec(n->a);
        if (stopping()) return rc;
        return exec(n->b);
    case N_AND:
        r = cond(n->a);
        if (r == 0 && !stopping()) r = exec(n->b);
        return r;
    case N_OR:
        r = cond(n->a);
        if (r != 0 && !stopping()) r = exec(n->b);
        return r;
    case N_NOT:
        r = cond(n->a) ? 0 : SH_FALSE;
        setrc(r);
        return r;
    case N_PIPE:
        r = pipeline(n, 0);
        if (r && !incond) failed(r);
        return r;
    case N_BG:
        return pipeline(n->a, 1);
    case N_GROUP:
        return n->a ? exec(n->a) : (setrc(0), 0);
    case N_SUB:
        return n->a ? subshell(n->a, NULL, 0) : (setrc(0), 0);
    case N_IF:
        r = cond(n->a);
        if (stopping()) return rc;
        if (r == 0) return exec(n->b);
        if (n->c) return exec(n->c);
        setrc(0);
        return 0;
    case N_WHILE: case N_UNTIL:
        sh_loops++;
        r = 0;
        for (;;) {
            long t = cond(n->a);
            if (stopping()) break;
            if ((t == 0) != (n->type == N_WHILE)) break;
            r = loopbody(n->b);
            if (sh_break) { sh_break--; break; }
            if (stopping()) break;
        }
        sh_loops--;
        setrc(r);
        return r;
    case N_FOR:
        return exec_for(n);
    case N_CFOR:
        return exec_cfor(n);
    case N_SELECT:
        return exec_select(n);
    case N_CASE:
        return exec_case(n);
    case N_FUNC:
        deffunc(n);
        setrc(0);
        return 0;
    }
    return 0;
}

/* ---- reading and running text ------------------------------------------------------ */

/* Parses and runs text line by line (a line's commands run before the next
 * line is read, so a function defined on one line is there on the next).
 * An error ends the whole text. Returns the last rc. */
static long run_text(const char *text, int len)
{
    Arena ar = { NULL };
    Lex L;
    long r = rc;
    memset(&L, 0, sizeof L);
    L.src = text; L.n = len; L.ar = &ar;
    depth++;
    while (!aborted && !sh_exiting && !sh_return && !sh_break && !sh_continue) {
        Node *n;
        while (peek(&L)->type == T_NL) next(&L);
        if (peek(&L)->type == T_EOF) { if (L.incomplete) { sh_error("unexpected end:", "a quote, ( or { not closed"); r = 10; } break; }
        n = parse_line(&L);
        if (L.incomplete) { sh_error("unexpected end:", "if/while/for/case or a quote not closed"); r = 10; setrc(r); break; }
        if (L.error || !n) { r = 10; setrc(r); break; }
        r = exec(n);
    }
    depth--;
    afree(&ar);
    return r;
}

long lang_eval(const char *text)
{
    long r;
    int top = !depth;
    if (!t0) t0 = os_ticks() | 1;
    if (top) { aborted = 0; sh_break = sh_continue = sh_return = 0; }   /* csh COMMAND, from run.c */
    r = run_text(text, strlen(text));
    if (top) {
        if (aborted) { r = 20; setrc(r); }
        if (sh_exiting) { sh_exiting = 0; r = sh_exitrc; }
        aborted = 0; sh_break = sh_continue = sh_return = 0;
    }
    return r;
}

/* 1 = complete, 0 = more lines wanted, -1 = a syntax error (said) */
int lang_parse_check(const char *text)
{
    Arena ar = { NULL };
    Lex L;
    int r = 1;
    memset(&L, 0, sizeof L);
    L.src = text; L.n = strlen(text); L.ar = &ar;
    while (!L.error && !L.incomplete) {
        while (peek(&L)->type == T_NL) next(&L);
        if (peek(&L)->type == T_EOF) break;
        parse_line(&L);
    }
    if (L.error) r = -1;
    else if (L.incomplete) r = 0;
    afree(&ar);
    return r;
}

/* ---- the prompt ------------------------------------------------------------------- */

int lang_more;
static char *pend;
static long pendlen, pendcap;

int lang_line(const char *line, int interactive)
{
    long n = strlen(line);
    int ok;
    if (!t0) t0 = os_ticks() | 1;
    if (pendlen + n + 2 > pendcap) {
        long cap = (pendlen + n + 2) * 2;
        char *p = os_alloc(cap);
        if (!p) return 0;
        if (pend) { memcpy(p, pend, pendlen); os_free(pend); }
        pend = p; pendcap = cap;
    }
    memcpy(pend + pendlen, line, n);
    pendlen += n;
    if (!pendlen || pend[pendlen - 1] != '\n') pend[pendlen++] = '\n';
    pend[pendlen] = 0;
    ok = lang_parse_check(pend);
    if (ok == 0) { lang_more = 1; return 0; }
    lang_more = 0;
    if (ok > 0) {
        aborted = 0;
        sh_break = sh_continue = sh_return = 0;
        run_text(pend, pendlen);
        if (aborted) setrc(20);
        aborted = 0;
        sh_break = sh_continue = sh_return = 0;
        if (sh_exiting && interactive) { sh_exiting = 0; os_quit(sh_exitrc); }
        sh_exiting = 0;
    }
    pendlen = 0;
    return 1;
}

void lang_reset(void)
{
    pendlen = 0;
    lang_more = 0;
}

/* A script run as a command of its own (csh FROM, a #!csh file, csh
 * COMMAND): its traps are its own, its EXIT trap runs when it ends. */
long lang_script(const char *path, int argc, char **argv, const char *text)
{
    char *saved[TR_COUNT];
    long r;
    memcpy(saved, traps, sizeof saved);
    memset(traps, 0, sizeof traps);
    r = text ? lang_eval(text) : lang_source(path, argc, argv);
    lang_exittrap();
    traps_free();
    memcpy(traps, saved, sizeof saved);
    return r;
}

char *lang_expandstr(const char *s)
{
    Arena ar = { NULL };
    Exp e;
    char *r;
    memset(&e, 0, sizeof e); e.ar = &ar; e.cur.ar = &ar;
    expand_into(&e, s, strlen(s), 1);
    aborted = 0;
    if ((r = os_alloc(e.cur.n + 1)) && e.cur.s) memcpy(r, e.cur.s, e.cur.n);
    afree(&ar);
    return r;
}

/* read -a, mapfile: an array from words */
void sh_setarray(const char *name, int argc, char **argv)
{
    Arr *a;
    int i;
    if (ro_refuse(name) || !(a = arr_make(name, 0))) return;
    for (i = 0; i < argc; i++) arr_push(a, argv[i]);
}

/* unset -f */
int sh_unsetfunc(const char *name)
{
    Func **p;
    for (p = &funcs; *p; p = &(*p)->next)
        if (!strcmp((*p)->name, name)) {
            Func *f = *p;
            *p = f->next;
            if (f->busy) f->dead = 1; else funcfree(f);
            return 1;
        }
    return 0;
}

void lang_end(void)
{
    Func *f, *n;
    Arr *a, *an;
    RO *o, *on;
    for (f = funcs; f; f = n) { n = f->next; if (!f->busy) funcfree(f); }
    funcs = NULL;
    for (a = arrs; a; a = an) { an = a->next; arr_drop(a); }
    arrs = NULL;
    for (o = ros; o; o = on) { on = o->next; os_free(o); }
    ros = NULL;
    execredir_end();
    traps_free();
    os_free(hookbuf); hookbuf = NULL; hookn = hookcap = 0;
    afree(&posar);
    pos = NULL; npos = 0;
    if (pend) os_free(pend);
    pend = NULL; pendlen = pendcap = 0;
}

/* ---- scripts ------------------------------------------------------------------------ */

long lang_source(const char *path, int argc, char **argv)
{
    if (!t0) t0 = os_ticks() | 1;
    long len, r;
    char *text = os_readfile(path, &len), **opos = pos, *opos0 = pos0, *name;
    int onpos = npos, top = !depth;
    Arena oposar = posar, nm = { NULL };
    if (!text) { sh_error("cannot read", path); return 20; }
    if (top) { aborted = 0; sh_break = sh_continue = sh_return = 0; }   /* from the Amiga's rules: a fresh start */
    name = astr(&nm, path, strlen(path));          /* $0: the caller's path may be a static buffer */
    posar.head = NULL;
    sh_setpos(argc, argv);
    pos0 = name ? name : "csh";
    sourcing++;
    r = run_text(text, len);
    sourcing--;
    afree(&posar);
    posar = oposar;
    pos = opos; npos = onpos; pos0 = opos0;
    afree(&nm);
    if (top) { if (aborted) r = 20; aborted = 0; sh_break = sh_continue = 0; }
    if (sh_exiting) { sh_exiting = 0; r = sh_exitrc; }
    sh_return = 0;
    os_free(text);
    setrc(r);
    return r;
}

/* csh FROM file [args], csh COMMAND text, csh -c text, typed in csh (its
 * language or the Amiga's rules): run here, in a subshell - what bash's
 * "bash -c" and "bash file" give - without starting csh as a command (csh
 * is the system Shell: the file cannot be both). A #!csh file is sourced,
 * any other an Amiga script (Execute). csh alone: there is nothing to start. */
long lang_csh(int argc, char **argv)
{
    Arena ar = { NULL };
    Str t;
    int i = 1;
    long r;
    memset(&t, 0, sizeof t); t.ar = &ar;
    if (argc < 2) {
        sh_print("csh: this is csh (NewShell opens another Shell; csh -c TEXT, csh FROM FILE)\n");
        return 0;
    }
    if ((!strcmp(argv[1], "-c") || veq(argv[1], "COMMAND")) && argc > 2) {
        sput(&t, "(\n", 2);
        for (i = 2; i < argc; i++) { if (i > 2) sputc(&t, ' '); sput(&t, argv[i], strlen(argv[i])); }
        sput(&t, "\n)", 2);
    } else {
        const char *f;
        if (veq(argv[1], "FROM") && argc > 2) i = 2;
        f = argv[i++];
        sput(&t, lang_isscript(f) ? "( source " : "( Execute ", lang_isscript(f) ? 9 : 10);
        shq(&t, f);
        for (; i < argc; i++) { sputc(&t, ' '); shq(&t, argv[i]); }
        sput(&t, " )", 2);
    }
    sputc(&t, 0);
    r = lang_eval(t.s);
    afree(&ar);
    return r;
}

int lang_isscript(const char *path)
{
    char t[6];
    int len = os_head(path, t, 6);       /* the first line's start only, not the whole file */
    return len >= 5 && !memcmp(t, "#!csh", 5) && (len == 5 || t[5] == '\n' || t[5] == ' ' || t[5] == '\r');
}

/* An s-bit file starting with #!csh, typed as a command: its arguments
 * from the Amiga command line (words, "quoted words"). */
long lang_runscript(const char *path, struct cmdline *c)
{
    Arena ar = { NULL };
    char **argv = aalloc(&ar, 64 * sizeof *argv), *p = c->args;
    int argc = 0;
    long r;
    while (argv && argc < 63) {
        Str w;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        memset(&w, 0, sizeof w); w.ar = &ar;
        if (*p == '"') {
            for (p++; *p && *p != '"' && *p != '\n'; p++) {
                if (*p == '*' && p[1] && p[1] != '\n') { p++; sputc(&w, *p == 'N' || *p == 'n' ? '\n' : *p == 'E' || *p == 'e' ? 27 : *p); }
                else sputc(&w, *p);
            }
            if (*p == '"') p++;
        } else while (*p && *p != ' ' && *p != '\t' && *p != '\n') sputc(&w, *p++);
        argv[argc++] = w.s ? w.s : astr(&ar, "", 0);
    }
    {
        /* a pipe's last stage: the pipe is its input (job.c's c->inh) */
        void *sel = c->inh && !c->in[0] ? os_select(c->inh, 0) : NULL, *s = NULL;
        if (hasredir(c) && !(s = os_redir(c))) r = 20;
        else r = lang_script(path, argc, argv, NULL);
        if (s) os_unredir(s);
        if (sel) os_unselect(sel);
    }
    afree(&ar);
    return r;
}
