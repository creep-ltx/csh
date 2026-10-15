/* job.c - pipes and jobs on the Amiga (docs/design.md "Pipes", "Jobs").
 *
 * Each Amiga command of a pipe runs in a process of its own with a CLI
 * (NP_Cli: the Path and the local variables are copied), joined to the next
 * by PIPE: (queue-handler). csh finds and loads the command itself
 * (run_prepare, the same search as always); the child only runs it with
 * RunCommand, exactly as csh would, and says when it is done. A script
 * stage is handed to the ROM Shell in the child, as the ROM Shell runs
 * scripts.
 *
 * The child's streams are closed by DOS when it ends (NP_CloseInput /
 * NP_CloseOutput for a pipe or a file, not for csh's own console): closing
 * them in the child first made DOS free them twice (exec alert 01000009,
 * "freeing memory already freed", seen in FS-UAE on 3.2). So a T: file a
 * child read may still be open a moment after it said it is done: csh
 * tries again to delete it.
 *
 * The last stage of a pipe in the foreground runs in csh itself, as the
 * ROM Shell does (a process fewer per pipe; csh's pipes were 4% slower than
 * the ROM's with one each). While it runs, the console's break signal is
 * pointed (ACTION_CHANGE_SIGNAL, as Execute and SystemTags do for a child)
 * at a small relay task of csh's, which gives each Ctrl-C to every stage:
 * the earlier ones and the one csh runs.
 *
 * Ctrl-C while csh waits goes to every stage still running. A job (&) runs
 * the same way, not waited for; its input is NIL:. Its code is csh's, so
 * csh does not unload itself while a job still runs: it leaves its code in
 * memory (cli_Module 0) when it has to end with jobs running.
 *
 * Traps (Knowledge/amiga/cli-tools-and-archives.md): the reading end of a
 * pipe is opened before the writer starts; WaitForChar never reports data
 * on 3.2's PIPE:; a full RAM: breaks pipes. */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "lang.h"

#ifdef CSHDEBUG
#include <clib/debug_protos.h>
#define DBG(x) KPrintF x
#else
#define DBG(x)
#endif

struct stage {
    void *a4;                   /* cshell: csh's data for this stage's process (first: shellstart.S) */
    struct stage *next;
    struct prepared p;
    struct Process *proc;
    struct Task * volatile parent;   /* NULL once csh has ended (its jobs left running) */
    char name[256];
    char *args;
    LONG arglen;
    BPTR in, out, err;
    int closein, closeout, errtoout;
    volatile int done;
    LONG rc, r2;
    int detached, clinum;
    char argstr[16];
};

struct tmpfile { struct tmpfile *next; char name[48]; };

struct job {
    struct job *next;
    int id, bg, nstages;
    char text[160];
    struct stage *st, **tail;
    BPTR jin, jout;
    int jinown;
    struct tmpfile *tmp;        /* T: files deleted when the job ends */
    int tmpk;
    char tempname[48];
};

#ifdef CSH_USERSHELL
/* cshell is pure (one code, a copy of the data each Shell): code running
 * in csh's other tasks gets a4, the data's address, through these
 * (shellstart.S) */
void stage_stub(void);
void relay_stub(void);
static void *mya4(void) { register void *r __asm__("a4"); return r; }
#define STAGE_ENTRY stage_stub
#define RELAY_ENTRY relay_stub
#else
#define STAGE_ENTRY stage_entry
#define RELAY_ENTRY relay_entry
#endif

static ULONG donesig;
static int donebit = -1;
static struct job *jobs;
static struct job *disowned;    /* disown: running on, reaped quietly when done */
static int lastbg;              /* $! */
/* the rc of jobs already given back (wait -n, "Done"), for a wait %n after:
 * bash answers that too. A number taken by a new job forgets its old rc. */
#define NDONE 8
static struct { int id; LONG rc; } done_[NDONE];
static int ndone_;

static void remember(int id, LONG rc)
{
    done_[ndone_ % NDONE].id = id;
    done_[ndone_ % NDONE].rc = rc;
    ndone_++;
}

static int recalled(int id, LONG *rc)
{
    int i;
    for (i = 0; i < NDONE; i++) if (done_[i].id == id) { *rc = done_[i].rc; done_[i].id = 0; return 1; }
    return 0;
}
static long pipek;

static void hexto(char *d, ULONG v)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) { d[i] = hx[v & 15]; v >>= 4; }
    d[8] = '\n'; d[9] = 0;
}

static ULONG hexfrom(const char *s)
{
    ULONG v = 0;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d;
    }
    return v;
}

/* The child: runs its one command, closes its streams, says it is done. */
void stage_entry(void)
{
    struct stage *st = (struct stage *)hexfrom(GetArgStr());
    struct Process *me = (struct Process *)FindTask(NULL);
    struct CommandLineInterface *cli = Cli();
    LONG rc, r2;
    DBG(("child %lx start in %lx out %lx seg %lx\n", (ULONG)st, st->in, st->out, st->p.seg));
    st->clinum = me->pr_TaskNum;
    if (st->err) me->pr_CES = st->err;
    else if (st->errtoout) me->pr_CES = Output();
    if (st->p.seg) {
        /* the child's own PROGDIR is a lock DOS made for it (a copy of csh's)
         * and frees when it ends: put aside and back, the command's (csh's,
         * freed by run_release) only lent */
        BPTR olddir = 0;
        SetProgramName(st->name);
        if (st->p.progdir) olddir = SetProgramDir(st->p.progdir);
        if (cli) cli->cli_Module = st->p.seg;
        SetIoErr(0);
        rc = RunCommand(st->p.seg, st->p.stack, st->args, st->arglen);
        r2 = IoErr();
        DBG(("child %lx ran rc %ld\n", (ULONG)st, rc));
        if (cli) {
            if (!cli->cli_Module) st->detached = 1;
            cli->cli_Module = 0;
        }
        if (st->p.progdir) SetProgramDir(olddir);
    } else {
        rc = SystemTags(st->p.romline, SYS_Input, Input(), SYS_Output, Output(), TAG_END);
        r2 = IoErr();
    }
    Flush(Output());
    DBG(("child %lx flushed\n", (ULONG)st));
    me->pr_CES = 0;
    if (st->err) Close(st->err);
    DBG(("child %lx closed\n", (ULONG)st));
    st->rc = rc;
    st->r2 = rc ? r2 : 0;
    Forbid();
    st->done = 1;
    if (st->parent) Signal(st->parent, donesig);    /* csh may have ended (job_leave) */
    /* returns with Forbid on: the process ends in the ROM, not in csh's code */
}

static int jobinit(void)
{
    if (donebit >= 0) return 1;
    if ((donebit = AllocSignal(-1)) < 0) return 0;
    donesig = 1UL << donebit;
    return 1;
}

void *os_job_new(const char *text, int bg)
{
    struct job *j;
    if (!jobinit() || !(j = AllocVec(sizeof *j, MEMF_ANY | MEMF_CLEAR))) return NULL;
    os_flush();
    j->bg = bg;
    j->tail = &j->st;
    strncpy(j->text, text, sizeof j->text - 1);
    if (bg) {
        if ((j->jin = Open("NIL:", MODE_OLDFILE))) j->jinown = 1;
    } else j->jin = Input();
    /* a job writes to csh's own console (not into a $(...) file, which is
     * gone when the job may still be writing) */
    j->jout = bg ? csh.out : Output();
    return j;
}

long os_job_in(void *job) { return ((struct job *)job)->jin; }
long os_job_out(void *job) { return ((struct job *)job)->jout; }

static int owned(struct job *j, BPTR h) { return h && h != j->jin && h != j->jout; }

void os_close(void *job, long h)
{
    if (owned(job, h)) Close(h);
}

int os_pipe(void *job, long *readh, long *writeh)
{
    char name[48];
    BPTR r, w;
    (void)job;
    strcpy(name, "PIPE:csh");
    csh_tag(name + strlen(name));
    strcat(name, "_");
    ltoa10(++pipek, name + strlen(name));
    /* the reading end first: a writer that is done before the reader opens
     * can leave the reader waiting for ever */
    if (!(r = Open(name, MODE_OLDFILE))) { PrintFault(IoErr(), name); return 0; }
    if (!(w = Open(name, MODE_NEWFILE))) { PrintFault(IoErr(), name); Close(r); return 0; }
    *readh = r; *writeh = w;
    return 1;
}

static BPTR openfor(const char *name, LONG mode, int append)
{
    BPTR fh;
    if (append && (fh = Open((STRPTR)name, MODE_READWRITE))) { Seek(fh, 0, OFFSET_END); return fh; }
    return Open((STRPTR)name, mode);
}

void *os_prepare(struct cmdline *c)
{
    struct prepared *p = AllocVec(sizeof *p, MEMF_ANY);
    if (!p) return NULL;
    if (!run_prepare(c, p)) { FreeVec(p); return NULL; }
    return p;
}

void os_unprepare(void *p)
{
    run_release(p, 0);
    FreeVec(p);
}

int os_spawn(void *job, struct cmdline *c, long inh, long outh, void *prep)
{
    struct prepared p;
    if (prep) {
        p = *(struct prepared *)prep;
        FreeVec(prep);
        return job_spawn_prepared(job, c, inh, outh, &p);
    }
    if (!run_prepare(c, &p)) {
        /* counted as a stage that ended at once with its rc (10) */
        struct job *j = job;
        struct stage *st = AllocVec(sizeof *st, MEMF_ANY | MEMF_CLEAR);
        os_close(j, inh); os_close(j, outh);
        if (st) { st->done = 1; st->rc = csh.rc ? csh.rc : 10; *j->tail = st; j->tail = &st->next; j->nstages++; }
        return 0;
    }
    return job_spawn_prepared(job, c, inh, outh, &p);
}

/* A stage whose command run_prepare found and loaded: its own process. */
int job_spawn_prepared(void *job, struct cmdline *c, long inh, long outh, struct prepared *pp)
{
    struct job *j = job;
    struct stage *st = AllocVec(sizeof *st, MEMF_ANY | MEMF_CLEAR);
    BPTR in = inh, out = outh;
    if (!st) { run_release(pp, 0); os_close(j, in); os_close(j, out); return 0; }
    st->p = *pp;
    st->parent = (struct Task *)csh.me;
#ifdef CSH_USERSHELL
    st->a4 = mya4();
#endif
    *j->tail = st; j->tail = &st->next;     /* linked even when it fails: its rc counts */
    j->nstages++;
    if (!in) in = Open("NIL:", MODE_OLDFILE);
    if (c->in[0]) {
        BPTR f = Open(c->in, c->inout ? MODE_READWRITE : MODE_OLDFILE);
        if (!f) { PrintFault(IoErr(), c->in); goto fail; }
        os_close(j, in);
        in = f;
    }
    if (c->inout) {
        os_close(j, out);
        out = in;
    } else if (c->out[0]) {
        BPTR f = openfor(c->out, MODE_NEWFILE, c->out_append);
        if (!f) { PrintFault(IoErr(), c->out); goto fail; }
        os_close(j, out);
        out = f;
    }
    if (c->err[0] && !(st->err = openfor(c->err, MODE_NEWFILE, c->err_append))) { PrintFault(IoErr(), c->err); goto fail; }
    if (j->bg && out == j->jout) {
        /* a job writes to the console through a handle of its own, closed
         * by DOS when it ends: csh's may be gone by then (the tab closed) */
        BPTR f = Open("*", MODE_NEWFILE);
        if (f) out = f;
    }
    st->errtoout = c->errtoout;
    strncpy(st->name, c->cmd, sizeof st->name - 1);
    if (!(st->args = AllocVec(c->arglen + 1, MEMF_ANY))) goto fail;
    memcpy(st->args, c->args, c->arglen + 1);
    st->arglen = c->arglen;
    st->in = in; st->out = out;
    st->closein = owned(j, in);
    st->closeout = owned(j, out) && out != in;
    hexto(st->argstr, (ULONG)st);
    st->proc = CreateNewProcTags(NP_Entry, (ULONG)STAGE_ENTRY,
                                 NP_Input, in, NP_Output, out,
                                 NP_CloseInput, st->closein, NP_CloseOutput, st->closeout,
                                 NP_Cli, TRUE, NP_Name, (ULONG)(j->bg ? "csh job" : "csh pipe"),
                                 NP_StackSize, 6000, NP_Arguments, (ULONG)st->argstr,
                                 TAG_END);
    DBG(("spawned %lx proc %lx\n", (ULONG)st, (ULONG)st->proc));
    if (!st->proc) { PrintFault(IoErr(), c->cmd); goto fail; }
    return 1;
fail:
    run_release(&st->p, 0);
    if (st->err) { Close(st->err); st->err = 0; }
    if (owned(j, in)) Close(in);
    if (owned(j, out) && out != in) Close(out);
    st->done = 1;
    st->rc = csh.rc ? csh.rc : 10;
    return 0;
}

struct sel { BPTR oin, oout, in, out; };

void *os_select(long in, long out)
{
    struct sel *s = AllocVec(sizeof *s, MEMF_ANY | MEMF_CLEAR);
    if (!s) return NULL;
    os_flush();
    if (!in) in = s->in = Open("NIL:", MODE_OLDFILE);
    s->oin = SelectInput(in);
    if (out) s->oout = SelectOutput(out);
    s->out = out;
    return s;
}

void os_unselect(void *p)
{
    struct sel *s = p;
    if (!s) return;
    os_flush();
    if (s->out) { Flush(s->out); SelectOutput(s->oout); }
    SelectInput(s->oin);
    if (s->in) Close(s->in);
    FreeVec(s);
}

long os_tempout(void *job)
{
    struct job *j = job;
    BPTR fh;
    strcpy(j->tempname, "T:csh-pipe-");
    csh_tag(j->tempname + strlen(j->tempname));
    strcat(j->tempname, "-");
    ltoa10(++j->tmpk, j->tempname + strlen(j->tempname));
    if (!(fh = Open(j->tempname, MODE_NEWFILE))) { PrintFault(IoErr(), j->tempname); return 0; }
    os_job_tmp(j, j->tempname);
    return fh;
}

long os_tempin(void *job)
{
    return Open(((struct job *)job)->tempname, MODE_OLDFILE);
}

void os_job_tmp(void *job, const char *name)
{
    struct job *j = job;
    struct tmpfile *t = AllocVec(sizeof *t, MEMF_ANY | MEMF_CLEAR);
    if (!t) { DeleteFile((STRPTR)name); return; }   /* better gone early than left */
    strncpy(t->name, name, sizeof t->name - 1);
    t->next = j->tmp;
    j->tmp = t;
}

static int alldone(struct job *j)
{
    struct stage *s;
    for (s = j->st; s; s = s->next) if (!s->done) return 0;
    return 1;
}

static void breakall(struct job *j)
{
    struct stage *s;
    Forbid();
    for (s = j->st; s; s = s->next) if (!s->done && s->proc) Signal((struct Task *)s->proc, SIGBREAKF_CTRL_C);
    Permit();
}

/* Waits for every stage; Ctrl-C goes on to each one still running. A
 * second Ctrl-C (a stage that ignores breaks) stops waiting: returns 1,
 * the job goes on in the background. */
static int waitjob(struct job *j)
{
    int breaks = 0;
    while (!alldone(j)) {
        ULONG sig = Wait(donesig | SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) {
            if (++breaks >= 2) return 1;
            breakall(j);
        }
    }
    return 0;
}

/* Everything given back; the last stage's rc (and its Result2 in *r2).
 * $RC is the caller's to set: a job ending in the background leaves it. */
static LONG reap(struct job *j, LONG *r2p)
{
    struct stage *s, *n;
    struct tmpfile *t, *tn;
    LONG rc = 0, r2 = 0;
    for (s = j->st; s; s = n) {
        n = s->next;
        rc = s->rc; r2 = s->r2;
        DBG(("reap %lx\n", (ULONG)s));
        run_release(&s->p, s->detached);
        if (s->args) FreeVec(s->args);
        FreeVec(s);
    }
    j->st = NULL;
    for (t = j->tmp; t; t = tn) {     /* a child may hold it a moment longer */
        int k;
        tn = t->next;
        for (k = 0; k < 25 && !DeleteFile(t->name) && IoErr() == ERROR_OBJECT_IN_USE; k++) Delay(2);
        FreeVec(t);
    }
    j->tmp = NULL;
    if (j->jinown) Close(j->jin);
    j->jinown = 0;
    if (r2p) *r2p = r2;
    return rc;
}

static void addjob(struct job *j);

long os_job_end(void *job, long lastrc, int lastinternal, long *rcs, int *nrcs)
{
    struct job *j = job;
    LONG rc;
    *nrcs = 0;
    if (!j->bg) {
        LONG r2, lastr2 = csh.cli->cli_Result2;     /* the last stage's, when it ran in csh */
        DBG(("wait job\n"));
        if (lastinternal) {
            /* the last stage is done: nobody reads what the others write any
             * more. One still running after a second gets a Ctrl-C - Unix's
             * SIGPIPE; else a writer waits for ever on its full pipe (List |
             * Sort | NoSuchCommand: Sort gives up, List waits) */
            int k;
            for (k = 0; k < 50 && !alldone(j); k++) Delay(1);
            if (!alldone(j)) breakall(j);
        }
        if (waitjob(j)) {
            /* Ctrl-C twice: a stage ignores breaks; it goes on as a job */
            os_flush();
            addjob(j);
            FPuts(csh.out, "csh: still running, in the background now (jobs, fg, kill)\n");
            set_rc(20, 0);
            return 20;
        }
        DBG(("waited\n"));
        {   /* each stage's rc, for PIPESTATUS and pipefail */
            struct stage *s;
            for (s = j->st; s && *nrcs < 32; s = s->next) rcs[(*nrcs)++] = s->rc;
        }
        rc = reap(j, &r2);
        if (lastinternal || !j->nstages) { rc = lastrc; r2 = lastr2; }
        FreeVec(j);
        set_rc(rc, r2);
        return rc;
    }
    if (!j->st) { reap(j, NULL); FreeVec(j); return lastrc; }
    addjob(j);
    lastbg = j->id;
    {
        char b[16];
        struct stage *s;
        int i, waited = 0;
        /* the CLI numbers, as Run shows them ([CLI n]) */
        for (s = j->st; s; s = s->next) while (!s->clinum && !s->done && waited++ < 50) Delay(1);
        os_write("[", 1);
        os_write(b, ltoa10(j->id, b));
        os_write("]", 1);
        for (s = j->st, i = 0; s; s = s->next) if (s->clinum) { os_write(" ", 1); os_write(b, ltoa10(s->clinum, b)); i++; }
        os_write("\n", 1);
        os_flush();
    }
    return 0;
}


/* In the job list, with the lowest free number. */
static void addjob(struct job *j)
{
    struct job **p, *k;
    int id = 1;
    for (;;) {
        for (k = jobs; k && k->id != id; k = k->next) ;
        if (!k) break;
        id++;
    }
    j->id = id;
    j->bg = 1;
    {   /* the number's old rc is not this job's */
        int i;
        for (i = 0; i < NDONE; i++) if (done_[i].id == id) done_[i].id = 0;
    }
    j->next = NULL;
    for (p = &jobs; *p; p = &(*p)->next) ;
    *p = j;
}

/* ---- the break relay ---- */

static struct Task *relay, *relayfor;
static struct MsgPort *relayport;
static struct job *relayjob;
static volatile int relayquit, relaygone;
static void *relaymem;
static UBYTE relaybit;          /* the signal CreateMsgPort took: DeleteMsgPort frees it */

void relay_entry(void)
{
    for (;;) {
        ULONG s = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
        if (relayquit) break;
        Forbid();
        if ((s & SIGBREAKF_CTRL_C) && relayjob) breakall(relayjob);
        Signal(relayfor, s & (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F));
        Permit();
    }
    Forbid();
    relaygone = 1;
    /* ends in exec (RemTask), not in csh's code */
}

static int relaystart(void)
{
    struct Task *t;
    char *stk;
    if (relay) return 1;
    if (!(relaymem = AllocVec(sizeof(struct Task) + 2048 + 16, MEMF_PUBLIC | MEMF_CLEAR))) return 0;
    if (!(relayport = CreateMsgPort())) { FreeVec(relaymem); relaymem = NULL; return 0; }
    t = relaymem;
    stk = (char *)(t + 1);
    /* an empty list: RemTask walks it (the AddTask autodoc's NewList) */
    t->tc_MemEntry.lh_Head = (struct Node *)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_Tail = NULL;
    t->tc_MemEntry.lh_TailPred = (struct Node *)&t->tc_MemEntry.lh_Head;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Name = "csh break relay";
    t->tc_Node.ln_Pri = csh.me->pr_Task.tc_Node.ln_Pri + 1;
    t->tc_SPLower = stk;
    t->tc_SPUpper = stk + 2048;
    t->tc_SPReg = stk + 2048;
    relayfor = (struct Task *)csh.me;
#ifdef CSH_USERSHELL
    t->tc_UserData = mya4();
#endif
    relayquit = relaygone = 0;
    /* the console signals the port's task: the relay */
    relaybit = relayport->mp_SigBit;
    relayport->mp_SigTask = t;
    relayport->mp_SigBit = SIGBREAKB_CTRL_C;
    if (!AddTask(t, (APTR)RELAY_ENTRY, NULL)) {
        relayport->mp_SigTask = (struct Task *)csh.me;
        relayport->mp_SigBit = relaybit;
        DeleteMsgPort(relayport); relayport = NULL; FreeVec(relaymem); relaymem = NULL;
        return 0;
    }
    relay = t;
    return 1;
}

static void relaystop(void)
{
    if (!relay) return;
    relayquit = 1;
    Signal(relay, SIGBREAKF_CTRL_F);
    while (!relaygone) Delay(1);
    relay = NULL;
    relayport->mp_SigTask = (struct Task *)csh.me;
    relayport->mp_SigBit = relaybit;           /* its own bit freed, not Ctrl-C's */
    DeleteMsgPort(relayport);
    relayport = NULL;
    FreeVec(relaymem);
    relaymem = NULL;
}

/* The console's breaks to port (returns the one before), or 0 if it is no
 * console. The answer is DOSTRUE with the old port in res2 (the Guru Book,
 * 21.2.4.3; CTerm does so): 0.1b3 took res1, DOSTRUE, for the port and
 * "gave back" port -1. A handler answering the port in res1 is read too. */
static LONG changesignal(BPTR con, struct MsgPort *port)
{
    struct FileHandle *fh = BADDR(con);
    LONG r;
    if (!con || !fh->fh_Type || !IsInteractive(con)) return 0;
    r = DoPkt(fh->fh_Type, ACTION_CHANGE_SIGNAL, fh->fh_Arg1, (LONG)port, 0, 0, 0);
    if (r == DOSTRUE) r = IoErr();
    return r;
}

static long lastrun(void *job, struct cmdline *c, long in, struct prepared *p);

long os_lastrun(void *job, struct cmdline *c, long in, void *prep)
{
    struct prepared p;
    if (!prep) return lastrun(job, c, in, NULL);
    p = *(struct prepared *)prep;
    FreeVec(prep);
    return lastrun(job, c, in, &p);
}

long job_lastrun_prepared(void *job, struct cmdline *c, long in, struct prepared *p) { return lastrun(job, c, in, p); }

/* The last stage in csh itself, its input the pipe; Ctrl-C to every stage. */
static long lastrun(void *job, struct cmdline *c, long in, struct prepared *p)
{
    struct job *j = job;
    LONG rc, old = 0;
    BPTR con = csh.cli->cli_StandardInput;
    int moved = 0;
    if (!in) in = Open("NIL:", MODE_OLDFILE);
    if (j->st && relaystart()) {
        relayjob = j;
        old = changesignal(con, relayport);
        moved = 1;
    }
    c->inh = in;
    /* _CMDLINE: the pipe's whole line (its stages leave it); a line cut
     * to fit text[] is not one to run again */
    run_cmdline_begin(strlen(j->text) < sizeof j->text - 1 ? j->text : NULL, NULL);
    rc = p ? run_prepared(c, p) : run_cmd(c, 0);
    run_cmdline_end();
    c->inh = 0;
    /* back where it was; none said: csh's own (never left at the relay) */
    if (moved) changesignal(con, old ? (struct MsgPort *)old : &csh.me->pr_MsgPort);
    relayjob = NULL;
    if (owned(j, in)) Close(in);
    return rc;
}

/* ---- jobs, fg, kill, wait; and the news at the prompt ---- */

static struct job *jobfind(const char *spec)
{
    struct job *j;
    long id;
    if (!spec) {               /* the newest */
        struct job *last = NULL;
        for (j = jobs; j; j = j->next) last = j;
        return last;
    }
    if (*spec == '%') spec++;
    if (StrToLong((STRPTR)spec, &id) <= 0) return NULL;
    for (j = jobs; j; j = j->next) if (j->id == id) return j;
    return NULL;
}

static void unlink_(struct job *j)
{
    struct job **p;
    for (p = &jobs; *p && *p != j; p = &(*p)->next) ;
    if (*p) *p = j->next;
}

static void say(struct job *j, const char *what, LONG rc)
{
    char b[16];
    os_write("[", 1);
    os_write(b, ltoa10(j->id, b));
    os_write("] ", 2);
    os_write(what, strlen(what));
    if (rc) { os_write(" (rc ", 5); os_write(b, ltoa10(rc, b)); os_write(")", 1); }
    os_write("  ", 2);
    os_write(j->text, strlen(j->text));
    os_write("\n", 1);
}

/* At the prompt: the jobs that ended since, said and given back. */
int job_count(void)
{
    struct job *j;
    int n = 0;
    for (j = jobs; j; j = j->next) if (!alldone(j)) n++;
    return n;
}

int job_lastbg(void) { return lastbg; }

/* the disowned that are done: given back, nothing said */
static void reapdisowned(void)
{
    struct job *j, *n, **p = &disowned;
    for (j = disowned; j; j = n) {
        n = j->next;
        if (alldone(j)) { *p = n; reap(j, NULL); FreeVec(j); }
        else p = &j->next;
    }
}

void job_news(void)
{
    struct job *j, *n;
    reapdisowned();
    for (j = jobs; j; j = n) {
        n = j->next;
        if (alldone(j)) {
            LONG rc;
            unlink_(j);
            rc = reap(j, NULL);
            remember(j->id, rc);
            say(j, "Done", rc);
            FreeVec(j);
        }
    }
    os_flush();
}

int job_running(void)
{
    struct job *j;
    for (j = jobs; j; j = j->next) if (!alldone(j)) return 1;
    for (j = disowned; j; j = j->next) if (!alldone(j)) return 1;
    return 0;
}

long job_list(void)
{
    struct job *j, *n;
    for (j = jobs; j; j = n) {
        n = j->next;
        if (alldone(j)) {          /* said once, as bash: not again at the prompt */
            LONG rc;
            unlink_(j);
            rc = reap(j, NULL);
            remember(j->id, rc);
            say(j, "Done", rc);
            FreeVec(j);
        } else say(j, "Running", 0);
    }
    return 0;
}

long job_fg(const char *spec)
{
    struct job *j = jobfind(spec);
    LONG rc;
    if (!j) { sh_error("fg: no such job", spec ? spec : ""); return 10; }
    os_write(j->text, strlen(j->text));
    os_write("\n", 1);
    os_flush();
    if (waitjob(j)) { sh_error("fg: still running (Ctrl-C ignored):", j->text); return 20; }
    unlink_(j);
    rc = reap(j, NULL);
    FreeVec(j);
    return rc;
}

long job_kill(const char *spec)
{
    struct job *j = jobfind(spec);
    if (!j) { sh_error("kill: no such job", spec ? spec : ""); return 10; }
    /* a Ctrl-C to each stage: a program that ignores it cannot be ended by force on the Amiga */
    breakall(j);
    return 0;
}

/* wait %n: as fg, without the job's line */
long job_wait(const char *spec)
{
    struct job *j = jobfind(spec);
    LONG old;
    long id;
    if (!j && StrToLong((STRPTR)(spec[0] == '%' ? spec + 1 : spec), &id) > 0 && recalled(id, &old)) return old;
    if (!j) { sh_error("wait: no such job", spec); return 127; }
    if (waitjob(j)) { sh_error("wait: still running (Ctrl-C ignored):", j->text); return 20; }
    unlink_(j);
    {
        LONG rc = reap(j, NULL);
        FreeVec(j);
        return rc;
    }
}

/* wait -n: whichever job ends first; its rc. Ctrl-C stops waiting. */
long job_waitany(void)
{
    struct job *j;
    if (!jobs) return 127;
    jobinit();
    for (;;) {
        for (j = jobs; j; j = j->next)
            if (alldone(j)) {
                LONG rc;
                unlink_(j);
                rc = reap(j, NULL);
                remember(j->id, rc);
                FreeVec(j);
                return rc;
            }
        if (Wait(donesig | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) return 20;
    }
}

/* disown: the job runs on, out of the list (jobs, fg, wait, "Done" leave
 * it); its memory is given back once it ends. -a: all of them. */
long job_disown(const char *spec)
{
    struct job *j;
    if (spec && !strcmp(spec, "-a")) {
        while ((j = jobs)) { unlink_(j); j->next = disowned; disowned = j; }
        return 0;
    }
    if (!(j = jobfind(spec))) { sh_error("disown: no such job", spec ? spec : ""); return 1; }
    unlink_(j);
    j->next = disowned;
    disowned = j;
    return 0;
}

long job_waitall(void)
{
    struct job *j;
    LONG rc = 0;
    while ((j = jobs)) {
        if (waitjob(j)) { sh_error("wait: still running (Ctrl-C ignored):", j->text); return 20; }
        unlink_(j);
        rc = reap(j, NULL);
        FreeVec(j);
    }
    return rc;
}

/* csh ends with jobs still running: their code is csh's, so csh's code
 * stays in memory (the ROM Shell does not unload a cli_Module of 0). */
void job_leave(void)
{
    struct job *j, *n;
    if (job_running()) {
        struct stage *s;
        csh.module = 0;
        /* the children must not Signal csh when they end: it is gone */
        Forbid();
        for (j = jobs; j; j = j->next)
            for (s = j->st; s; s = s->next) s->parent = NULL;
        for (j = disowned; j; j = j->next)
            for (s = j->st; s; s = s->next) s->parent = NULL;
        Permit();
        relaystop();
        return;
    }
    for (j = jobs; j; j = n) { n = j->next; reap(j, NULL); FreeVec(j); }
    jobs = NULL;
    for (j = disowned; j; j = n) { n = j->next; reap(j, NULL); FreeVec(j); }
    disowned = NULL;
    if (donebit >= 0) FreeSignal(donebit);
    donebit = -1;
    relaystop();
}
