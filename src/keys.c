/* keys.c - every key of csh's prompt is the person's: ENVARC:CShell/csh.keys,
 * one binding a line ("<C-r> search", "<A-Left> wordleft"; "none" takes a
 * key away; ';' starts a comment), the same form as Sumi.keys and Maki.keys.
 * Written with the defaults - bash's and fish's keys - when it is missing.
 *
 * Right Amiga keys belong to CTerm's menus and Left Amiga keys to its panes
 * (csh never sees them); Shift+arrows scroll CTerm's history. Ctrl-C..F
 * come as signals and term.c makes them keys; the A1200 has no Home or End,
 * so Ctrl+arrows and Ctrl+A / Ctrl+E do that. */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

#define KEYFILE "ENVARC:CShell/csh.keys"
#define KEYENV  "ENV:CShell/csh.keys"
#define MAXB    192

static const char *const actnames[A_COUNT] = {
    "none", "left", "right", "wordleft", "wordright", "start", "end",
    "backspace", "delete", "delwordleft", "delwordright", "killend", "killstart",
    "yank", "prev", "next", "search", "complete", "accept", "suggest", "suggestword",
    "clear", "cancel", "eof", "transpose", "first", "last", "lastarg"
};

static const char defaults[] =
    "; csh's keys: one a line, KEY ACTION. <C-x> is Ctrl+X, <A-x> Alt, <S-x> Shift.\n"
    "; Keys: <Up> <Down> <Left> <Right> <Home> <End> <PgUp> <PgDn> <Ins> <Del> <BS>\n"
    "; <Tab> <Enter> <Esc> <Help> <F1>-<F12>, or a character. 'none' takes a key away.\n"
    "; (Right Amiga keys are CTerm's menus, Left Amiga keys its panes: not here.)\n"
    ";\n"
    "; Actions: left right wordleft wordright start end backspace delete delwordleft\n"
    "; delwordright killend killstart yank prev next search complete accept suggest\n"
    "; suggestword clear cancel eof transpose first last lastarg\n"
    "\n; moving (right and wordright at the end take the grey suggestion)\n"
    "<Left> left\n<Right> right\n<C-b> left\n<C-f> right\n"
    "<A-Left> wordleft\n<A-Right> wordright\n<A-b> wordleft\n<A-f> wordright\n"
    "; (a plain Shell window has no Alt+arrows: Shift+arrows, the Amiga way)\n"
    "<S-Left> wordleft\n<S-Right> wordright\n<S-Up> first\n<S-Down> last\n"
    "<C-Left> start\n<C-Right> end\n<Home> start\n<End> end\n<C-a> start\n<C-e> end\n"
    "\n; deleting\n"
    "<BS> backspace\n<Del> delete\n<A-BS> delwordleft\n<C-w> delwordleft\n"
    "<A-d> delwordright\n<A-Del> delwordright\n<C-k> killend\n<C-u> killstart\n<C-y> yank\n"
    "<C-t> transpose\n"
    "\n; the history\n"
    "<Up> prev\n<Down> next\n<C-p> prev\n<C-n> next\n<C-r> search\n"
    "<C-Up> first\n<C-Down> last\n<PgUp> first\n<PgDn> last\n<A-.> lastarg\n"
    "\n; the rest\n"
    "<Tab> complete\n<Enter> accept\n<C-j> accept\n<C-l> clear\n<C-c> cancel\n<Esc> cancel\n"
    "<C-d> eof\n<C-\\> eof\n<F1> suggest\n";

typedef struct { int key, act; } Bind;
static Bind binds[MAXB];
static int nbinds;
char keys_bad[120];

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same(const char *a, const char *b)
{
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static const struct { const char *n; int k; } named[] = {
    { "Up", K_UP }, { "Down", K_DOWN }, { "Left", K_LEFT }, { "Right", K_RIGHT },
    { "Home", K_HOME }, { "End", K_END }, { "PgUp", K_PGUP }, { "PgDn", K_PGDN },
    { "Ins", K_INS }, { "Del", K_DEL }, { "BS", K_BS }, { "Tab", K_TAB },
    { "Enter", K_ENTER }, { "Return", K_ENTER }, { "Esc", K_ESC }, { "Help", K_HELP },
    { "Space", ' ' }, { NULL, 0 }
};

static int parsekey(const char *s)
{
    int m = 0, i;
    char name[16];
    const char *e;
    if (s[0] != '<' || !s[1]) return (unsigned char)s[0] && !s[1] ? (unsigned char)s[0] : 0;
    s++;
    if (!(e = strrchr(s, '>'))) return 0;
    while (e - s > 2 && s[1] == '-') {
        int c = lower((unsigned char)s[0]);
        if (c == 'c') m |= TM_CTRL;
        else if (c == 's') m |= TM_SHIFT;
        else if (c == 'a' || c == 'm') m |= TM_ALT;
        else return 0;
        s += 2;
    }
    if (e - s >= (int)sizeof name || e == s) return 0;
    memcpy(name, s, e - s);
    name[e - s] = 0;
    if (!name[1]) {
        int c = (unsigned char)name[0];
        if (m & TM_CTRL) c = lower(c);
        return c | m;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        int n = name[1] - '0';
        if (name[2] >= '0' && name[2] <= '9') n = n * 10 + name[2] - '0';
        return n >= 1 && n <= 12 ? (K_F1 + n - 1) | m : 0;
    }
    for (i = 0; named[i].n; i++) if (same(name, named[i].n)) return named[i].k | m;
    return 0;
}

static void addbind(int key, int act)
{
    int i;
    for (i = 0; i < nbinds; i++) if (binds[i].key == key) { binds[i].act = act; return; }
    if (nbinds < MAXB) { binds[nbinds].key = key; binds[nbinds].act = act; nbinds++; }
    else if (!keys_bad[0]) strcpy(keys_bad, "csh.keys: too many keys (the rest left out)");
}

static void parse(char *text)
{
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n'), *k, *a, *e;
        int key, act;
        if (nl) *nl = 0;
        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k && *k != ';' && *k != '#') {
            for (e = k; *e && *e != ' ' && *e != '\t'; e++) ;
            a = e;
            while (*a == ' ' || *a == '\t') a++;
            *e = 0;
            for (e = a; *e && *e != ' ' && *e != '\t' && *e != '\r'; e++) ;
            *e = 0;
            key = parsekey(k);
            for (act = 0; act < A_COUNT && !same(a, actnames[act]); act++) ;
            if (!key || act == A_COUNT) {
                if (!keys_bad[0]) { strcpy(keys_bad, "csh.keys: not understood: "); strncat(keys_bad, k, 40); }
            } else addbind(key, act);
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* Requesters off (a drawer or volume missing, a disk write-protected):
 * csh's own files are never worth a requester. */
static APTR quiet(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR w = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return w;
}

static void loud(APTR w) { ((struct Process *)FindTask(NULL))->pr_WindowPtr = w; }

char *readall(const char *path, long max)
{
    APTR w = quiet();
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char *t;
    long n;
    loud(w);
    if (!fh) return NULL;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    if (n < 0 || n > max || !(t = AllocVec(n + 1, MEMF_ANY))) { Close(fh); return NULL; }
    n = Read(fh, t, n);
    Close(fh);
    t[n > 0 ? n : 0] = 0;
    return t;
}

/* Writes text to path, making the drawer first. */
void writeall(const char *dir, const char *path, const char *text, long n)
{
    APTR w = quiet();
    BPTR l = Lock((STRPTR)dir, SHARED_LOCK), fh;
    if (!l) l = CreateDir((STRPTR)dir);
    if (l) UnLock(l);
    if ((fh = Open((STRPTR)path, MODE_NEWFILE))) { Write(fh, (APTR)text, n); Close(fh); }
    loud(w);
}

void keys_load(void)
{
    static char d[sizeof defaults];
    char *t;
    keys_bad[0] = 0;
    nbinds = 0;
    /* the defaults first, the person's file over them: a new key arrives
     * without editing the file, and the file wins */
    memcpy(d, defaults, sizeof defaults);
    parse(d);
    if (!(t = readall(KEYENV, 32768))) t = readall(KEYFILE, 32768);
    if (!t) {
        writeall("ENVARC:CShell", KEYFILE, defaults, sizeof defaults - 1);
        writeall("ENV:CShell", KEYENV, defaults, sizeof defaults - 1);
        return;
    }
    parse(t);
    FreeVec(t);
}

int keys_find(int key)
{
    int i;
    for (i = 0; i < nbinds; i++) if (binds[i].key == key) return binds[i].act;
    return A_NONE;
}
