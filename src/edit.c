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
static char last[LINE_MAX * 2];  /* what was drawn: the line's string, escapes and all */
static int lastn, lastvalid, lastsug;
static char sug[LINE_MAX];       /* the suggestion's rest */
static int suglen;
static int hidx, origlen, navigating;
static char *pending;            /* the rest of a pasted text, one line a prompt */

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
static int rn;

static void rput(const char *s, int n)
{
    if (rn + n >= (int)sizeof last) return;
    memcpy(ro + rn, s, n); rn += n;
}

static void colour(int c)
{
    const char *s;
    if (c == curcol) return;
    if (curcol >= 0 && *prefs_sgr(curcol)) rput("\x1b[0m", 4);
    curcol = c;
    if (c >= 0 && *(s = prefs_sgr(c))) rput(s, strlen(s));
}

static int iskw(const char *w, int n)
{
    static const char *const kw[] = { "if", "then", "else", "elif", "fi", "for", "in", "while", "until",
        "do", "done", "case", "esac", "function", "return", "break", "continue", "!", "{", "}", NULL };
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
    int i = 0, cmdplace = 1;
    ro = out; rn = 0; curcol = -1;
    while (i < len) {
        char c = buf[i];
        int s = i;
        if (c == ' ' || c == '\t') { colour(-1); rput(buf + i, 1); i++; continue; }
        if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')') {
            while (i < len && (buf[i] == ';' || buf[i] == '|' || buf[i] == '&' || buf[i] == '(' || buf[i] == ')')) i++;
            colour(C_REDIR); rput(buf + s, i - s);
            cmdplace = 1;
            continue;
        }
        if (c == '#' && (s == 0 || buf[s - 1] == ' ' || buf[s - 1] == '\t') && !(i + 1 < len && buf[i + 1] == '?')) {
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
                if (iskw(buf + i, e - i)) { colour(C_BUILTIN); rput(buf + i, e - i); i = e; continue; }
                if (isassign(buf + i, e - i)) {
                    for (k = i; buf[k] != '='; k++) ;
                    colour(C_VAR); rput(buf + i, k - i + 1); i = k + 1;
                    /* the value as an argument, the command still to come */
                } else {
                    kind = comp_kind(buf + i, e - i);
                    /* the word being typed is not judged yet unless it is known */
                    if (kind == W_UNKNOWN && e == len && cur == len) kind = W_NONE;
                    colour(kind == W_COMMAND ? C_COMMAND : kind == W_BUILTIN ? C_BUILTIN : kind == W_UNKNOWN ? C_UNKNOWN : -1);
                    rput(buf + i, e - i);
                    i = e;
                    cmdplace = 0;
                    continue;
                }
            }
            /* an argument: strings, variables, options */
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

static int lastend = -1, lastpos;

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

static void draw(void)
{
    static char now[LINE_MAX * 2];
    int n, endcol, W = term_cols, T, oldT = plen + drawnlen;
    suggest();
    n = render(now, &endcol);
    T = plen + vislen(now);
    if (lastvalid && !lastsug && !suglen && n >= lastn && !memcmp(now, last, lastn)
        && lastpos == oldT && (plen + cur) == T && oldT % W && T / W == oldT / W) {
        /* the old string is the start of the new: only the rest, in the colour it was in */
        if (lastend >= 0) term_outs(prefs_sgr(lastend));
        term_out(now + lastn, n - lastn);
        reset(endcol);
    } else {
        term_move(currow, 'A');
        term_out("\r", 1);
        term_outs(prompt);
        term_out(now, n);
        reset(endcol);
        if (T > 0 && T % W == 0) term_outs(" \b");   /* onto the next row, whichever way it wraps */
        term_outs("\x1b[J");
    }
    memcpy(last, now, n + 1); lastn = n; lastvalid = 1; lastsug = suglen != 0; lastend = endcol;
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
        draw();
        k = term_key();
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
            if (act == A_CANCEL) { memcpy(buf, saved, savelen + 1); len = cur = savelen; draw(); return 0; }
            if (act == A_ACCEPT) return 1;
            draw();
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
    suglen = 0; navigating = 1;
    cur = len;
    draw();
    navigating = 0;
    term_outs("\n");
    term_flush();
}

int edit_line(const char *p, char *line, int size)
{
    prompt = p;
    plen = vislen(p);
    len = cur = 0; buf[0] = 0;
    navigating = 0; suglen = 0;
    fresh();
    comp_forget();
    hist_refresh();
    if (pending) {      /* the next line of a paste */
        char *nl = strchr(pending, '\n');
        int n = nl ? nl - pending : (int)strlen(pending);
        insert(pending, n);
        if (nl && nl[1]) memmove(pending, nl + 1, strlen(nl + 1) + 1);
        else { FreeVec(pending); pending = NULL; }
        if (nl) { finish(); goto done; }
    }
    draw();
    for (;;) {
        int k = term_key(), act;
        if (k == K_PASTE) {
            int n;
            const char *t = term_paste(&n), *nl = memchr(t, '\n', n);
            if (!nl) nl = memchr(t, '\r', n);
            if (!nl) { insert(t, n); navigating = 0; draw(); continue; }
            insert(t, nl - t);
            if (nl + 1 < t + n && (pending = AllocVec(t + n - nl, MEMF_ANY))) {
                memcpy(pending, nl + 1, t + n - nl - 1);
                pending[t + n - nl - 1] = 0;
                { char *r; while ((r = strchr(pending, '\r'))) *r = '\n'; }
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
        case A_FIRST: navigating = 0; memcpy(orig, buf, len + 1); hidx = -1; if (hist_count()) { setline(hist_get(0)); hidx = 0; navigating = 1; } break;
        case A_LAST: navigating = 0; break;
        case A_LASTARG: lastarg(); break;
        case A_SEARCH: if (search()) { finish(); goto done; } continue;
        case A_COMPLETE:
            suglen = 0;
            if (comp_complete(buf, &len, &cur, sizeof buf - 1) && comp_listing()) fresh();
            break;
        case A_CLEAR: term_outs("\f"); fresh(); break;
        case A_CANCEL:
            suglen = 0; navigating = 1; cur = len; draw(); navigating = 0;
            term_outs("\n"); len = cur = 0; buf[0] = 0; fresh();
            break;
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
