/* term.c - the console under csh's prompt, while a line is edited: raw
 * mode, the keys decoded, the window's width, the output gathered and sent
 * in one Write. Cut down from Sumi's tui.c (the input side only: csh
 * draws on the line it is on, never a screen of its own).
 *
 * Keys come without polling: an ACTION_WAIT_CHAR packet goes to the
 * console with a reply port of csh's own, so its signal sits in one Wait()
 * beside Ctrl-C..F. An idle prompt costs ten small packets a second.
 *
 * Ctrl-C..F come as a signal at once AND (from the 3.2 console) as a byte
 * behind what was typed before them; the signal must not overtake those
 * keys, so on a signal the wait packet is collected and the console read
 * first (the byte then comes in its place and the signal is dropped).
 *
 * Both key conventions are understood: the Amiga console's (0x9B CSI,
 * CSI T / S / SP A / SP @ for the shifted arrows, CSI n~ the function
 * keys) and xterm's (ESC [ A, ESC [ 1;5D, ESC [ 3~), which CTerm sends for
 * Ctrl/Alt/Shift+arrows once bracketed paste is asked for. Characters are
 * Latin-1, as everywhere in the Amiga Shell. */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

#define ID_CTRM  0x4354524DUL
#define WAITUS   100000         /* a key wait out at most 0.1 s: a signal waits for it */
#define BREAKS   (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F)

int term_cols = 80, term_cterm = -1;

static BPTR tin, tout;
static struct MsgPort *conport, *rport;
static struct DosPacket *wpkt;
static int wpending, isopen;

/* ---- output ---------------------------------------------------------- */

static char obuf[2048];
static int olen;

void term_flush(void)
{
    if (olen) Write(tout, obuf, olen);
    olen = 0;
}

void term_out(const char *s, int n)
{
    while (n > 0) {
        int k = (int)sizeof obuf - olen;
        if (k > n) k = n;
        memcpy(obuf + olen, s, k);
        olen += k; s += k; n -= k;
        if (olen == (int)sizeof obuf) term_flush();
    }
}

void term_outs(const char *s) { term_out(s, strlen(s)); }

void term_num(int n)
{
    char b[12];
    term_out(b, ltoa10(n, b));
}

/* ESC [ n f - a cursor move by n (nothing when n is 0) */
void term_move(int n, char f)
{
    char b[16];
    if (n <= 0) return;
    b[0] = 0x1b; b[1] = '[';
    ltoa10(n, b + 2);
    n = strlen(b);
    b[n++] = f;
    term_out(b, n);
}

/* ---- input ----------------------------------------------------------- */

#define KQ 32
static int kq[KQ], kh, kt;
static UBYTE ibuf[256];
static int ilen;
static char *paste;
static int pastelen, pastecap, inpaste;

static void push(int key)
{
    int n = (kt + 1) % KQ;
    if (n == kh) return;
    kq[kt] = key;
    kt = n;
}

static void pasteadd(const UBYTE *b, int n)
{
    if (pastelen + n + 1 > pastecap) {
        int cap = (pastelen + n) * 2 + 256;
        char *p = AllocVec(cap, MEMF_ANY);
        if (!p) return;
        if (paste) { memcpy(p, paste, pastelen); FreeVec(paste); }
        paste = p; pastecap = cap;
    }
    memcpy(paste + pastelen, b, n);
    pastelen += n;
    paste[pastelen] = 0;
}

const char *term_paste(int *len)
{
    *len = pastelen;
    return paste ? paste : "";
}

static int mods(int m)
{
    int r = 0;
    if (m < 2) return 0;
    m--;
    if (m & 1) r |= TM_SHIFT;
    if (m & 2) r |= TM_ALT;
    if (m & 4) r |= TM_CTRL;
    return r;
}

static void csi(const int *p, int np, int priv, int inter, int f, int fromamiga)
{
    int p0 = np > 0 ? p[0] : 0, m = np > 1 ? mods(p[1]) : 0;
    if (inter == ' ') {
        if (f == 'r' && np >= 4) {             /* window bounds: rows p[2], columns p[3] */
            if (p[3] >= 10) {
                if (term_cols != p[3] && isopen) push(K_RESIZE);
                term_cols = p[3];
            }
        } else if (f == 'A') push(K_LEFT | TM_SHIFT);
        else if (f == '@') push(K_RIGHT | TM_SHIFT);
        return;
    }
    if (priv) { if (priv == '?' && f == '~') push(K_HELP); return; }
    switch (f) {
    case 'A': push(K_UP | m); break;
    case 'B': push(K_DOWN | m); break;
    case 'C': push(K_RIGHT | m); break;
    case 'D': push(K_LEFT | m); break;
    case 'H': push(K_HOME | m); break;
    case 'F': push(K_END | m); break;
    case 'T': push(K_UP | TM_SHIFT); break;
    case 'S': push(K_DOWN | TM_SHIFT); break;
    case 'Z': push(K_TAB | TM_SHIFT); break;
    case '~':
        if (fromamiga) {
            if (np && p0 <= 9) push(K_F1 + p0);
            else if (p0 >= 10 && p0 <= 19) push((K_F1 + p0 - 10) | TM_SHIFT);
            break;
        }
        switch (p0) {
        case 1: case 7: push(K_HOME | m); break;
        case 2: push(K_INS | m); break;
        case 3: push(K_DEL | m); break;
        case 4: case 8: push(K_END | m); break;
        case 5: push(K_PGUP | m); break;
        case 6: push(K_PGDN | m); break;
        case 11: case 12: case 13: case 14: case 15: push((K_F1 + p0 - 11) | m); break;
        case 17: case 18: case 19: case 20: case 21: push((K_F1 + 5 + p0 - 17) | m); break;
        case 23: case 24: push((K_F1 + 10 + p0 - 23) | m); break;
        case 200: inpaste = 1; pastelen = 0; break;
        }
        break;
    case '|':                                  /* a raw input event: 12 = resized */
        if (p0 == 12) { term_outs("\x9b" "0 q"); term_flush(); }
        break;
    }
}

static void ctrlkey(int i, int fromsignal);

static void parse(int final)
{
    int i = 0;
    while (i < ilen) {
        UBYTE c = ibuf[i];
        int cs = -1, fromamiga = 0;
        if (inpaste) {
            int j;
            for (j = i; j < ilen && ibuf[j] != 0x1b; j++) ;
            if (j > i) pasteadd(ibuf + i, j - i);
            i = j;
            if (i >= ilen) break;
            if (ilen - i < 6) {
                if (!final) break;
                pasteadd(ibuf + i, ilen - i); i = ilen; break;
            }
            if (!memcmp(ibuf + i, "\x1b[201~", 6)) { inpaste = 0; push(K_PASTE); i += 6; }
            else { pasteadd(ibuf + i, 1); i++; }
            continue;
        }
        if (c == 0x9b) { cs = i + 1; fromamiga = 1; }
        else if (c == 0x1b) {
            if (i + 1 >= ilen) {
                if (!final) break;
                push(K_ESC); i++; continue;
            }
            if (ibuf[i + 1] == '[') cs = i + 2;
            else if (ibuf[i + 1] == 'O') {
                int k = 0;
                if (i + 2 >= ilen) { if (!final) break; push(K_ESC); i += 2; continue; }
                switch (ibuf[i + 2]) {
                case 'A': k = K_UP; break;
                case 'B': k = K_DOWN; break;
                case 'C': k = K_RIGHT; break;
                case 'D': k = K_LEFT; break;
                case 'H': k = K_HOME; break;
                case 'F': k = K_END; break;
                case 'P': k = K_F1; break;
                case 'Q': k = K_F1 + 1; break;
                case 'R': k = K_F1 + 2; break;
                case 'S': k = K_F1 + 3; break;
                }
                if (k) push(k);
                i += 3; continue;
            } else {
                UBYTE n = ibuf[i + 1];          /* ESC and a key: Alt+key (meta) */
                if (n >= 32 && n < 127) push(TM_ALT | n);
                else if (n == 0x7f || n == 8) push(TM_ALT | K_BS);
                else { push(K_ESC); i--; }
                i += 2; continue;
            }
        }
        if (cs >= 0) {
            int p[8], np = 0, priv = 0, inter = 0, j = cs, have = 0, done = 0;
            memset(p, 0, sizeof p);
            if (j < ilen && (ibuf[j] == '?' || ibuf[j] == '<' || ibuf[j] == '>' || ibuf[j] == '=')) priv = ibuf[j++];
            for (; j < ilen; j++) {
                UBYTE b = ibuf[j];
                if (b >= '0' && b <= '9') { if (np < 8) p[np] = p[np] * 10 + (b - '0'); have = 1; }
                else if (b == ';') { if (np < 8) np++; have = 1; }
                else if (b >= 0x20 && b <= 0x2f) inter = b;
                else if (b >= 0x40 && b <= 0x7e) {
                    if (have && np < 8) np++;
                    csi(p, np, priv, inter, b, fromamiga);
                    done = 1; break;
                } else { done = 1; break; }
            }
            if (!done) { if (!final) break; j = ilen - 1; }
            i = j + 1; continue;
        }
        switch (c) {
        case 8: push(K_BS); break;
        case 9: push(K_TAB); break;
        case 10: case 13: push(K_ENTER); break;
        case 0x7f: push(K_DEL); break;
        default:
            if (c >= 3 && c <= 6) ctrlkey(c - 3, 0);        /* Ctrl+C..F: see breaks() */
            else if (c <= 26) push(TM_CTRL | (c + 0x60));   /* Ctrl+A..Z */
            else if (c < 32) push(TM_CTRL | (c + 0x40));    /* Ctrl+\ ] ^ _ */
            else push(c);
        }
        i++;
    }
    if (i > 0) { memmove(ibuf, ibuf + i, ilen - i); ilen -= i; }
}

static void readnow(void)
{
    int guard = 16;
    while (guard-- && WaitForChar(tin, 0)) {
        LONG n = Read(tin, ibuf + ilen, sizeof ibuf - ilen);
        if (n <= 0) break;
        ilen += n;
        parse(0);
        if (ilen >= (int)sizeof ibuf - 8) { parse(1); if (ilen >= (int)sizeof ibuf - 8) ilen = 0; }
    }
    if (ilen) {         /* a lone Esc, or a sequence cut between reads: a moment for the rest */
        if (WaitForChar(tin, 40000)) {
            LONG n = Read(tin, ibuf + ilen, sizeof ibuf - ilen);
            if (n > 0) ilen += n;
            parse(0);
        }
        if (ilen && !inpaste) parse(1);
    }
}

static void arm(void)
{
    if (wpending) return;
    wpkt->dp_Type = ACTION_WAIT_CHAR;
    wpkt->dp_Arg1 = WAITUS;
    SendPkt(wpkt, conport, rport);
    wpending = 1;
}

static int collect(int block)
{
    int back = 0;
    while (wpending) {
        if (!GetMsg(rport)) {
            if (!block) return back;
            WaitPort(rport);
            continue;
        }
        wpending = 0;
        back = 1;
        if (wpkt->dp_Res1) readnow();
    }
    return back;
}

/* Ctrl-C..F: the 3.2 console sends a raw reader the byte (3..6) AND the
 * signal. Each press counts once: whichever comes second is dropped. */
static char sigfirst[4], bytefirst[4];

static void ctrlkey(int i, int fromsignal)
{
    char *mine = fromsignal ? sigfirst : bytefirst, *other = fromsignal ? bytefirst : sigfirst;
    if (other[i]) { other[i] = 0; return; }
    mine[i] = 1;
    push(TM_CTRL | ('c' + i));
}

static void breaks(ULONG sig)
{
    if (sig & SIGBREAKF_CTRL_C) ctrlkey(0, 1);
    if (sig & SIGBREAKF_CTRL_D) ctrlkey(1, 1);
    if (sig & SIGBREAKF_CTRL_E) ctrlkey(2, 1);
    if (sig & SIGBREAKF_CTRL_F) ctrlkey(3, 1);
}

int term_key(void)
{
    for (;;) {
        ULONG sig = SetSignal(0, BREAKS) & BREAKS;
        if (sig) { collect(1); readnow(); breaks(sig); }
        if (kh != kt) { int k = kq[kh]; kh = (kh + 1) % KQ; return k; }
        collect(0);
        if (kh != kt) continue;
        if (ilen) { readnow(); if (kh != kt) continue; }
        term_flush();
        arm();
        sig = Wait((1UL << rport->mp_SigBit) | BREAKS);
        if (sig & BREAKS) {
            collect(1);                 /* what was typed before the Ctrl key, first */
            readnow();
        }
        breaks(sig & BREAKS);
        collect(0);
    }
}

/* Keys typed while csh is busy stay queued in the console: is one there? */
int term_pending(void)
{
    return kh != kt || ilen;
}

static int iscterm(struct MsgPort *port)
{
    struct InfoData *id;
    int yes = 0;
    if ((id = AllocVec(sizeof *id, MEMF_PUBLIC | MEMF_CLEAR))) {
        if (DoPkt(port, ACTION_DISK_INFO, MKBADDR(id), 0, 0, 0, 0)) yes = id->id_DiskType == ID_CTRM;
        FreeVec(id);
    }
    return yes;
}

/* Can csh edit the line itself: an interactive console both ways? */
int term_possible(BPTR in, BPTR out)
{
    struct FileHandle *fh;
    if (!in || !out || !IsInteractive(in) || !IsInteractive(out)) return 0;
    fh = BADDR(in);
    return fh->fh_Type != NULL;
}

/* Raw mode for one line; the width asked each time (the window may have
 * changed while a command ran). */
int term_open(BPTR in, BPTR out)
{
    int tries;
    if (isopen) return 1;
    tin = in; tout = out;
    conport = ((struct FileHandle *)BADDR(tin))->fh_Type;
    if (!rport) {
        if (!(rport = CreateMsgPort())) return 0;
        if (!(wpkt = AllocDosObject(DOS_STDPKT, NULL))) { DeleteMsgPort(rport); rport = NULL; return 0; }
    }
    if (term_cterm < 0) term_cterm = iscterm(conport);
    SetMode(tin, 1);
    isopen = 1;
    term_outs("\x9b" "12{");                   /* resizes reported */
    if (term_cterm) term_outs("\x1b[?2004h");  /* marked pastes, and xterm's modified arrows */
    term_outs("\x9b" "0 q");                   /* the window bounds, answered with CSI ... r */
    term_flush();
    for (tries = 0; tries < 10; tries++) {
        int got = 0, i;
        if (!WaitForChar(tin, 100000)) continue;
        {
            LONG n = Read(tin, ibuf + ilen, sizeof ibuf - ilen);
            if (n > 0) ilen += n;
        }
        for (i = 0; i + 1 < ilen; i++) if (ibuf[i] == ' ' && ibuf[i + 1] == 'r') got = 1;
        parse(0);
        if (got) break;
    }
    /* the size report is not a key: drop resize news it caused */
    {
        int i, j = kh;
        for (i = kh; i != kt; i = (i + 1) % KQ) if (kq[i] != K_RESIZE) { kq[j] = kq[i]; j = (j + 1) % KQ; }
        kt = j;
    }
    return 1;
}

void term_close(void)
{
    if (!isopen) return;
    collect(1);
    term_outs("\x9b" "12}");
    if (term_cterm) term_outs("\x1b[?2004l");
    term_flush();
    SetMode(tin, 0);
    isopen = 0;
}

void term_cleanup(void)
{
    term_close();
    if (wpkt) FreeDosObject(DOS_STDPKT, wpkt);
    if (rport) DeleteMsgPort(rport);
    if (paste) FreeVec(paste);
    wpkt = NULL; rport = NULL; paste = NULL;
}
