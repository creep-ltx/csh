/* lang_test - the csh language (src/lang.c, src/builtin.c) on the PC with
 * the fake system of tests/fakeos.c: each case is csh text and what it
 * prints; an Amiga command shows as [Name|its Amiga argument line].
 * Built and run by tests/run.sh, with ASan. */
#include <stdio.h>
#include <string.h>
#include "../src/lang.h"

void fake_reset(const char *in);
const char *fake_out(void);
extern long fake_rcs[256];
extern int fake_quit;

static int bad, ran;

static void runin(const char *in, const char *text, const char *want)
{
    const char *got;
    ran++;
    fake_reset(in);
    sh_opt_e = sh_opt_x = 0;
    lang_line(text, 0);
    got = fake_out();
    if (strcmp(got, want)) {
        printf("FAIL: %s\n  got:    ", text);
        for (const char *p = got; *p; p++) printf(*p == '\n' ? "\\n" : "%c", *p);
        printf("\n  wanted: ");
        for (const char *p = want; *p; p++) printf(*p == '\n' ? "\\n" : "%c", *p);
        printf("\n");
        bad++;
    }
}

static void run(const char *text, const char *want) { runin("", text, want); }

static void check(const char *text, int want)
{
    int got;
    ran++;
    fake_reset("");
    got = lang_parse_check(text);
    if (got != want) { printf("FAIL parse_check [%s]: %d, wanted %d (%s)\n", text, got, want, fake_out()); bad++; }
}

int main(void)
{
    /* words and quotes */
    run("echo hello world", "hello world\n");
    run("echo 'single $x' \"double $x\"", "single $x double \n");
    run("x=5; echo \"[$x]\" [${x}] $x$x", "[5] [5] 55\n");
    run("echo a\\ b \"c\\\"d\" 'e\\f'", "a b c\"d e\\f\n");
    run("echo \"$nosuch\"x", "x\n");
    run("echo one;echo two", "one\ntwo\n");
    run("echo a # comment", "a\n");
    run("echo a#b", "a#b\n");
    run("v='a  b'; echo $v; echo \"$v\"", "a b\na  b\n");
    run("echo -n hi; echo there", "hithere\n");
    run("echo -e 'a\\tb\\nc'", "a\tb\nc\n");
    run("echo ${u:-dflt} ${u-d2} ${u:+no}; u=x; echo ${u:+yes} ${#u}", "dflt d2\nyes 1\n");
    run("f=file.tar.gz; echo ${f%.gz} ${f%%.*} ${f#*.} ${f##*.}", "file.tar file tar.gz gz\n");
    run("echo $((1 + 2 * 3)) $(( (1+2)*3 )) $((10/3)) $((10%3)) $((-4+1))", "7 9 3 1 -3\n");
    run("n=4; echo $((n * 2)) $(($n+1)) $((n > 3 && n < 5))", "8 5 1\n");
    run("echo $(echo inner) `echo back`", "inner back\n");
    run("echo \"$(echo a; echo b)\"", "a\nb\n");
    run("x=$(echo  spaced   out ); echo \"$x\"", "spaced out\n");
    run("x=$(printf 'a  b'); echo \"$x\"; echo $x", "a  b\na b\n");
    run("echo $?; false; echo $?; true; echo $?", "0\n1\n0\n");
    run("echo $$", "7\n");
    /* Amiga commands: the words back as an Amiga line */
    run("List Work: ALL", "[List|Work: ALL]\n");
    run("Copy \"my file\" RAM:", "[Copy|\"my file\" RAM:]\n");
    run("Copy 'a*b' RAM:", "[Copy|\"a**b\" RAM:]\n");
    run("List #?.txt", "[List|#?.txt]\n");
    run("List *.txt", "[List|#?.txt]\n");
    run("List '*.txt'", "[List|\"**.txt\"]\n");
    run("Type * >RAM:x", "[Type|* >RAM:x]\n");
    run("Echo \"say \\\"hi\\\"\"", "say \"hi\"\n");
    run("List DIRS SINCE=yesterday", "[List|DIRS SINCE=yesterday]\n");
    run("Copy a \"\"", "[Copy|a \"\"]\n");
    run("x='two words'; Delete $x", "[Delete|two words]\n");
    run("x='two words'; Delete \"$x\"", "[Delete|\"two words\"]\n");
    run("Type <in >out", "[Type| <in >out]\n");
    run("List >>log 2>err", "[List| >>log *>err]\n");
    run("List &>both", "[List| >both 2>&1]\n");
    run("List 2>&1 *>e2", "[List| *>e2 2>&1]\n");
    run("Type <<END\nline $x\nEND\n", "{heredoc T:hd1:line \n}[Type| <T:hd1]\n");
    run("x=1; Type <<'END'\nline $x\nEND\n", "{heredoc T:hd2:line $x\n}[Type| <T:hd2]\n");
    run("Type <<<word", "{heredoc T:hd3:word\n}[Type| <T:hd3]\n");
    run("List | Sort", "{job List | Sort}[spawn List| in=1 out=11][last Sort| in=10]{end rc=0 own}\n");
    run("Wait 5 &", "{job & Wait 5}[spawn Wait|5 in=1 out=2]{end rc=0}\n");
    run("List Work: | Search * x | Sort", "{job List Work: | Search * x | Sort}[spawn List|Work: in=1 out=11][spawn Search|* x in=10 out=13][last Sort| in=12]{end rc=0 own}\n");
    run("List | while read l; do echo \"got $l\"; done", "{job List | while ...}[spawn List| in=1 out=11]{sel in=10 out=0}{/sel}{close 10}{end rc=0 own}\n");
    run("echo hi | Sort", "{job echo hi | Sort}{sel in=1 out=51}hi\n{/sel}{close 51}[last Sort| in=151]{end rc=0 own}\n");
    run("List >f.txt | Sort", "{job List | Sort}[spawn List| in=1 out=11 >f.txt][last Sort| in=10]{end rc=0 own}\n");
    run("jobs; fg; kill %1; wait; wait 5", "[jobs]\n[fg]\n[kill]\n[wait]\n[wait|5]\n");
    /* control */
    run("if true; then echo yes; else echo no; fi", "yes\n");
    run("if false; then echo yes; elif true; then echo elif; else echo no; fi", "elif\n");
    run("if [ 3 -gt 2 ]; then echo big; fi", "big\n");
    run("[ abc = abc ] && echo same || echo diff", "same\n");
    run("[ -f a.txt ] && echo file; [ -d dir ] && echo drawer; [ -e none ] || echo none", "file\ndrawer\nnone\n");
    run("test -z \"\" && test -n x && echo both", "both\n");
    run("! false && echo not", "not\n");
    run("for i in 1 2 3; do echo $i; done", "1\n2\n3\n");
    run("for f in *.txt; do echo $f; done", "a.txt\nb.txt\n");
    run("for f in *.none; do echo $f; done", "*.none\n");
    run("i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done", "0\n1\n2\n");
    run("i=0; until [ $i -ge 2 ]; do i=$((i+1)); done; echo $i", "2\n");
    run("for i in 1 2 3 4; do if [ $i = 3 ]; then break; fi; echo $i; done", "1\n2\n");
    run("for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo $i; done", "1\n3\n");
    run("for a in x y; do for b in 1 2; do [ $b = 2 ] && continue 2; echo $a$b; done; done", "x1\ny1\n");
    run("case foo.txt in *.c) echo c;; *.txt|*.doc) echo text;; *) echo other;; esac", "text\n");
    run("case X in [a-z]) echo low;; [A-Z]) echo up;; esac", "up\n");
    run("j='[1] 6'; case \"$j\" in \"[1] \"*) echo yes;; *) echo no;; esac", "yes\n");
    run("case 'a*b' in 'a*b') echo literal;; esac; case axb in 'a*b') echo wrong;; a*b) echo glob;; esac", "literal\nglob\n");
    run("f='x.*'; echo ${f%'.*'} ${f%.*}", "x x\n");
    run("{ echo a; echo b; }", "a\nb\n");
    run("(echo sub)", "sub\n");
    run("if true\nthen\n  echo multi\nfi\n", "multi\n");
    /* ~ and braces */
    run("echo ~ ~/x", "SYS: SYS:x\n");
    run("HOME=Work:; echo ~ ~/Docs '~' \"~\" a~b", "Work: Work:Docs ~ ~ a~b\n");
    run("HOME=Work:Me; echo ~/x", "Work:Me/x\n");
    run("echo a{b,c}d {x,y}{1,2}", "abd acd x1 x2 y1 y2\n");
    run("echo {1..4} {3..1} {a..c} {-1..1}", "1 2 3 4 3 2 1 a b c -1 0 1\n");
    run("echo '{a,b}' \"{a,b}\" {a} {} ${HOME:-x}{1,2}", "{a,b} {a,b} {a} {} x1 x2\n");
    run("echo {a,{b,c}}z", "az bz cz\n");
    run("Copy file{1,2}.txt RAM:", "[Copy|file1.txt file2.txt RAM:]\n");
    run("for i in {1..3}; do echo n$i; done", "n1\nn2\nn3\n");
    run("x={a,b}; echo $x", "{a,b}\n");
    /* local */
    run("v=out; f() { local v=in; echo $v; }; f; echo $v", "in\nout\n");
    run("f() { local w; w=set; echo $w; }; f; echo \"[$w]\"", "set\n[]\n");
    run("local x=1", "csh: local: only in a function\n");
    run("v=1; f() { local v=2; g; echo f$v; }; g() { echo g$v; v=3; }; f; echo top$v", "g2\nf3\ntop1\n");
    /* functions */
    run("greet() { echo hello $1; }; greet you", "hello you\n");
    run("f() { echo \"fn $1\"; }; f x\n", "fn x\n");
    run("f() { echo $# $@; }; f a b c", "3 a b c\n");
    run("f() { return 3; }; f; echo $?", "3\n");
    run("function g { echo in g; }; g", "in g\n");
    run("fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }; fact 5", "120\n");
    run("f() { for x in \"$@\"; do echo \"[$x]\"; done; }; f 'a b' c", "[a b]\n[c]\n");
    /* builtins */
    run("printf '%s=%d\\n' a 1 b 2", "a=1\nb=2\n");
    run("printf '%5s|%-4s|%03d|%x\\n' ab cd 7 255", "   ab|cd  |007|ff\n");
    runin("one two three\n", "read a b; echo \"$a|$b\"", "one|two three\n");
    runin("x\ny\n", "while read l; do echo got $l; done", "got x\ngot y\n");
    run("set -- p q r; echo $2 $#; shift; echo $1", "q 3\nq\n");
    run("eval 'echo evaled'", "evaled\n");
    run("alias ll='List DIRS'; ll Work:", "[List|DIRS Work:]\n");
    run("Set name Amiga value; echo $name", "Amiga value\n");
    run("Echo hi NOLINE", "[Echo|hi NOLINE]\n");
    run("x=1 List", "[List|]\n");
    run("break 3", "[break|3]\n");
    run("Eval 1 + 1", "[Eval|1 + 1]\n");
    run("export G=glob; echo $G", "glob\n");
    run("echo a > f.txt", "{redir >f.txt}a\n{/redir}");
    run("set -e; false; echo not here", "");
    run("pwd", "Work:\n");
    /* complete or not */
    check("echo hi", 1);
    check("f() { echo \"fn $1\"; }; f x\n", 1);
    check("if true; then", 0);
    check("echo 'open", 0);
    check("for i in 1 2; do echo $i", 0);
    check("echo \"$(echo a", 0);
    check("cat <<END\nbody\n", 0);
    check("cat <<END\nbody\nEND\n", 1);
    check("fi", 1);
    check("echo )", -1);
    check("ls |", 0);
    check("a &&", 0);
    printf("%d of %d language checks passed\n", ran - bad, ran);
    return bad != 0;
}
