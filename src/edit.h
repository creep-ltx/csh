/* edit.h - csh's prompt: the console (term.c), the keys (keys.c), the
 * history (hist.c), completion and colours (comp.c), the line editor
 * (edit.c), the settings (prefs.c). */
#ifndef CSH_EDIT_H
#define CSH_EDIT_H

#include <exec/types.h>
#include <dos/dos.h>

/* keys: characters are their code (Latin-1); the rest below, the
 * modifiers or'ed in: TM_CTRL | 'r' is Ctrl+R (Sumi's numbering). */
enum {
    K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INS, K_DEL, K_BS, K_TAB, K_ENTER, K_ESC, K_HELP,
    K_F1,                       /* K_F1 .. K_F1 + 11 */
    K_PASTE = 0x120000,         /* a marked paste: term_paste() has it */
    K_RESIZE
};
#define TM_SHIFT 0x01000000
#define TM_ALT   0x02000000
#define TM_CTRL  0x04000000
#define TM_MASK  0x07000000
#define K_BASE(k) ((k) & ~TM_MASK)

/* term.c */
extern int term_cols, term_cterm, term_col0;
int term_possible(BPTR in, BPTR out);
int term_open(BPTR in, BPTR out);
void term_close(void);
void term_cleanup(void);
int term_key(void);
int term_pending(void);
const char *term_paste(int *len);
void term_out(const char *s, int n);
void term_outs(const char *s);
void term_num(int n);
void term_move(int n, char f);
void term_flush(void);

/* keys.c: the actions a key can do */
enum {
    A_NONE, A_LEFT, A_RIGHT, A_WORDLEFT, A_WORDRIGHT, A_START, A_END,
    A_BACKSPACE, A_DELETE, A_DELWORDLEFT, A_DELWORDRIGHT, A_KILLEND, A_KILLSTART,
    A_YANK, A_PREV, A_NEXT, A_SEARCH, A_COMPLETE, A_ACCEPT, A_SUGGEST, A_SUGGESTWORD,
    A_CLEAR, A_CANCEL, A_EOF, A_TRANSPOSE, A_FIRST, A_LAST, A_LASTARG,
    A_COUNT
};
extern char keys_bad[120];
void keys_load(void);
int keys_find(int key);

/* prefs.c: ENVARC:CShell/csh.prefs */
enum { C_COMMAND, C_UNKNOWN, C_BUILTIN, C_STRING, C_VAR, C_OPTION, C_COMMENT,
       C_REDIR, C_SUGGEST, C_PROMPTRC, C_COUNT };
extern int prefs_history, prefs_suggest, prefs_colour;
const char *prefs_sgr(int what);    /* the colour as an SGR string, "" for none */
void prefs_load(void);

/* hist.c */
void hist_load(void);
void hist_add(const char *line);
void hist_save(void);
int hist_count(void);
const char *hist_get(int i);        /* 0 = the oldest */
void hist_refresh(void);            /* lines other csh tabs added meanwhile */
void hist_print(BPTR out);
void hist_end(void);                /* csh ends: the lines freed */
void hist_load_once(void);          /* loaded if it was not (a prompt without the editor) */

/* comp.c */
enum { W_NONE, W_COMMAND, W_BUILTIN, W_UNKNOWN };
int comp_kind(const char *word, int n, int cheap);   /* what a command word is, for its colour; cheap: no disk */
void comp_end(void);                /* csh ends: the tables freed, C:'s locks given back */
int comp_complete(char *line, int *len, int *cur, int max);   /* Tab; TRUE if it changed */
void comp_forget(void);             /* the command table is built again when next needed */
int comp_listing(void);             /* a list was shown under the line */
int comp_takesmany(const char *name);   /* its template: 1 several names (/M), 0 one, -1 none seen */

/* edit.c */
int edit_line(const char *prompt, char *line, int size);   /* -1 = end (Ctrl-\), -2 = Ctrl-C */
extern const char *edit_rprompt;    /* RPROMPT: shown at the right edge while the line leaves room */
void edit_droppaste(void);          /* the rest of a multi-line paste dropped (Ctrl-C) */
void edit_end(void);                /* csh ends: the prompt's memory freed */

#endif
