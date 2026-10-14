/* edit.c - csh edits the prompt line itself (docs/design.md "The prompt"):
 * the console is raw while a line is typed (term.c), the line is drawn
 * with escape sequences on the row(s) it stands on, and cooked again before
 * anything runs. So it behaves the same in CTerm and in a plain Shell
 * window, and never fights the console's own editor.
 *
 *  - the line coloured as it is typed: the command word by what it is
 *    (comp.c: green found, red unknown, cyan built in), strings, $variables,
 *    options, redirections, comments (colours in csh.prefs);
 *  - a grey suggestion from the history while typing at the end (fish);
 *    Right or End at the end takes it, Alt+Right a word of it;
 *  - Up/Down through the history, only the lines starting with what was
 *    typed (zsh's up-line-or-search); Ctrl+R searches it;
 *  - Tab completes (comp.c);
 *  - every key from csh.keys (keys.c).
 *
 * Drawing: what the line looks like is made into a string (escape
 * sequences and all); when the old one is the start of the new one and the
 * cursor was at the end, only the new part is sent - so plain typing costs
 * a character or two, which counts on a 14 MHz console. Otherwise the line
 * is drawn again from the prompt's first row. Lines longer than the window
 * wrap; the rows are worked out from the window's width (term_cols). */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

static char buf[LINE_MAX], kill_[LINE_MAX], orig[LINE_MAX];
static int len, cur;
static const char *prompt;
static int plen;                 /* the prompt's visible width */
static int currow;               /* the cursor's row now, counted from the prompt's first */
static int drawnlen;             /* visible width drawn after the prompt (line + suggestion) */
#define RMAX (LINE_MAX * 4)      /* the line and suggestion (2 KB at most) and their colours */
static char last[RMAX];          /* what was drawn: the line's string, escapes and all */
static int lastn, lastvalid;
static char sug[LINE_MAX];       /* the suggestion's rest */
static int suglen;
static int hidx, origlen, navigating, judged;
static char *pending;            /* the rest of a pasted text, one line a prompt */
const char *edit_rprompt;        /* RPROMPT (main.c), NULL = none */
static int rplen;
static int ungot;                /* a key read and given back (Ctrl+R ended by it) */

static int getkey(void)
{
    int k = ungot;
    if (k) { ungot = 0; return k; }
    return term_key();
}

static int vislen(const char *s)
{
    int n = 0;
    while (*s) {
        UBYTE c = *s;
        if (c == 0x1b || c == 0x9b) {
            s++;
            if (c == 0x1b && *s == '[') s++;
            else if (c == 0x1b) { if (*s) s++; continue; }
            while (*s && !((UBYTE)*s >= 0x40 && (UBYTE)*s <= 0x7e)) s++;
            if (*s) s++;
            continue;
        }
        if (c >= 32 && c != 0x7f) n++;
        s++;
    }
    return n;
}

/* ---- the line's colours -------------------------------------------------- */

static int curcol = -1;
static char *ro;
static int rn, rleft;            /* rleft: the line's characters still to come */

static void rput(const char *s, int n)
{
    if (rn + n >= RMAX) return;
    memcpy(ro + rn, s, n); rn += n;
}

/* A colour change: only while the text still to come fits after it (a
 * long line of $vars and quotes runs out of colours, never of text). */
static void colour(int c)
{
    const char *s = c >= 0 ? prefs_sgr(c) : "";
    if (c == curcol) return;
    if (rn + (int)strlen(s) + 8 + rleft >= RMAX) c = -1, s = "";
    if (c == curcol) return;
    if (curcol >= 0 && *prefs_sgr(curcol)) rput("\x1b[0m", 4);
    curcol = c;
    if (*s) rput(s, strlen(s));
}

/* # starts a comment at a word's start, except an Amiga pattern (#? #( #[ ## #~), as lang.c */
static int commentat(int i)
{
    char d = i + 1 < len ? buf[i + 1] : 0;
    return (i == 0 || buf[i - 1] == ' ' || buf[i - 1] == '\t') && d != '?' && d != '(' && d != '[' && d != '#' && d != '~';
}

static int iskw(const char *w, int n)
{
    static const char *const kw[] = { "if", "then", "else", "elif", "fi", "for", "in", "while", "until",
        "do", "done", "case", "esac", "function", "return", "break", "continue", "!", "{", "}",
        "select", "[[", "]]", NULL };
    int i;
    for (i = 0; kw[i]; i++) if ((int)strlen(kw[i]) == n && !memcmp(kw[i], w, n)) return 1;
    return 0;
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

/* The line (and the suggestion) as a string to send, into out, without
 * the closing colour reset: *endcol is the colour it ends in. */
static int render(char *out, int *endcol)
{
    int i = 0, cmdplace = 1, forname = 0;   /* forname: counts down from for/case to its "in" */
    ro = out; rn = 0; curcol = -1;
    while (i < len) {
        char c = buf[i];
        int s = i;
        rleft = len - i + suglen;
        if (c == ' ' || c == '\t') { colour(-1); rput(buf + i, 1); i++; continue; }
        if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')') {
            while (i < len && (buf[i] == ';' || buf[i] == '|' || buf[i] == '&' || buf[i] == '(' || buf[i] == ')')) i++;
            colour(C_REDIR); rput(buf + s, i - s);
            cmdplace = 1;
            continue;
        }
        if (c == '#' && commentat(i)) {
            colour(C_COMMENT); rput(buf + i, len - i); i = len; break;
        }
        if (c == '>' || c == '<' || (c >= '0' && c <= '2' && i + 1 < len && buf[i + 1] == '>') || (c == '*' && i + 1 < len && buf[i + 1] == '>')) {
            while (i < len && (buf[i] == '>' || buf[i] == '<' || buf[i] == '&' || ((buf[i] == '*' || (buf[i] >= '0' && buf[i] <= '2')) && i == s))) i++;
            colour(C_REDIR); rput(buf + s, i - s);
            continue;
        }
        /* a word: to the next space or operator outside quotes */
        {
            int q = 0, e = i, k, kind;
            while (e < len) {
                char d = buf[e];
                if (q) { if (d == q) q = 0; else if (d == '*' && q == '"' && e + 1 < len) e++; else if (d == '\\' && q == '"' && e + 1 < len) e++; }
                else if (d == '"' || d == '\'') q = d;
                else if (d == '\\' && e + 1 < len) e++;
                else if (d == ' ' || d == '\t' || d == ';' || d == '|' || d == '&' || d == '>' || d == '<' || d == '(' || d == ')') break;
                e++;
            }
            if (cmdplace) {
                if (iskw(buf + i, e - i)) {
                    /* for NAME, case WORD, function NAME: a name next, not a command */
                    int n = e - i;
                    if ((n == 3 && !memcmp(buf + i, "for", 3)) || (n == 4 && !memcmp(buf + i, "case", 4)) ||
                        (n == 8 && !memcmp(buf + i, "function", 8))) { cmdplace = 0; forname = n != 8 ? 3 : 0; }
                    colour(C_BUILTIN); rput(buf + i, e - i); i = e; continue;
                }
                if (isassign(buf + i, e - i)) {
                    for (k = i; buf[k] != '='; k++) ;
                    colour(C_VAR); rput(buf + i, k - i + 1); i = k + 1;
                    /* the value as an argument, the command still to come */
                } else {
                    /* the word being typed: no disk asked about it (a key costs
                     * nothing), and not called unknown yet */
                    int typing = e == len && cur == len && !judged;
                    kind = comp_kind(buf + i, e - i, typing);
                    if (kind == W_UNKNOWN && typing) kind = W_NONE;
                    colour(kind == W_COMMAND ? C_COMMAND : kind == W_BUILTIN ? C_BUILTIN : kind == W_UNKNOWN ? C_UNKNOWN : -1);
                    rput(buf + i, e - i);
                    i = e;
                    cmdplace = 0;
                    continue;
                }
            }
            /* an argument: strings, variables, options; for's and case's "in" a keyword */
            if (forname) forname--;
            if (forname == 1 && e - i == 2 && !memcmp(buf + i, "in", 2) && i == s) { forname = 0; colour(C_BUILTIN); rput(buf + i, 2); i = e; continue; }
            if (buf[i] == '-' && e - i > 1 && i == s) { colour(C_OPTION); rput(buf + i, e - i); i = e; continue; }
            q = 0;
            while (i < e) {
                char d = buf[i];
                if (!q && (d == '"' || d == '\'')) { q = d; colour(C_STRING); rput(buf + i, 1); i++; continue; }
                if (q && d == q) { rput(buf + i, 1); i++; q = 0; colour(-1); continue; }
                if (d == '$' && q != '\'') {
                    int v = i + 1;
                    if (v < e && (buf[v] == '{' || buf[v] == '(')) {
                        char close = buf[v] == '{' ? '}' : ')';
                        while (v < e && buf[v] != close) v++;
                        if (v < e) v++;
                    } else while (v < e && ((buf[v] >= 'a' && buf[v] <= 'z') || (buf[v] >= 'A' && buf[v] <= 'Z') || (buf[v] >= '0' && buf[v] <= '9') || buf[v] == '_' || buf[v] == '?' || buf[v] == '#')) v++;
                    colour(C_VAR); rput(buf + i, v - i); i = v;
                    colour(q ? C_STRING : -1);
                    continue;
                }
                colour(q ? C_STRING : -1);
                rput(buf + i, 1);
                i++;
            }
        }
    }
    rleft = suglen;
    if (suglen) { colour(C_SUGGEST); rput(sug, suglen); }
    *endcol = curcol;
    ro[rn] = 0;
    return rn;
}

/* ---- the suggestion --------------------------------------------------------- */

static void suggest(void)
{
    int i, n = hist_count();
    suglen = 0;
    if (!prefs_suggest || !len || cur != len || navigating) return;
    for (i = n - 1; i >= 0; i--) {
        const char *h = hist_get(i);
        if (!strncmp(h, buf, len) && h[len]) {
            suglen = strlen(h + len);
            if (suglen >= (int)sizeof sug) suglen = sizeof sug - 1;
            memcpy(sug, h + len, suglen);
            sug[suglen] = 0;
            return;
        }
    }
}

/* ---- drawing ----------------------------------------------------------------- */

static int lastpos;

static void cursorto(int pos)          /* from the end of what was just drawn */
{
    int W = term_cols, T = plen + drawnlen, endrow = T / W, row = pos / W, col = pos % W;
    currow = row;
    lastpos = pos;
    if (pos == T) return;               /* drawing left it there */
    term_move(endrow - row, 'A');
    term_out("\r", 1);
    term_move(col, 'C');
}

static void reset(int c)
{
    if (c >= 0 && *prefs_sgr(c)) term_outs("\x1b[0m");
}

/* Where the new string first differs from what was drawn: the byte (at
 * the start of an escape sequence, never inside one), its visible column
 * after the prompt, and the colour escape in force there (*sgr, *sgrn;
 * none = the plain colour). */
static int firstdiff(const char *now, int n, int *vis, const char **sgr, int *sgrn)
{
    int i = 0, v = 0;
    *sgr = NULL; *sgrn = 0;
    while (i < n) {
        int t = i;
        UBYTE c = now[i];
        if (c == 0x1b || c == 0x9b) {           /* an escape: one token */
            t++;
            if (c == 0x1b && t < n && now[t] == '[') t++;
            while (t < n && !((UBYTE)now[t] >= 0x40 && (UBYTE)now[t] <= 0x7e)) t++;
            if (t < n) t++;
        } else t++;
        if (t > lastn || memcmp(now + i, last + i, t - i)) break;
        if (c == 0x1b || c == 0x9b) {
            if (t - i == 4 && !memcmp(now + i, "\x1b[0m", 4)) { *sgr = NULL; *sgrn = 0; }
            else { *sgr = now + i; *sgrn = t - i; }
        } else if (c >= 32 && c != 0x7f) v++;
        i = t;
    }
    *vis = v;
    return i;
}

/* RPROMPT at the right edge of the line's row, while the line leaves room
 * (it goes when the line reaches it, as in zsh); the cursor back after */
static void rprompt(int T, int W)
{
    if (!rplen || T >= W || T + rplen + 1 >= W) return;
    term_move(W - rplen - T - 1, 'C');
    term_outs(edit_rprompt);
    term_move(W - T - 1, 'D');
}

static void draw(void)
{
    static char now[RMAX];
    int n, endcol, W = term_cols, T;
    suggest();
    n = render(now, &endcol);
    T = plen + vislen(now);
    if (lastvalid && (n != lastn || memcmp(now, last, n))) {
        /* only from the first change on: the cursor there, the colour in
         * force there, the rest, the screen cleared after it (a key while
         * a grey suggestion shows costs a few bytes, not the whole line) */
        const char *sgr;
        int sgrn, v, at = firstdiff(now, n, &v, &sgr, &sgrn), pos = plen + v;
        int row = pos / W, nowrow = lastpos / W;
        if (nowrow > row) term_move(nowrow - row, 'A');
        else if (row > nowrow) term_move(row - nowrow, 'B');
        term_out("\r", 1);
        term_move(pos % W, 'C');
        if (sgr) term_out(sgr, sgrn);
        term_out(now + at, n - at);
        reset(endcol);
        if (T > 0 && T % W == 0) term_outs(" \b");   /* onto the next row, whichever way it wraps */
        term_outs("\x1b[J");
        rprompt(T, W);
    } else if (!lastvalid) {
        term_move(currow, 'A');
        term_out("\r", 1);
        term_outs(prompt);
        term_out(now, n);
        reset(endcol);
        if (T > 0 && T % W == 0) term_outs(" \b");   /* onto the next row, whichever way it wraps */
        term_outs("\x1b[J");
        rprompt(T, W);
    } else {                                    /* the same: only the cursor moves */
        drawnlen = T - plen;
        if (lastpos != plen + cur) {
            int row = (plen + cur) / W, nowrow = lastpos / W;
            if (nowrow > row) term_move(nowrow - row, 'A');
            else if (row > nowrow) term_move(row - nowrow, 'B');
            term_out("\r", 1);
            term_move((plen + cur) % W, 'C');
            currow = row;
            lastpos = plen + cur;
        }
        return;
    }
    memcpy(last, now, n + 1); lastn = n; lastvalid = 1;
    drawnlen = T - plen;
    cursorto(plen + cur);
}

/* A fresh start on the row the cursor is on. */
static void fresh(void)
{
    currow = 0; drawnlen = 0; lastvalid = 0;
}

/* ---- editing --------------------------------------------------------------- */

static int isword(char c)
{
    return c && c != ' ' && c != '\t' && c != '/' && c != ':' && c != ';' && c != '|' && c != '"' && c != '=';
}

static void insert(const char *s, int n)
{
    if (len + n >= (int)sizeof buf - 2) n = sizeof buf - 2 - len;
    if (n <= 0) return;
    memmove(buf + cur + n, buf + cur, len - cur);
    memcpy(buf + cur, s, n);
    len += n; cur += n;
    buf[len] = 0;
}

static void delrange(int a, int b, int keep)
{
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a >= b) return;
    if (keep) { memcpy(kill_, buf + a, b - a); kill_[b - a] = 0; }
    memmove(buf + a, buf + b, len - b);
    len -= b - a;
    if (cur > b) cur -= b - a; else if (cur > a) cur = a;
    buf[len] = 0;
}

static int wordleft(int p)
{
    while (p > 0 && !isword(buf[p - 1])) p--;
    while (p > 0 && isword(buf[p - 1])) p--;
    return p;
}

static int wordright(int p)
{
    while (p < len && !isword(buf[p])) p++;
    while (p < len && isword(buf[p])) p++;
    return p;
}

static void setline(const char *s)
{
    int n = strlen(s);
    if (n >= (int)sizeof buf - 1) n = sizeof buf - 2;
    memcpy(buf, s, n); buf[n] = 0;
    len = cur = n;
}

/* Up / Down: the lines starting with what was typed before the first Up. */
static void histmove(int dir)
{
    int n = hist_count(), i;
    if (!navigating) {
        memcpy(orig, buf, len + 1); origlen = len;
        hidx = n; navigating = 1;
    }
    for (i = hidx + dir; i >= 0 && i < n; i += dir) {
        const char *h = hist_get(i);
        if (!strncmp(h, orig, origlen) && strcmp(h, buf)) { hidx = i; setline(h); return; }
    }
    if (dir > 0) { hidx = n; setline(orig); navigating = 0; return; }
    term_outs("\a");
}

static void lastarg(void)
{
    int n = hist_count(), e, s;
    const char *h;
    if (!n) return;
    h = hist_get(n - 1);
    e = strlen(h);
    while (e > 0 && (h[e - 1] == ' ' || h[e - 1] == '\t')) e--;
    s = e;
    if (s > 0 && h[s - 1] == '"') { s--; while (s > 0 && h[s - 1] != '"') s--; if (s > 0) s--; }
    else while (s > 0 && h[s - 1] != ' ' && h[s - 1] != '\t') s--;
    insert(h + s, e - s);
}

/* Ctrl+R: the newest line holding what is typed; Ctrl+R again an older one. */
static int search(void)
{
    static char pat[128];
    const char *save = prompt;
    char sp[200];
    int np = 0, at = hist_count(), found = -1, k, i, savelen = len;
    char saved[LINE_MAX];
    memcpy(saved, buf, len + 1);
    pat[0] = 0;
    for (;;) {
        strcpy(sp, "(search \"");
        strncat(sp, pat, 100);
        strcat(sp, found < 0 && np ? "\" not found) " : "\") ");
        prompt = sp; plen = vislen(sp);
        navigating = 1;           /* no suggestion while searching */
        lastvalid = 0;            /* the prompt changed: drawn whole */
        draw();
        k = getkey();
        if (k == (TM_CTRL | 'r')) {
            for (i = (found >= 0 ? found : at) - 1; i >= 0; i--) if (np && strstr(hist_get(i), pat)) break;
            if (i >= 0) { found = i; setline(hist_get(i)); } else term_outs("\a");
            continue;
        }
        if (k == K_BS) { if (np) pat[--np] = 0; found = -1; }
        else if (k >= 32 && k < 256 && k != 127 && np < 100) { pat[np++] = k; pat[np] = 0; found = -1; }
        else {
            int act = keys_find(k);
            prompt = save; plen = vislen(save); navigating = 0;
            lastvalid = 0;
            if (act == A_CANCEL) { memcpy(buf, saved, savelen + 1); len = cur = savelen; draw(); return 0; }
            if (act == A_ACCEPT) return 1;
            draw();
            ungot = k;            /* any other key ends the search and does what it does */
            return 0;
        }
        if (found < 0 && np) {
            for (i = at - 1; i >= 0; i--) if (strstr(hist_get(i), pat)) break;
            if (i >= 0) { found = i; setline(hist_get(i)); }
        }
    }
}

static void finish(void)
{
    /* the suggestion taken off, the cursor below the line */
    suglen = 0; navigating = 1; judged = 1;
    cur = len;
    draw();
    navigating = 0; judged = 0;
    term_outs("\n");
    term_flush();
}

/* Pasted text into the line: a tab is a space, other control bytes
 * (an escape sequence's ESC too) left out - the cursor's place is counted
 * by characters that show. */
static void insertpaste(const char *t, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = t[i] == '\t' ? ' ' : t[i];
        if ((UBYTE)c < 32 || c == 0x7f || (UBYTE)c == 0x9b) continue;
        insert(&c, 1);
    }
}

/* The rest of a paste of several lines: dropped (Ctrl-C at the prompt or
 * in a command it ran). */
void edit_droppaste(void)
{
    if (pending) FreeVec(pending);
    pending = NULL;
}

void edit_end(void)
{
    edit_droppaste();
    comp_end();
    hist_end();
}

int edit_line(const char *p, char *line, int size)
{
    const char *nlp = strrchr(p, '\n');
    if (nlp) {
        /* a prompt of several rows (Prompt "%S*N> "): the rows before the
         * last once, the last one is what is drawn again */
        term_out(p, nlp + 1 - p);
        p = nlp + 1;
    }
    prompt = p;
    plen = vislen(p);
    rplen = edit_rprompt ? vislen(edit_rprompt) : 0;
    len = cur = 0; buf[0] = 0;
    navigating = 0; suglen = 0; ungot = 0;
    fresh();
    comp_forget();
    hist_refresh();
    if (pending) {      /* the next line of a paste */
        char *nl = strchr(pending, '\n');
        int n = nl ? nl - pending : (int)strlen(pending);
        insertpaste(pending, n);
        if (nl && nl[1]) memmove(pending, nl + 1, strlen(nl + 1) + 1);
        else { FreeVec(pending); pending = NULL; }
        if (nl) { finish(); goto done; }
    }
    draw();
    for (;;) {
        int k = getkey(), act;
        if (k == K_PASTE) {
            int n, rest;
            const char *t = term_paste(&n), *nl = memchr(t, '\n', n), *cr = memchr(t, '\r', n);
            if (!nl || (cr && cr < nl)) nl = cr;
            if (!nl) { insertpaste(t, n); navigating = 0; draw(); continue; }
            insertpaste(t, nl - t);
            if (*nl == '\r' && nl + 1 < t + n && nl[1] == '\n') nl++;     /* CR LF: one line end */
            rest = t + n - nl - 1;
            if (rest > 0 && (pending = AllocVec(rest + 1, MEMF_ANY))) {
                char *o = pending;
                const char *s;
                for (s = nl + 1; s < t + n; s++) {
                    if (*s == '\r') { *o++ = '\n'; if (s + 1 < t + n && s[1] == '\n') s++; }
                    else *o++ = *s;
                }
                *o = 0;
            }
            finish();
            break;
        }
        if (k == K_RESIZE) { term_outs("\n"); fresh(); draw(); continue; }
        act = keys_find(k);
        if (act == A_NONE) {
            if (k >= 32 && k < 256 && k != 127 && !(k & TM_MASK)) {
                char c = k;
                insert(&c, 1);
                navigating = 0;
                draw();
            }
            continue;
        }
        if (act != A_PREV && act != A_NEXT && act != A_FIRST && act != A_LAST) navigating = 0;
        switch (act) {
        case A_LEFT: if (cur > 0) cur--; break;
        case A_RIGHT:
            if (cur < len) cur++;
            else if (suglen) { insert(sug, suglen); }
            break;
        case A_SUGGEST: if (suglen) insert(sug, suglen); break;
        case A_WORDLEFT: cur = wordleft(cur); break;
        case A_WORDRIGHT:
            if (cur < len) cur = wordright(cur);
            else if (suglen) {
                int e = 0;
                while (e < suglen && !isword(sug[e])) e++;
                while (e < suglen && isword(sug[e])) e++;
                insert(sug, e);
            }
            break;
        case A_START: cur = 0; break;
        case A_END: if (cur < len) cur = len; else if (suglen) insert(sug, suglen); break;
        case A_BACKSPACE: if (cur > 0) delrange(cur - 1, cur, 0); break;
        case A_DELETE: delrange(cur, cur + 1, 0); break;
        case A_DELWORDLEFT: delrange(wordleft(cur), cur, 1); break;
        case A_DELWORDRIGHT: delrange(cur, wordright(cur), 1); break;
        case A_KILLEND: delrange(cur, len, 1); break;
        case A_KILLSTART: delrange(0, cur, 1); break;
        case A_YANK: insert(kill_, strlen(kill_)); break;
        case A_TRANSPOSE:
            if (cur > 0 && len > 1) {
                char t;
                if (cur == len) cur--;
                t = buf[cur - 1]; buf[cur - 1] = buf[cur]; buf[cur] = t;
                cur++;
            }
            break;
        case A_PREV: histmove(-1); break;
        case A_NEXT: if (navigating) histmove(1); break;
        case A_FIRST:       /* the oldest line; Down goes on from there */
            if (!navigating) { memcpy(orig, buf, len + 1); origlen = len; }
            navigating = 0;
            if (hist_count()) { setline(hist_get(0)); hidx = 0; navigating = 1; }
            break;
        case A_LAST:        /* back to the line being typed */
            if (navigating) { setline(orig); hidx = hist_count(); navigating = 0; }
            break;
        case A_LASTARG: lastarg(); break;
        case A_SEARCH: if (search()) { finish(); goto done; } continue;
        case A_COMPLETE:
            suglen = 0;
            if (comp_complete(buf, &len, &cur, sizeof buf - 1) && comp_listing()) fresh();
            break;
        case A_CLEAR: term_outs("\f"); fresh(); break;
        case A_CANCEL:      /* the line (and an unfinished command, and the rest of a paste) thrown away */
            edit_droppaste();
            suglen = 0; navigating = 1; cur = len; draw(); navigating = 0;
            term_outs("\n"); term_flush();
            return -2;
        case A_EOF:
            if (len) { delrange(cur, cur + 1, 0); break; }
            term_outs("\n"); term_flush();
            return -1;
        case A_ACCEPT: finish(); goto done;
        }
        draw();
    }
done:
    if (len >= size - 1) len = size - 2;
    memcpy(line, buf, len);
    line[len] = '\n'; line[len + 1] = 0;
    return len;
}
