/* hist.c - the history: the last prefs_history lines, shared between csh
 * tabs and kept across boots.
 *
 * ENV:CShell/history (RAM) gets each line as it is typed, so every csh sees
 * the others' lines at its next prompt (hist_refresh: the file's size
 * changed -> read again); ENVARC:CShell/history (the disk) is written every
 * 20 lines and when csh ends, so typing never waits on a disk. */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

#define HISTENV    "ENV:CShell/history"
#define HISTENVARC "ENVARC:CShell/history"

extern char *readall(const char *path, long max);
extern void writeall(const char *dir, const char *path, const char *text, long n);

static char **lines;
static int count, cap, sincesave;
static LONG envsize = -1;

static void clear(void)
{
    int i;
    for (i = 0; i < count; i++) FreeVec(lines[i]);
    count = 0;
}

static void push(const char *s, int n)
{
    char *p;
    if (!lines) {
        cap = prefs_history;
        if (!(lines = AllocVec(cap * sizeof *lines, MEMF_ANY | MEMF_CLEAR))) return;
    }
    if (count && (int)strlen(lines[count - 1]) == n && !memcmp(lines[count - 1], s, n)) return;
    if (!(p = AllocVec(n + 1, MEMF_ANY))) return;
    memcpy(p, s, n); p[n] = 0;
    if (count == cap) {
        FreeVec(lines[0]);
        memmove(lines, lines + 1, (cap - 1) * sizeof *lines);
        count--;
    }
    lines[count++] = p;
}

static LONG filesize(const char *path)
{
    struct FileInfoBlock *fib;
    LONG n = -1;
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    if (!l) return -1;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib)) n = fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return n;
}

/* Reads the file: only its last prefs_history lines are kept. An empty
 * file (another tab is writing it anew this moment) changes nothing: 0. */
static int readfile(const char *path)
{
    char *t = readall(path, 1024L * 1024), *p, *e, *start;
    int nl = 0;
    if (!t) return 0;
    if (!*t && count) { FreeVec(t); return 0; }
    clear();
    /* skip to where the last cap lines begin: no copying of lines dropped */
    for (p = t + strlen(t); p > t; p--) if (p[-1] == '\n' && p[0] && ++nl > prefs_history) break;
    start = p;
    for (p = start; *p; p = e) {
        e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (e > p) push(p, e - p);
        if (*e) e++;
    }
    FreeVec(t);
    return 1;
}

static void writeout(const char *dir, const char *path)
{
    long n = 0, i;
    char *t, *o;
    for (i = 0; i < count; i++) n += strlen(lines[i]) + 1;
    if (!(t = AllocVec(n + 1, MEMF_ANY))) return;
    for (o = t, i = 0; i < count; i++) {
        int l = strlen(lines[i]);
        memcpy(o, lines[i], l); o += l; *o++ = '\n';
    }
    writeall(dir, path, t, n);
    FreeVec(t);
}

static int loaded;

void hist_load_once(void) { if (!loaded) hist_load(); }

void hist_load(void)
{
    loaded = 1;
    if (filesize(HISTENV) < 0) {
        readfile(HISTENVARC);
        writeout("ENV:CShell", HISTENV);
    } else readfile(HISTENV);
    envsize = filesize(HISTENV);
}

void hist_refresh(void)
{
    LONG n = filesize(HISTENV);
    if (n >= 0 && n != envsize && readfile(HISTENV)) envsize = n;
}

/* ENV:CShell/history held by one tab at a time (an exclusive lock, others
 * wait a moment): two tabs appending at once would write over each other. */
static BPTR histopen(void)
{
    BPTR l = 0, fh;
    int k;
    for (k = 0; k < 25 && !(l = Lock(HISTENV, EXCLUSIVE_LOCK)); k++) {
        if (IoErr() != ERROR_OBJECT_IN_USE) break;
        Delay(1);
    }
    if (!l) return 0;
    if (!(fh = OpenFromLock(l))) UnLock(l);
    return fh;
}

void hist_add(const char *line)
{
    int n = strlen(line);
    BPTR fh;
    char *t;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (!n || line[0] == ' ') return;        /* a line starting with a space stays private */
    if (count && (int)strlen(lines[count - 1]) == n && !memcmp(lines[count - 1], line, n)) return;
    push(line, n);
    if ((fh = histopen())) {
        LONG end;
        Seek(fh, 0, OFFSET_END);
        end = Seek(fh, 0, OFFSET_CURRENT);
        /* the line and its LF in one Write */
        if ((t = AllocVec(n + 1, MEMF_ANY))) {
            memcpy(t, line, n); t[n] = '\n';
            Write(fh, t, n + 1);
            FreeVec(t);
        }
        /* another tab wrote since this one last read: read it all again at
         * the next prompt (its lines too), else this size is known */
        envsize = end == envsize ? Seek(fh, 0, OFFSET_CURRENT) : -1;
        Close(fh);
        /* the file grows: past the kept lines it is written short again,
         * from what is in it now (every tab's lines), not from memory */
        if (envsize > (LONG)prefs_history * 160 || envsize < 0) {
            LONG sz = filesize(HISTENV);
            if (sz > (LONG)prefs_history * 160 && readfile(HISTENV)) {
                writeout("ENV:CShell", HISTENV);
                envsize = filesize(HISTENV);
            }
        }
    } else if (filesize(HISTENV) < 0) {
        writeout("ENV:CShell", HISTENV);     /* ENV: emptied meanwhile: made again */
        envsize = filesize(HISTENV);
    }
    if (++sincesave >= 20) hist_save();
}

void hist_save(void)
{
    if (!count) return;
    writeout("ENVARC:CShell", HISTENVARC);
    sincesave = 0;
}

void hist_end(void)
{
    clear();
    if (lines) FreeVec(lines);
    lines = NULL;
    cap = 0;
}

int hist_count(void) { return count; }

const char *hist_get(int i) { return i >= 0 && i < count ? lines[i] : ""; }

void hist_print(BPTR out)
{
    int i;
    char b[12];
    for (i = 0; i < count; i++) {
        int n = ltoa10(i + 1, b);
        while (n < 5) { FPutC(out, ' '); n++; }
        FPuts(out, b); FPuts(out, "  "); FPuts(out, lines[i]); FPutC(out, '\n');
    }
}
