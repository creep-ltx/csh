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

enum { T_EOF, T_WORD, T_NL, T_SEMI, T_DSEMI, T_AMP, T_AND, T_OR, T_PIPE, T_LP, T_RP, T_REDIR };
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
        if (c == ';') { L->pos += d == ';' ? 2 : 1; return t->type = d == ';' ? T_DSEMI : T_SEMI; }
        if (c == '&' && d == '&') { L->pos += 2; return t->type = T_AND; }
        if (c == '&' && d == '>') { L->pos += e == '>' ? 3 : 2; t->op = e == '>' ? R_BOTHAPP : R_BOTH; return t->type = T_REDIR; }
        if (c == '&') { L->pos++; return t->type = T_AMP; }
        if (c == '|' && d == '|') { L->pos += 2; return t->type = T_OR; }
        if (c == '|') { L->pos++; return t->type = T_PIPE; }
        if (c == '(') { L->pos++; return t->type = T_LP; }
        if (c == ')') { L->pos++; return t->type = T_RP; }
        if (c == '<') {
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
        if (isspace_(c) || c == '\n' || isop(c)) break;
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
    if (t->type == T_EOF || t->type == T_RP || t->type == T_DSEMI) return 1;
    for (i = 0; stops[i]; i++) if (isword(t, stops[i])) return 1;
    return 0;
}

/* ---- the tree --------------------------------------------------------------- */

enum { N_SIMPLE, N_LIST, N_AND, N_OR, N_NOT, N_PIPE, N_BG, N_IF, N_WHILE, N_UNTIL, N_FOR,
       N_CASE, N_GROUP, N_SUB, N_FUNC };

typedef struct Case { char **pats; int npats; struct Node *body; struct Case *next; } Case;

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
    char *src;                  /* a function's text */
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
            static const char *const names[] = { "end", "word", "newline", ";", ";;", "&", "&&", "||", "|", "(", ")", "redirection" };
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

static int isassign(const char *w, int n)
{
    int i;
    if (!n || !((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= 'A' && w[0] <= 'Z') || w[0] == '_')) return 0;
    for (i = 1; i < n; i++) {
        if (w[i] == '=') return 1;
        if (!((w[i] >= 'a' && w[i] <= 'z') || (w[i] >= 'A' && w[i] <= 'Z') || (w[i] >= '0' && w[i] <= '9') || w[i] == '_')) return 0;
    }
    return 0;
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

static Node *parse_command(Lex *L)
{
    Tok *t = peek(L);
    Node *n;
    if (L->error || L->incomplete) return NULL;
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
    if (isword(t, "for")) {
        Tok v;
        next(L);
        v = next(L);
        if (v.type != T_WORD) { fail(L, &v); return NULL; }
        n = mk(L, N_FOR, NULL, NULL);
        n->name = v.text;
        if (peek(L)->type == T_NL) skipnl(L);
        if (isword(peek(L), "in")) {
            next(L);
            n->haslist = 1;
            while (peek(L)->type == T_WORD) n->words = growv(L, n->words, n->nwords++, next(L).text);
        }
        if (peek(L)->type == T_SEMI || peek(L)->type == T_NL) next(L);
        skipnl(L);
        if (!expect(L, "do")) return NULL;
        n->b = parse_list(L);
        if (!expect(L, "done")) return NULL;
        return parse_compound_redirs(L, n);
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
            if (peek(L)->type == T_DSEMI) { next(L); skipnl(L); continue; }
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

static Node *parse_pipeline(Lex *L)
{
    int bang = 0;
    Node *n;
    if (isword(peek(L), "!")) { next(L); bang = 1; }
    n = parse_command(L);
    while (n && peek(L)->type == T_PIPE) {
        next(L);
        skipnl(L);
        n = mk(L, N_PIPE, n, parse_command(L));
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

int sh_opt_e, sh_opt_x;
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
typedef struct Func { struct Func *next; char *name; Node *body; Arena ar; int busy, dead; } Func;

/* local: the values a function's call put aside, given back at its end */
typedef struct Saved { struct Saved *next; char *name, *val; int had; } Saved;
static Saved **frame;

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

void sh_print(const char *s) { os_write(s, strlen(s)); }
void sh_printn(const char *s, int n) { os_write(s, n); }

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

/* ---- arithmetic: $(( )) -------------------------------------------------------- */

typedef struct { const char *p; int err, depth; } Ar;

static long a_expr(Ar *a);

static void a_sp(Ar *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }

/* A variable's value, itself arithmetic (x=y+1); its own frame, so a_prim's
 * stays small (it calls itself for each ( - ! ~). */
static long __attribute__((noinline)) a_var(Ar *a)
{
    static int nest;            /* x=x: a variable naming itself */
    char name[64], buf[64];
    int n = 0;
    long v = 0;
    while (wordch(*a->p) && n < 63) name[n++] = *a->p++;
    name[n] = 0;
    if (os_getvar(name, buf, sizeof buf) > 0) {
        int err = 0;
        if (nest >= 8) { a->err = 1; return 0; }
        nest++;
        v = arith(buf, &err);
        nest--;
        if (err) a->err = err;
    }
    return v;
}

static long a_prim1(Ar *a);

static long a_prim(Ar *a)
{
    long v;
    if (++a->depth > 100) { a->err = 1; return 0; }   /* ((((... or - - - -...: the stack */
    v = a_prim1(a);
    a->depth--;
    return v;
}

static long a_prim1(Ar *a)
{
    long v = 0;
    a_sp(a);
    if (*a->p == '(') {
        a->p++;
        v = a_expr(a);
        a_sp(a);
        if (*a->p == ')') a->p++; else a->err = 1;
        return v;
    }
    if (*a->p == '-') { a->p++; return -a_prim(a); }
    if (*a->p == '+') { a->p++; return a_prim(a); }
    if (*a->p == '!') { a->p++; return !a_prim(a); }
    if (*a->p == '~') { a->p++; return ~a_prim(a); }
    if (*a->p >= '0' && *a->p <= '9') {
        int base = 10;
        if (a->p[0] == '0' && (a->p[1] == 'x' || a->p[1] == 'X')) { base = 16; a->p += 2; }
        for (;;) {
            char c = *a->p;
            int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 99;
            if (d >= base) break;
            v = v * base + d;
            a->p++;
        }
        return v;
    }
    if ((*a->p >= 'a' && *a->p <= 'z') || (*a->p >= 'A' && *a->p <= 'Z') || *a->p == '_') return a_var(a);
    a->err = 1;
    return 0;
}

static long a_mul(Ar *a)
{
    long v = a_prim(a);
    for (;;) {
        a_sp(a);
        if (*a->p == '*' && a->p[1] != '*') { a->p++; v *= a_prim(a); }
        else if (*a->p == '/') { long d; a->p++; d = a_prim(a); if (!d) { a->err = 2; return 0; } v /= d; }
        else if (*a->p == '%') { long d; a->p++; d = a_prim(a); if (!d) { a->err = 2; return 0; } v %= d; }
        else return v;
    }
}

static long a_add(Ar *a)
{
    long v = a_mul(a);
    for (;;) {
        a_sp(a);
        if (*a->p == '+') { a->p++; v += a_mul(a); }
        else if (*a->p == '-') { a->p++; v -= a_mul(a); }
        else return v;
    }
}

static long a_cmp(Ar *a)
{
    long v = a_add(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '<' && a->p[1] == '=') { a->p += 2; v = v <= a_add(a); }
        else if (a->p[0] == '>' && a->p[1] == '=') { a->p += 2; v = v >= a_add(a); }
        else if (a->p[0] == '<' && a->p[1] != '<') { a->p++; v = v < a_add(a); }
        else if (a->p[0] == '>' && a->p[1] != '>') { a->p++; v = v > a_add(a); }
        else if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; v = v == a_add(a); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; v = v != a_add(a); }
        else return v;
    }
}

static long a_expr(Ar *a)
{
    long v = a_cmp(a);
    for (;;) {
        a_sp(a);
        if (a->p[0] == '&' && a->p[1] == '&') { long w; a->p += 2; w = a_cmp(a); v = v && w; }
        else if (a->p[0] == '|' && a->p[1] == '|') { long w; a->p += 2; w = a_cmp(a); v = v || w; }
        else return v;
    }
}

long arith(const char *expr, int *err)
{
    Ar a;
    long v;
    a.p = expr; a.err = 0; a.depth = 0;
    v = a_expr(&a);
    a_sp(&a);
    if (*a.p) a.err = 1;
    *err = a.err;
    return v;
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
    sputc(&e->cur, c);
}

static void qputs(Exp *e, const char *s, int n)
{
    int i;
    if (!e->patmode) { sput(&e->cur, s, n); return; }
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

static char *capture(Arena *ar, const char *text, int len)
{
    void *save = os_capture();
    char *out = NULL, *r;
    int n;
    if (!save) return astr(ar, "", 0);
    caprc = subshell(NULL, text, len);
    capcount++;
    n = os_captured(save, &out);
    if (!out) return astr(ar, "", 0);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    r = astr(ar, out, n);
    os_free(out);
    return r;
}

static int getparam(Exp *e, const char *name, int n, Str *val)
{
    char b[24];
    val->n = 0;
    if (n == 1 && name[0] == '?') { sput(val, b, ltoa_(rc, b)); return 1; }
    if (n == 1 && name[0] == '#') { sput(val, b, ltoa_(npos, b)); return 1; }
    if (n == 1 && name[0] == '$') { sput(val, b, ltoa_(os_number(), b)); return 1; }
    if (n == 1 && name[0] == '0') { sput(val, pos0, strlen(pos0)); return 1; }
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
        if ((l = os_getvar(nm, buf, sizeof buf)) < 0) return 0;
        sput(val, buf, l);
        return 1;
    }
}

static void expand_into(Exp *e, const char *s, int n, int inquotes);

/* ${...}: the inside, without the braces */
static void brace(Exp *e, const char *s, int n, int inquotes)
{
    Str val, w;
    int i = 0, set, len = 0;
    memset(&val, 0, sizeof val); val.ar = e->ar;
    memset(&w, 0, sizeof w); w.ar = e->ar;
    if (n > 1 && s[0] == '#') {       /* ${#name} */
        char b[24];
        getparam(e, s + 1, n - 1, &val);
        addtext(e, b, ltoa_(val.n, b), 1);
        return;
    }
    while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) i++;
    if (i == 0 && n > 0 && (s[0] == '?' || s[0] == '#' || s[0] == '$' || s[0] == '@' || s[0] == '*')) i = 1;
    set = getparam(e, s, i, &val);
    len = val.n;
    if (i == n) { if (set) addtext(e, val.s ? val.s : "", len, inquotes); return; }
    {
        int colon = s[i] == ':';
        char op = s[i + colon];
        const char *word = s + i + colon + 1;
        int wl = n - (i + colon + 1);
        int empty = !set || (colon && len == 0);
        Exp sub;
        if (op == '-' || op == '=' || op == '+' || op == '?') {
            if ((op == '+') ? !empty : empty) {
                /* the word's own quotes come off: ${u:-'a b'} is a b */
                memset(&sub, 0, sizeof sub); sub.ar = e->ar; sub.cur.ar = e->ar;
                sub.sqlit = inquotes;
                expand_into(&sub, word, wl, 0);
                if (op == '=') {
                    char nm[128];
                    if (i < (int)sizeof nm) { memcpy(nm, s, i); nm[i] = 0; os_setvar(nm, sub.cur.s ? sub.cur.s : "", V_ASSIGN); }
                }
                if (op == '?') { sh_error(sub.cur.n ? sub.cur.s : "parameter not set", ""); aborted = 1; return; }
                addtext(e, sub.cur.s ? sub.cur.s : "", sub.cur.n, inquotes || sub.quoted);
            } else if (op != '+' && set) addtext(e, val.s ? val.s : "", len, inquotes);
            return;
        }
        if (s[i] == '#' || s[i] == '%') {
            int longest = s[i + 1] == s[i], k;
            const char *pat = s + i + 1 + longest;
            char none[1] = { 0 }, *v = val.s ? val.s : none, *p;
            memset(&sub, 0, sizeof sub); sub.ar = e->ar; sub.cur.ar = e->ar;
            sub.patmode = 1;
            expand_into(&sub, pat, n - (pat - s), 0);
            p = sub.cur.s ? sub.cur.s : "";
            if (s[i] == '#') {
                for (k = longest ? len : 0; longest ? k >= 0 : k <= len; k += longest ? -1 : 1) {
                    char c = v[k];
                    v[k] = 0;
                    if (gmatch(p, v, 0)) { v[k] = c; addtext(e, v + k, len - k, inquotes); return; }
                    v[k] = c;
                }
            } else {
                for (k = longest ? 0 : len; longest ? k <= len : k >= 0; k += longest ? 1 : -1)
                    if (gmatch(p, v + k, 0)) { addtext(e, v, k, inquotes); return; }
            }
            addtext(e, v, len, inquotes);
            return;
        }
    }
    sh_error("bad substitution:", "${...}");
    aborted = 1;
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
            if (err) { sh_error(err == 2 ? "division by zero in" : "bad arithmetic:", sub.cur.s ? sub.cur.s : ""); aborted = 1; }
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
    if (n >= 2 && inquotes && s[1] == '@') {             /* "$@": one word each */
        int i;
        for (i = 0; i < npos; i++) {
            if (i) endfield(e);
            sput(&e->cur, pos[i], strlen(pos[i]));
            e->have = 1; e->quoted = 1;
        }
        return 2;
    }
    if (n >= 2 && (s[1] == '?' || s[1] == '#' || s[1] == '$' || s[1] == '*' || s[1] == '@' || (s[1] >= '0' && s[1] <= '9'))) {
        Str v;
        memset(&v, 0, sizeof v); v.ar = e->ar;
        if (getparam(e, s + 1, 1, &v)) addtext(e, v.s ? v.s : "", v.n, inquotes);
        return 2;
    }
    {
        int i = 1;
        Str v;
        while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) i++;
        if (i == 1) { sputc(&e->cur, '$'); e->have = 1; return 1; }   /* a lone $ */
        memset(&v, 0, sizeof v); v.ar = e->ar;
        if (getparam(e, s + 1, i - 1, &v)) addtext(e, v.s ? v.s : "", v.n, inquotes);
        else if (inquotes) e->have = 1;
        return i;
    }
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
            expand_into(e, s + i + 1, j - i - 1, 1);
            i = j + 1;
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            if (s[i + 1] != '\n') { qput(e, s[i + 1]); e->quoted = 1; }
            e->have = 1;
            i += 2;
            continue;
        }
        if (c == '*' || c == '?') e->pattern = 1;
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
    if (f->pattern && !f->quoted && !(f->len == 1 && f->s[0] == '*')) {   /* *.txt -> #?.txt; * alone is the console */
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

static long __attribute__((noinline)) run_simple(Node *n)
{
    static char ab[LINE_MAX];   /* an alias's text: used up before anything runs */
    Arena ar = { NULL };
    Exp e;
    struct cmdline *c;
    char tmp[4][40];
    int ntmp = 0, i, b, caps;
    Old *old = NULL;
    long r = 0;

    memset(&e, 0, sizeof e);
    e.ar = &ar;
    for (i = 0; i < n->nwords && !aborted; i++) expand(&e, n->words[i], 1);
    if (aborted) { afree(&ar); return 1; }

    /* name=value alone: set; before a command: only for it */
    caps = capcount;
    for (i = 0; i < n->nassigns && !aborted; i++) {
        char *eq = strchr(n->assigns[i], '='), *name, *v;
        int nl = eq - n->assigns[i];
        name = astr(&ar, n->assigns[i], nl);
        v = expand1(&ar, eq + 1, NULL);
        if (!name || !v) { r = 20; goto done0; }
        if (e.nf) old = putaside(&ar, old, name);
        os_setvar(name, v, e.nf ? V_LOCAL : V_ASSIGN);   /* X=1 cmd: only for cmd */
    }
    if (aborted) { r = 1; goto done0; }
    if (!(c = aalloc(&ar, sizeof *c))) { r = 20; goto done0; }
    if (!redirs(&ar, n->redirs, c, tmp, &ntmp)) { r = 20; goto done; }

    if (sh_opt_x && e.nf) {
        Str t;
        memset(&t, 0, sizeof t); t.ar = &ar;
        sputc(&t, '+');
        for (i = 0; i < e.nf; i++) { sputc(&t, ' '); sput(&t, e.f[i].s, e.f[i].len); }
        sputc(&t, '\n');
        os_err(t.s, t.n);
    }
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
        argv = aalloc(&ar, (e.nf + 1) * sizeof *argv);
        if (!argv) { r = 20; goto done; }
        for (i = 0; i < e.nf; i++) argv[i] = e.f[i].s;

        /* an alias (the ROM's Alias, or alias x=...): its text, the rest after it */
        if (!e.f[0].quoted && naliasing < 8 && !inalias(name) && os_getalias(name, ab, sizeof ab) >= 0) {
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
        if ((f = findfunc(name))) {
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
                frame = outer;
                unframe(mine);
            }
            funcdone(f);
            depth--;
            if (sh_return) { sh_return = 0; r = rc; }
            afree(&posar);
            posar = oposar;
            pos = opos; npos = onpos; pos0 = opos0;
            if (s) os_unredir(s);
            goto done;
        }
        if ((b = builtin_find(name)) >= 0) {
            void *s = NULL;
            if (hasredir(c) && !(s = os_redir(c))) { r = 20; goto done; }
            r = builtin_run(b, e.nf, argv);
            if (s) os_unredir(s);
            if (r != -999) goto done;
        }
        /* an Amiga command */
        {
            Str a;
            memset(&a, 0, sizeof a); a.ar = &ar;
            for (i = 1; i < e.nf; i++) { if (i > 1) sputc(&a, ' '); amigaarg(&a, &e.f[i]); }
            if (a.n + 2 >= (int)sizeof c->args || e.f[0].len >= (int)sizeof c->cmd) { sh_error("line too long", ""); r = 10; goto done; }
            memcpy(c->cmd, e.f[0].s, e.f[0].len); c->cmd[e.f[0].len] = 0;
            if (a.s) memcpy(c->args, a.s, a.n);
            c->args[a.n] = '\n'; c->args[a.n + 1] = 0;
            c->arglen = a.n + 1;
            r = os_run(c);
        }
    }
done:
    for (i = 0; i < ntmp; i++) os_delete(tmp[i]);
done0:
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
    if (!aborted && os_break()) { os_err("***Break\n", 8); aborted = 1; }
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
static int flatten(Node *n, Node **st, int max)
{
    int k;
    if (n->type != N_PIPE) { st[0] = n; return 1; }
    k = flatten(n->a, st, max);
    if (k < 0 || k >= max) return -1;
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
static int stagecmd(Arena *ar, Node *n, struct cmdline *c, void *job)
{
    Exp e;
    Str a;
    char tmp[4][40];
    int ntmp = 0, i;
    memset(&e, 0, sizeof e); e.ar = ar;
    for (i = 0; i < n->nwords && !aborted; i++) expand(&e, n->words[i], 1);
    if (aborted || !e.nf) return 0;
    i = redirs(ar, n->redirs, c, tmp, &ntmp);
    { int k; for (k = 0; k < ntmp; k++) os_job_tmp(job, tmp[k]); }   /* made before one failed too */
    if (!i) return 0;
    memset(&a, 0, sizeof a); a.ar = ar;
    for (i = 1; i < e.nf; i++) { if (i > 1) sputc(&a, ' '); amigaarg(&a, &e.f[i]); }
    if (a.n + 2 >= (int)sizeof c->args || e.f[0].len >= (int)sizeof c->cmd) { sh_error("line too long", ""); return 0; }
    memcpy(c->cmd, e.f[0].s, e.f[0].len); c->cmd[e.f[0].len] = 0;
    if (a.s) memcpy(c->args, a.s, a.n);
    c->args[a.n] = '\n'; c->args[a.n + 1] = 0;
    c->arglen = a.n + 1;
    return 1;
}

/* a | b | c, and anything with &: an Amiga command runs in a process of
 * its own, joined to the next by PIPE:; csh's own stage runs in csh (the
 * last one reading the pipe as it comes, one in the middle into a T: file
 * the next one reads). The rc is the last stage's. */
static long __attribute__((noinline)) pipeline(Node *n, int bg)
{
    Arena ar = { NULL };
    Node *st[32];
    Str text;
    void *job;
    long in, r = 0;
    int ns, i, lastint = 0;
    memset(&text, 0, sizeof text); text.ar = &ar;
    if ((ns = flatten(n, st, 32)) < 0) { sh_error("a pipe of more than 32 commands", ""); afree(&ar); setrc(10); return 10; }
    nodetext(n, &text);
    if (!(job = os_job_new(text.s ? text.s : "", bg))) { afree(&ar); return 20; }
    in = os_job_in(job);
    for (i = 0; i < ns && !aborted; i++) {
        int last = i == ns - 1;
        if (internal(&ar, st[i])) {
            void *s;
            if (bg) { sh_error("csh's own commands cannot run with & (yet):", text.s); os_close(job, in); r = 10; break; }
            if (last) {
                s = os_select(in, 0);
                r = exec(st[i]);
                os_unselect(s);
                os_close(job, in);
                lastint = 1;
            } else {
                long out = os_tempout(job);
                if (!out) { os_close(job, in); r = 20; break; }
                s = os_select(in, out);
                exec(st[i]);
                os_unselect(s);
                os_close(job, out);
                os_close(job, in);
                in = os_tempin(job);
            }
        } else {
            struct cmdline *c = aalloc(&ar, sizeof *c);
            long out, next = 0;
            /* a stage that cannot start: the next one reads nothing (in 0 = NIL:) */
            if (!c || !stagecmd(&ar, st[i], c, job)) { os_close(job, in); in = 0; r = 10; if (last) break; continue; }
            if (sh_opt_x) {
                os_err("+ ", 2); os_err(c->cmd, strlen(c->cmd)); os_err(" ", 1);
                os_err(c->args, c->arglen);
            }
            if (last && !bg) {          /* in csh itself, as the ROM Shell does */
                r = os_lastrun(job, c, in);
                lastint = 1;
                break;
            }
            if (last) out = os_job_out(job);
            else if (!os_pipe(job, &next, &out)) { os_close(job, in); r = 20; break; }
            if (!os_spawn(job, c, in, out)) r = sh_rc();
            in = next;
        }
    }
    r = os_job_end(job, r, lastint);
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
    for (c = n->cases; c; c = c->next) {
        int i, hit = 0;
        for (i = 0; i < c->npats && !hit; i++) {
            Exp pe;
            memset(&pe, 0, sizeof pe); pe.ar = &ar; pe.patmode = 1;
            expand(&pe, c->pats[i], 0);
            hit = gmatch(pe.nf ? pe.f[0].s : "", w, 0);
        }
        if (hit) { r = exec(c->body); break; }
    }
    afree(&ar);
    if (!c) setrc(0);
    return r;
}

/* ( ... ) and $( ... ): not a subshell (no fork on the Amiga), but exit,
 * return, break and continue end only it, as they would end bash's
 * subshell; its exit code is its rc. */
static long subshell(Node *n, const char *text, int len)
{
    int ob = sh_break, oc = sh_continue, oloops = sh_loops;
    long r;
    sh_break = sh_continue = 0;
    sh_loops = 0;
    r = n ? exec(n) : run_text(text, len);
    if (sh_exiting) { sh_exiting = 0; r = sh_exitrc; setrc(r); }
    sh_return = 0;
    sh_break = ob; sh_continue = oc; sh_loops = oloops;
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
    case N_SIMPLE:
        r = run_simple(n);
        setrc(r);
        if (r && sh_opt_e && !incond && (sourcing || !os_interactive())) { sh_exiting = 1; sh_exitrc = r; }
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
        return pipeline(n, 0);
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

void lang_end(void)
{
    Func *f, *n;
    for (f = funcs; f; f = n) { n = f->next; if (!f->busy) funcfree(f); }
    funcs = NULL;
    afree(&posar);
    pos = NULL; npos = 0;
    if (pend) os_free(pend);
    pend = NULL; pendlen = pendcap = 0;
}

/* ---- scripts ------------------------------------------------------------------------ */

long lang_source(const char *path, int argc, char **argv)
{
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
        else r = lang_source(path, argc, argv);
        if (s) os_unredir(s);
        if (sel) os_unselect(sel);
    }
    afree(&ar);
    return r;
}
