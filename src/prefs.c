/* prefs.c - csh's settings, ENVARC:CShell/csh.prefs ("name value", ';'
 * starts a comment), written with the defaults when missing:
 *   history 500     lines kept
 *   suggest on      the grey suggestion from the history
 *   colour on       the line coloured as it is typed
 *   command 32,33   a colour: SGR numbers in CTerm, then in a plain Shell
 *                   window (its pens: 4 colours on a stock Workbench);
 *                   "-" = none. The program picks the kind, CTerm's
 *                   theme decides the shade. */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

#define PREFFILE "ENVARC:CShell/csh.prefs"
#define PREFENV  "ENV:CShell/csh.prefs"

int prefs_history = 500, prefs_suggest = 1, prefs_colour = 1;

static const char *const names[C_COUNT] = {
    "command", "unknown", "builtin", "string", "variable", "option", "comment",
    "redirection", "suggestion", "promptrc"
};
static char sgr[2][C_COUNT][16];

static const char defaults[] =
    "; csh's settings: one a line, NAME VALUE.\n"
    "history 500\n"
    "suggest on\n"
    "colour on\n"
    "; colours: SGR numbers in CTerm, then in a plain Shell window (its pens);\n"
    "; '-' = none. 31 red 32 green 33 yellow 34 blue 35 magenta 36 cyan 90 grey,\n"
    "; 1 bold 3 italic 4 underline, joined with ';' (1;32).\n"
    "command 32,33\n"
    "unknown 31,3\n"
    "builtin 36,33\n"
    "string 33,-\n"
    "variable 35,-\n"
    "option -,-\n"
    "comment 90,32\n"
    "redirection 1,1\n"
    "suggestion 90,32\n"
    "promptrc 31,1\n";

extern char *readall(const char *path, long max);
extern void writeall(const char *dir, const char *path, const char *text, long n);

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same(const char *a, const char *b)
{
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static void setsgr(char *d, const char *v, int n)
{
    if (n <= 0 || n > 10 || (n == 1 && *v == '-')) { d[0] = 0; return; }
    d[0] = 0x1b; d[1] = '[';
    memcpy(d + 2, v, n);
    d[n + 2] = 'm'; d[n + 3] = 0;
}

static int onoff(const char *v) { return same(v, "on") || same(v, "yes") || same(v, "1"); }

static void parse(char *text)
{
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n'), *k, *v, *e;
        int i;
        if (nl) *nl = 0;
        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k && *k != ';' && *k != '#') {
            for (e = k; *e && *e != ' ' && *e != '\t'; e++) ;
            v = e;
            while (*v == ' ' || *v == '\t') v++;
            *e = 0;
            for (e = v; *e && *e != ' ' && *e != '\t' && *e != '\r'; e++) ;
            *e = 0;
            if (same(k, "history")) { LONG n; if (StrToLong(v, &n) > 0 && n > 0 && n < 100000) prefs_history = n; }
            else if (same(k, "suggest")) prefs_suggest = onoff(v);
            else if (same(k, "colour") || same(k, "color")) prefs_colour = onoff(v);
            else for (i = 0; i < C_COUNT; i++) if (same(k, names[i])) {
                char *comma = strchr(v, ',');
                setsgr(sgr[0][i], v, comma ? comma - v : (int)strlen(v));
                if (comma) setsgr(sgr[1][i], comma + 1, strlen(comma + 1));
                else strcpy(sgr[1][i], sgr[0][i]);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

const char *prefs_sgr(int what)
{
    if (!prefs_colour) return "";
    return sgr[term_cterm > 0 ? 0 : 1][what];
}

void prefs_load(void)
{
    static char d[sizeof defaults];
    char *t;
    memcpy(d, defaults, sizeof defaults);
    parse(d);
    if (!(t = readall(PREFENV, 16384))) t = readall(PREFFILE, 16384);
    if (!t) {
        writeall("ENVARC:CShell", PREFFILE, defaults, sizeof defaults - 1);
        writeall("ENV:CShell", PREFENV, defaults, sizeof defaults - 1);
        return;
    }
    parse(t);
    FreeVec(t);
}
