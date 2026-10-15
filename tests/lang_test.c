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
    lang_end();                 /* each case from nothing: no functions, arrays, traps of the last */
    fake_reset(in);
    sh_opt_e = sh_opt_x = sh_opt_u = sh_opt_f = sh_opt_pipefail = 0;
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

static const char *hl[] = { "List Work: ALL\n", "echo one two three\n", "Copy a b\n" };   /* the oldest first */
static const char *hback(int k) { return k >= 1 && k <= 3 ? hl[3 - k] : NULL; }

static void bang(const char *in, const char *want)
{
    char out[1024];
    int r;
    ran++;
    fake_reset("");
    r = bang_expand(in, out, sizeof out, hback);
    if (r < 0) snprintf(out, sizeof out, "{err}%s", fake_out());
    else if (!r) snprintf(out, sizeof out, "{same}");
    if (strcmp(out, want)) { printf("FAIL bang [%s]: [%s], wanted [%s]\n", in, out, want); bad++; }
}

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
    run("List *.txt", "[List|a.txt b.txt]\n");
    run("Dir *.txt; Dir *.info; List *.none", "[Dir|#?.txt]\n[Dir|#?.info]\n[List|#?.none]\n");
    run("List '*.txt'", "[List|\"**.txt\"]\n");
    run("Type * >RAM:x; Type '*'", "[Type|a.txt b.txt c.info dir >RAM:x]\n[Type|\"**\"]\n");
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
    run("x=1; Type <<'END'\nline $x\nEND\n", "{heredoc T:hd1:line $x\n}[Type| <T:hd1]\n");
    run("Type <<<word", "{heredoc T:hd1:word\n}[Type| <T:hd1]\n");
    run("List | Sort", "{job List | Sort}[spawn List| in=1 out=11][last Sort| in=10]{end rc=0 own}\n");
    run("Wait 5 &", "{job & Wait 5}[spawn Wait|5 in=1 out=2]{end rc=0}\n");
    run("List Work: | Search * x | Sort", "{job List Work: | Search * x | Sort}[spawn List|Work: in=1 out=11][spawn Search|a.txt b.txt c.info dir x in=10 out=13][last Sort| in=12]{end rc=0 own}\n");
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
    check("fi", -1);
    check("echo $(case a in a) echo x;; esac)", 1);
    check("echo \"${x:-\"a b\"}\"", 1);
    check("echo )", -1);
    check("ls |", 0);
    check("a &&", 0);
    /* the 2026-10-13 audit: each bug found, kept fixed */
    run("set -- a b c; f() { shift; echo in $1; }; f x y; echo $1 $#", "in y\na 3\n");
    run("f() { set -- z; }; set -- a b; f; echo $1", "a\n");
    run("f() { f() { echo new; }; echo old; echo after; }; f; f", "old\nafter\nnew\n");
    run("x=x; echo $((x)); echo next", "csh: bad arithmetic: x\n");
    run("echo $((------------------------------------------------------------------------------------------------------------1))", "csh: bad arithmetic: ------------------------------------------------------------------------------------------------------------1\n");
    run("unset x; echo [${x#a}] [${x%b}]", "[] []\n");
    run("echo $((1000000*1000))", "1000000000\n");
    run("x=$(exit 3); echo still $?", "still 3\n");
    run("( exit 4 ); echo after $?", "after 4\n");
    run("f() { echo $(return 2) r; echo here; }; f", "r\nhere\n");
    run("a=$(false); echo $?; b=$(true); echo $?", "1\n0\n");
    run("g() { echo $#; }; f() { g \"$@\"; }; f; f a 'b c'", "0\n2\n");
    run("echo ${u:-'a b'} \"${u:-'a b'}\" ${u:-\"c  d\"}", "a b 'a b' c  d\n");
    run("x=y; echo \"${x:-\"a b\"}\" \"${u:-\"a b\"}\"", "y a b\n");
    run("echo $(case a in a) echo hit;; esac)", "hit\n");
    run("echo $(echo case)", "case\n");
    run("List #(a|b).txt ~(x)", "[List|#(a|b).txt ~(x)]\n");
    run("echo hi 1>&2", "{redir >*}hi\n{/redir}");
    run("List 2>/dev/null", "[List| *>NIL:]\n");
    run("alias ls='ls -F'; ls x", "[ls|-F x]\n");
    run("alias e='echo'; e '$HOME' 'it\"s' \"a'b\"", "$HOME it\"s a'b\n");
    run("n=0; c() { n=$((n+1)); echo List; }; $(c) | Sort; echo ran $n", "{job $(c) | Sort}{sel in=1 out=51}[List|]\n{/sel}{close 51}[last Sort| in=151]{end rc=0 own}\nran 0\n");
    run("{ echo hi; } >NOSUCH:x; echo rc $?", "{redir failed}rc 20\n");
    run("f() { echo in; }; f >NOSUCH:x; echo rc $?", "{redir failed}rc 20\n");
    run("echo >NOSUCH:x hi; echo rc $?", "{redir failed}rc 20\n");
    run("f() { f; }; f; echo rc $?", "csh: too deep: f\nrc 20\n");
    run("x=1; f() { local x=2; echo $x; }; f; echo $x", "2\n1\n");
    run("export G=g; f() { local G=l; echo $G; }; f; echo $G", "l\ng\n");
    run("export G=g; G=p List; echo $G", "[List|]\ng\n");
    run("A=1 B=2 C=3 D=4 E=5 F=6 G=7 H=8 I=9 J=10 List; echo [$J]", "[List|]\n[]\n");
    run("done", "csh: syntax error near done\n");
    run("x=$(printf %d abc); echo \"[$x]\"", "[0]\n");
    run("cat <<E\na \\\" b\nE\n", "{heredoc T:hd1:a \\\" b\n}[cat| <T:hd1]\n");
    run("f() { cat <<E; }\nbody\nE\nf", "{heredoc T:hd1:body\n}[cat| <T:hd1]\n");
    run("echo ${}", "csh: bad substitution: ${}\n");
    run("X=0; export X=1; echo $X", "1\n");
    run("export X=1; X=2; echo $X; unset X; echo [$X]", "2\n[]\n");
    run("[ \"(\" = \"(\" ]; echo $?; x='!'; [ \"$x\" = '!' ]; echo $?; [ ! -n '' ]; echo $?", "0\n0\n0\n");
    run("[ \\( a = a \\) ]; echo $?", "0\n");
    run("printf '%.5d|%5s|%-3d|%05d|%x|%c\\n' 42 ab 7 -3 255 xyz", "00042|   ab|7  |-0003|ff|x\n");
    run("printf '\\101\\x42%b|' '\\0103'; printf 'a\\cb'; echo", "ABC|a\n");
    run("printf %d abc; echo \" rc $?\"", "csh: printf: not a number: abc\n0 rc 10\n");
    run("x=$(printf '%.300d' 7); echo ${#x}; y=$(printf %s \"$x\"); echo ${#y}", "300\n300\n");
    run("set -- a b; shift -1; echo $? $1; shift x; echo $?", "csh: shift: not a count: -1\n10 a\ncsh: shift: not a count: x\n10\n");
    run("echo I want to go; echo hi NOLINE; echo a TO b; echo -n to; echo", "I want to go\n[echo|hi NOLINE]\n[echo|a TO b]\nto\n");
    run("alias foo bar=baz", "[alias|foo bar=baz]\n");
    runin("x\n", "read -k a; echo $?", "csh: read: unknown option -k\n10\n");
    /* read -n -N -t -s (0.1b9) */
    runin("abcdef\n", "read -n 2 a; read -n3 b; echo $a $b", "ab cde\n");
    runin("ab\ncd\n", "read -n 5 a; echo [$a]; read b; echo [$b]", "[ab]\n[cd]\n");
    runin("ab\ncd\n", "read -N 4 a; echo \"[$a]\"", "[ab\nc]\n");
    runin("secret\n", "read -rs p; echo got $p", "got secret\n");
    runin("", "read -t 1 a; echo $?", "142\n");
    runin("y\n", "read -t 0.5 -n1 a; echo $? $a", "0 y\n");
    runin("x\n", "read -t soon a; echo $?", "csh: read: not a time: soon\n10\n");
    runin("x\n", "read -n; echo $?", "csh: read: a value is wanted after -n\n10\n");
    run("case aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab in *a*a*a*a*a*a*a*a*a*a*a*a*a*a*c) echo no;; *a*b) echo yes;; esac", "yes\n");
    run("for f in *.txt; do echo $f; done", "a.txt\nb.txt\n");
    run("return x", "csh: return: not a number: x\n");
    run("List | Sort | NoSuchCmd; echo rc $?", "NoSuchCmd: Unknown command\nrc 10\n");
    /* 2026-10-14: bash's missing parts */
    run("echo $((2**10)) $((7>3?1:0)) $((5&3)) $((5|3)) $((5^3)) $((1<<4)) $((-7/2)) $((2**3**2))", "1024 1 1 7 6 16 -3 512\n");
    run("x=5; ((x++)); echo $x; ((x+=10)); echo $x; ((x*=2, x-=1)); echo $x; echo $((x--)) $x $((--x))", "6\n16\n31\n31 30 29\n");
    run("((0)); echo $?; ((3)); echo $?; let 'y=2*3' z=y+1; echo $y $z $?", "1\n0\n6 7 0\n");
    run("i=1; echo $(( i > 0 && (j=5) )) $j; echo $(( 0 && (k=5) )) [$k]", "1 5\n0 []\n");
    run("echo $((16#ff)) $((2#101)) $((010)) $((0x10))", "255 5 8 16\n");
    run("for ((i=0; i<3; i++)); do echo $i; done", "0\n1\n2\n");
    run("for ((i=10; i>0; i-=4)) { echo $i; }", "10\n6\n2\n");
    run("echo $((1/0)); echo after", "csh: division by zero in 1/0\n");
    run("a=(one two three); echo ${a[0]} ${a[2]} ${#a[@]} $a", "one three 3 one\n");
    run("a=(x 'y z'); for i in \"${a[@]}\"; do echo [$i]; done; echo ${a[*]}", "[x]\n[y z]\nx y z\n");
    run("a=(1 2); a+=(3 4); a[6]=7; echo ${a[@]} ${#a[@]} ${!a[@]}", "1 2 3 4 7 5 0 1 2 3 6\n");
    run("a=(p q r s); echo ${a[-1]} ${a[@]:1:2} ${a[@]: -2}", "s q r r s\n");
    run("a=(a b c); unset 'a[1]'; echo ${a[@]} ${#a[@]}; unset a; echo [${a[@]}]", "a c 2\n[]\n");
    run("declare -A m; m[one]=1; m[two]=2; echo ${m[one]} ${m[two]} ${#m[@]}; for k in \"${!m[@]}\"; do echo $k=${m[$k]}; done", "1 2 2\none=1\ntwo=2\n");
    run("declare -A m=([a]=x [b]='y z'); echo \"${m[b]}\"", "y z\n");
    run("a=(); echo ${#a[@]}; for x in \"${a[@]}\"; do echo no; done; echo end", "0\nend\n");
    run("i=2; a=(x y z w); echo ${a[i]} ${a[$i+1]} ${a[i-1]}", "z w y\n");
    run("a=(x y); a[1]+=z; s=ab; s+=cd; echo ${a[1]} $s", "yz abcd\n");
    run("f() { local -a v=(1 2 3); echo ${#v[@]}; }; v=(9); f; echo ${v[@]}", "3\n9\n");
    run("s='hello world'; echo ${s:0:5} ${s:6} ${s: -5:3} ${s:1:-1}", "hello world wor ello worl\n");
    run("s='a.b.c'; echo ${s/./-} ${s//./-} ${s/#a/A} ${s/%c/C} ${s/b}", "a-b.c a-b-c A.b.c a.b.C a..c\n");
    run("s='Hello World'; echo ${s^^} ${s,,} ${s^} ${s,}", "HELLO WORLD hello world Hello World hello World\n");
    run("x=y; y=deep; echo ${!x}", "deep\n");
    run("echo $'a\\tb\\n' $'it\\'s' $'\\x41\\101'", "a\tb\n it's AA\n");
    run("set -- a b c d; echo ${@:2} ${@:2:2} ${#@}", "b c d b c 4\n");
    run("[[ abc == a* ]] && echo glob; [[ abc == 'a*' ]] || echo literal; [[ -f a.txt && ! -d a.txt ]] && echo file", "glob\nliteral\nfile\n");
    run("x='a b'; [[ $x = 'a b' ]] && echo nosplit; [[ -z $nothing ]] && echo empty; [[ b > a ]] && echo gt", "nosplit\nempty\ngt\n");
    run("[[ 10 -gt 9 && ( 1 -eq 2 || 3 -eq 3 ) ]] && echo yes", "yes\n");
    run("[[ abc123 =~ ^([a-z]+)([0-9]+)$ ]] && echo ${BASH_REMATCH[0]} ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}", "abc123 abc 123\n");
    run("[[ x.y =~ x.y ]] && echo dot; [[ xzy =~ 'x.y' ]] || echo quoted; [[ ab =~ a|c ]] && echo alt", "dot\nquoted\nalt\n");
    run("[[ aaa =~ ^a{2,3}$ ]] && echo rep; [[ hello =~ l+o ]] && echo ${BASH_REMATCH[0]}; [[ x =~ [[:digit:]] ]] || echo nodigit", "rep\nllo\nnodigit\n");
    run("[[ -v HOME ]] || echo nohome; x=1; [[ -v x ]] && echo set; a=(1); [[ -v a[0] ]] && echo elem", "nohome\nset\nelem\n");
    run("[[ a.txt -nt b.txt ]] || echo older; [[ a.txt -ot b.txt ]] && echo ot", "older\not\n");
    run("readonly r=5; r=6; echo $r; unset r; echo $r", "csh: readonly variable: r\n5\ncsh: readonly variable: r\n5\n");
    run("declare -r q=1; declare -p q; a=(x); declare -p a", "declare -r q=\"1\"\ndeclare -a a=([0]=\"x\")\n");
    /* declare -i -l -u (0.1b9) */
    run("declare -i n=2+3; echo $n; n=n*2; echo $n; n+=4; echo $n", "5\n10\n14\n");
    run("declare -i n; n='7 / 2'; echo $n; declare -p n", "3\ndeclare -i n=\"3\"\n");
    run("x=4; declare -i x; x+=1; echo $x", "5\n");
    run("declare -l s=MiXed; echo $s; s=\xc5NGA; echo $s; declare -u t=abc; echo $t; declare -p t", "mixed\n\xe5nga\nABC\ndeclare -u t=\"ABC\"\n");
    run("s=Hello; declare -u s; echo $s; declare +u s; s=Low; echo $s", "HELLO\nLow\n");
    run("declare -ir k=3; declare -p k; k=4; echo $k", "declare -ir k=\"3\"\ncsh: readonly variable: k\n3\n");
    run("declare -i a; a[1]=2*3; echo ${a[1]}", "6\n");
    /* printf %q, ${x@Q} ${x@U} ${x@L} ${x@u} ${x@E} (0.1b9) */
    run("printf '%q\\n' 'a b' \"it's\" '' 'Work:x.c'", "a\\ b\nit\\'s\n''\nWork:x.c\n");
    run("printf '%q\\n' $'a\\tb'", "$'a\\tb'\n");
    run("x=\"it's me\"; echo ${x@Q}; eval y=${x@Q}; echo \"$y\"", "'it'\\''s me'\nit's me\n");
    run("x='hello World'; echo ${x@U} ${x@L} ${x@u}", "HELLO WORLD hello world Hello World\n");
    run("x='a\\tb'; echo \"${x@E}\"", "a\tb\n");
    /* case ;& and ;;& (0.1b9) */
    run("case b in a) echo A;; b) echo B;& c) echo C;; d) echo D;; esac", "B\nC\n");
    run("case abc in a*) echo 1;;& *c) echo 2;;& x*) echo 3;; *) echo 4;; esac", "1\n2\n4\n");
    run("case x in x) echo X;& esac; echo end", "X\nend\n");
    /* command, exec (0.1b9) */
    run("echo() { printf 'mine\\n'; }; echo a; command echo b; unset -f echo", "mine\nb\n");
    run("alias List='echo no'; command List x", "[List|x]\n");
    run("command -v echo List nothere; echo $?", "echo\nC:List\n1\n");
    run("f() { :; }; alias ll='List DIRS'; command -V f echo List ll", "f is a function\necho is a shell builtin\nList is C:List\nll is aliased to `List DIRS'\n");
    run("x=$(exec List y; echo not); echo \"[$x]\" $?", "[[List|y]] 0\n");
    run("( exec false; echo not ); echo $?", "1\n");
    /* $!, wait -n, wait %n, disown (0.1b9; the fake's last job is %2) */
    run("echo $! \"$!\"; kill $!", "%2 %2\n[kill]\n");
    run("wait $!; wait -n; wait %1 %3; wait 5", "[wait %2]\n[wait -n]\n[wait %1]\n[wait %3]\n[wait|5]\n");
    run("disown; disown %1; disown -a", "[disown ]\n[disown %1]\n[disown -a]\n");
    run("v=out; f() { declare v=in; echo $v; }; f; echo $v", "in\nout\n");
    run("x='a  b'; f() { local y=$x; echo \"$y\"; }; f", "a  b\n");
    run("trap 'echo bye' EXIT; trap; trap - EXIT; trap", "trap -- 'echo bye' EXIT\n");
    run("trap 'echo err $?' ERR; false; true; trap - ERR; false", "err 1\n");
    run("f() { trap 'echo ret' RETURN; echo in; }; f", "in\nret\n");
    run("trap 'echo x' TERM HUP; trap 'echo y' NOSUCH", "csh: trap: unknown signal NOSUCH\n");
    run("set -- -a -b val -cd x; while getopts ab:cd o; do echo $o ${OPTARG:-}; done; echo $OPTIND", "a\nb val\nc\nd\n5\n");
    run("OPTIND=1; while getopts :a: o -z -a; do echo \"$o $OPTARG\"; done", "? z\n: a\n");
    runin("2\n\n9\n", "PS3='> '; select c in red green; do echo \"[$c] $REPLY\"; done", "1) red\n2) green\n> [green] 2\n> 1) red\n2) green\n> [] 9\n> \n");
    run("printf '%.2f|%8.3f|%-6.1f|%e|%g|%g|%g\\n' 3.14159 2.5 -1.25 12345.678 0.0001 100000 1e7", "3.14|   2.500|-1.2  |1.234568e+04|0.0001|100000|1e+07\n");
    run("printf '%05.1f|%+.0f|%.3g|%G\\n' 9.96 2.5 1234.5 0.000012", "010.0|+2|1.23e+03|1.2E-05\n");
    run("printf -v out '%s-%d' a 5; echo $out", "a-5\n");
    run("set -u; echo ${undefined:-ok}; echo $undefined; echo no", "ok\ncsh: unbound variable: undefined\n");
    run("set -o pipefail; set -o | Sort", "{job set -o | Sort}{sel in=1 out=51}errexit\toff\nxtrace\toff\nnounset\toff\nnoglob\toff\npipefail\ton\nhistexpand\ton\n{/sel}{close 51}[last Sort| in=151]{end rc=0 own}\n");
    run("set -euo pipefail; echo $-", "eu\n");
    run("set -f; echo *.txt; List *.txt; set +f; echo *.txt", "*.txt\n[List|#?.txt]\na.txt b.txt\n");
    run("echo *.txt *.none \"*.txt\" '*'", "a.txt b.txt *.none *.txt *\n");
    run("f() { echo $#; }; f *.txt", "2\n");
    run("echo a?txt [ab].txt; Version ?", "a.txt a.txt b.txt\n[Version|?]\n");
    run("x=1; (x=2; echo in $x); echo out $x", "in 2\nout 1\n");
    run("x=1; y=$(x=5; echo $x); echo $x $y", "1 5\n");
    run("(f() { echo inner; }; f); f 2>&1; echo rc $?", "inner\n[f| 2>&1]\nrc 0\n");
    run("a=(1 2); (a+=(3); echo ${#a[@]}); echo ${#a[@]}", "3\n2\n");
    run("set -- a b; (set -- z; echo $1); echo $1", "z\na\n");
    run("cd -; echo $?", "csh: cd: OLDPWD not set\n10\n");
    run("cat <(echo hi)", "{heredoc T:hd1:hi\n}[cat|T:hd1]\n");
    run("List |& Sort", "{job List | Sort}[spawn List| in=1 out=11 2>&1][last Sort| in=10]{end rc=0 own}\n");
    run("f() { echo f; }; f | Sort", "{job f | Sort}{heredoc T:hd1:#!csh\nf () { echo f; }\nset --\nf \n}[spawn csh|FROM T:hd1 in=1 out=11][last Sort| in=10]{end rc=0 own}\n");
    run("echo bg &", "{job & echo bg}{heredoc T:hd1:#!csh\nset --\necho bg \n}[spawn csh|FROM T:hd1 in=1 out=2]{end rc=0}\n");
    run("List | Sort; echo ${PIPESTATUS[@]}", "{job List | Sort}[spawn List| in=1 out=11][last Sort| in=10]{end rc=0 own}\n0 0\n");
    runin("l1\nl2\n", "mapfile -t L; echo ${#L[@]} ${L[1]}", "2 l2\n");
    runin("w1 w2  w3\n", "read -a W; echo ${#W[@]} ${W[2]}", "3 w3\n");
    run("echo $((RANDOM >= 0 && RANDOM < 32768))", "1\n");
    run("unset -f nosuch; f() { echo f; }; unset -f f; f", "[f|]\n");
    bang("!!\n", "Copy a b\n");
    bang("echo !$ !^ !*\n", "echo b a a b\n");
    bang("!-2 x\n", "echo one two three x\n");
    bang("!1\n", "List Work: ALL\n");
    bang("!ec | Sort\n", "echo one two three | Sort\n");
    bang("^a^z^\n", "Copy z b\n");
    bang("^Copy^Move\n", "Move a b\n");
    bang("[ a != b ] && ! true; echo '!!' ${!x} \\!!\n", "{same}");
    bang("!nosuch\n", "{err}csh: event not found: !nosuch\n");
    check("[[ a == b", 0);
    check("(( 1 +", 0);
    check("a=(1 2", 0);
    check("for ((i=0;i<3;i++)); do echo; done", 1);
    printf("%d of %d language checks passed\n", ran - bad, ran);
    return bad != 0;
}
