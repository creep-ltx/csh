/* line_test - the line rules (src/line.c) on the PC: cases taken from the
 * 3.2 ROM Shell's behaviour (Knowledge/amiga/writing-a-shell.md, D).
 * Built and run by tests/run.sh, with ASan. */
#include <stdio.h>
#include <string.h>
#include "../src/line.h"

static int bad, ran;

static int getvar(const char *n, char *b, int s)
{
    static const char *v[][2] = { {"v", "world"}, {"a_b", "under"}, {"r", ">RAM:x10"},
        {"c", "; comment"}, {"m", "a=b"}, {"RC", "0"}, {"empty", ""}, {0, 0} };
    for (int i = 0; v[i][0]; i++)
        if (!strcasecmp(v[i][0], n)) { snprintf(b, s, "%s", v[i][1]); return strlen(b); }
    return -1;
}

static int getalias(const char *n, char *b, int s)
{
    static const char *a[][2] = { {"ev", "Echo 7 alias [world]"}, {"br", "Echo [] end"},
        {"ll", "List"}, {0, 0} };
    for (int i = 0; a[i][0]; i++)
        if (!strcasecmp(a[i][0], n)) { snprintf(b, s, "%s", a[i][1]); return strlen(b); }
    return -1;
}

static int run(const char *cmd, char *b, int s)
{
    if (!strcmp(cmd, "Echo inner")) return snprintf(b, s, "inner\n");
    if (!strcmp(cmd, "two")) return snprintf(b, s, "line1\nline2\n");
    return -1;
}

static void vars(const char *in, const char *want)
{
    char out[LINE_MAX];
    ran++;
    line_vars(in, out, sizeof out, getvar);
    if (strcmp(out, want)) { printf("FAIL vars  [%s] -> [%s], wanted [%s]\n", in, out, want); bad++; }
}

static void alias(const char *in, const char *want)
{
    char out[LINE_MAX];
    ran++;
    if (line_alias(in, out, sizeof out, getalias) <= 0) strcpy(out, in);
    if (strcmp(out, want)) { printf("FAIL alias [%s] -> [%s], wanted [%s]\n", in, out, want); bad++; }
}

static void bq(const char *in, const char *want)
{
    char out[LINE_MAX];
    ran++;
    line_backquotes(in, out, sizeof out, run);
    if (strcmp(out, want)) { printf("FAIL bq    [%s] -> [%s], wanted [%s]\n", in, out, want); bad++; }
}

/* want: "cmd|args|in|out|err|flags" with args' "\n" left off; flags a=>> e=*>> b=<> p=pipe */
static void parse(const char *in, const char *want)
{
    struct cmdline c;
    char got[4096], fl[8] = "", *f = fl;
    ran++;
    line_parse(in, &c);
    if (c.out_append) *f++ = 'a';
    if (c.err_append) *f++ = 'e';
    if (c.inout) *f++ = 'b';
    if (c.pipe) *f++ = 'p';
    *f = 0;
    if (c.arglen < 1 || c.args[c.arglen - 1] != '\n') { printf("FAIL parse [%s]: args do not end in LF\n", in); bad++; }
    else c.args[c.arglen - 1] = 0;
    snprintf(got, sizeof got, "%s|%s|%s|%s|%s|%s", c.cmd, c.args, c.in, c.out, c.err, fl);
    if (strcmp(got, want)) { printf("FAIL parse [%s] -> [%s], wanted [%s]\n", in, got, want); bad++; }
}

int main(void)
{
    vars("Echo \"1 quoted [$v]\"", "Echo \"1 quoted [world]\"");
    vars("Echo 2 star [*$v]", "Echo 2 star [$v]");
    vars("Echo \"3 qstar [*$v]\"", "Echo \"3 qstar [*$v]\"");
    vars("Echo 4 unknown [$nosuch]", "Echo 4 unknown [$nosuch]");
    vars("Echo \"5 brace [${v}]\"", "Echo \"5 brace [world]\"");
    vars("Echo 6 [$a_b] [${a_b}]", "Echo 6 [$a_b] [under]");
    vars("Echo 10 $r", "Echo 10 >RAM:x10");
    vars("Echo 11 $c", "Echo 11 ; comment");
    vars("Echo $V$v", "Echo worldworld");
    vars("Echo $ alone $", "Echo $ alone $");
    vars("Echo \"**$v\"", "Echo \"**world\"");
    vars("Echo \"*\"$v*\"\"", "Echo \"*\"world*\"\"");
    vars("Echo x$empty.y", "Echo x.y");
    vars("Echo ${nosuch}", "Echo ${nosuch}");
    vars("Echo ${}", "Echo ${}");

    alias("ev", "Echo 7 alias [world]");
    alias("ev more", "Echo 7 alias [world] more");
    alias("  br middle", "Echo middle end");
    alias("br", "Echo  end");
    alias("evx", "evx");
    alias("\"ev\"", "\"ev\"");
    alias("LL RAM:", "List RAM:");
    alias("Echo ev", "Echo ev");

    bq("Echo \"8 bq [`Echo inner`]\"", "Echo \"8 bq [inner]\"");
    bq("Echo 9 [`Echo inner`]", "Echo 9 [inner]");
    bq("Echo `two`", "Echo line1 line2");
    bq("Echo *`x", "Echo `x");
    bq("Echo \"*`x\"", "Echo \"*`x\"");
    bq("Echo `nothing`!", "Echo !");

    parse("Echo hello", "Echo|hello||||");
    parse("  Echo   hello   world  ", "Echo|hello   world||||");
    parse("Echo >RAM:r1 before", "Echo|before||RAM:r1||");
    parse("Echo end >RAM:r2", "Echo|end||RAM:r2||");
    parse("Echo mid >RAM:r3 dle", "Echo|mid dle||RAM:r3||");
    parse("Echo x > RAM:sp", "Echo|x||RAM:sp||");
    parse("Echo x >>RAM:ap", "Echo|x||RAM:ap||a");
    parse("Type <RAM:in", "Type||RAM:in|||");
    parse("perr *>RAM:e1", "perr||||RAM:e1|");
    parse("perr *>>RAM:e1 x", "perr|x|||RAM:e1|e");
    parse("Echo hi ; a comment", "Echo|hi||||");
    parse("Echo \"a ; b\" ; c", "Echo|\"a ; b\"||||");
    parse("Echo \"a *\" ; b\"", "Echo|\"a *\" ; b\"||||");
    parse("\"My Prog\" arg", "My Prog|arg||||");
    parse("Echo a | pcat", "Echo|a | pcat||||p");
    parse("Echo \"a | b\"", "Echo|\"a | b\"||||");
    parse("Echo a *| b", "Echo|a *| b||||");
    parse(">RAM:pre Echo first", "Echo|first||RAM:pre||");
    parse("Echo x >\"RAM:a b\"", "Echo|x||RAM:a b||");
    parse("Cmd <>CON:0/0/100/100/t", "Cmd||CON:0/0/100/100/t|CON:0/0/100/100/t||b");
    parse("Echo a=b \"q=r\"", "Echo|a=b \"q=r\"||||");
    parse("; only a comment", "|||||");
    parse("", "|||||");
    parse("Echo hello\n", "Echo|hello||||");
    parse("Echo a>b", "Echo|a>b||||");
    parse("Echo one\ntwo\n", "Echo|one\ntwo||||");
    parse("Cmd a >RAM:o\nline2\nline3", "Cmd|a\nline2\nline3||RAM:o||");
    parse("Echo x ; c\nmore", "Echo|x\nmore||||");
    printf("%d of %d line checks passed\n", ran - bad, ran);
    return bad != 0;
}
