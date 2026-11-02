# stand_preflight — the second and third of stand.sh's three preflights: the
# tree asked whether it still carries a mutation a harness put there, and
# whether it still carries every anchor a harness's mutations expect to find.
#
# SOURCED, NEVER RUN — by stand.sh, where the first preflight and the reasons
# for all three are; "$0" is still the harness that sourced stand.sh. Its own
# file so stand.sh stays under a thousand lines.

# tools/ is asked as well as src/: an oracle that proves a host tool edits
# that tool exactly as one that proves the kernel edits the kernel, and an
# edit left behind there is a tool that judges every later run with its
# defect put back. Ten files in tools/ carry the two phrases as TEXT and are
# passed over by name: the files the harnesses write them from into their
# mutations (logcheck.sh, logcheck_book_logger.sh, logcheck_autostart.sh,
# logcheck_room.sh, logcheck_halthog.sh, bindery.sh, powercut_mut.sh —
# powercut.sh keeps none of its own), stand.sh,
# whose mutate and restore insist on them, this
# file, which asks for them, and the plate that reads them. A harness added
# to tools/ is added to this list, and until it is this refuses to run and
# names it — the safe way to be wrong. -I because every mutation is an edit
# to a text source, and the one binary that carries the words is the plate's
# own compiled search for them.
MUT_LEFTOVER=$( { grep -rl -e "logcheck mutation" -e "logcheck injection" src
                  grep -rlI -e "logcheck mutation" -e "logcheck injection" \
                       --exclude=logcheck.sh --exclude=logcheck_book_logger.sh --exclude=logcheck_autostart.sh --exclude=logcheck_room.sh --exclude=logcheck_halthog.sh --exclude=bindery.sh --exclude=powercut_mut.sh \
                       --exclude=stand.sh --exclude=stand_preflight.sh --exclude=buildplate.c tools
                } 2>/dev/null || true)
if [ -n "$MUT_LEFTOVER" ]; then
    echo "$STAND_NAME: REFUSING TO RUN — the source tree still carries a mutation:"
    printf '          %s\n' $MUT_LEFTOVER
    echo "          Nothing measured on this tree would mean anything. Put these"
    echo "          files back (git checkout, or the pristine copies under"
    echo "          $MUT_PRISTINE) and run again."
    exit 1
fi

# Preflight 3 — every mutation anchor still names code that is there.
#
# An anchor is a literal copied out of a source file. Rename that code, as a
# perfectly legitimate change may, and the anchor is orphaned. `mutate` and
# `mut_done` do catch it — that is what they are for — but only after a build
# and a boot, and only for whichever oracle happened to run; the other thirty
# keep their stale anchors until somebody reaches them. Measured 2026-09-12:
# `BrookCleanupProcess` was renamed to `BrookCleanupStrand` and `orderlifemut`
# was the only oracle that noticed, half an hour into a suite.
#
# The anchors are right here, in the harness that sources this — "$0" is its
# path, and sourcing does not change it — and in the file of mutations it
# sources beside itself (a sibling named *_mut.sh): every string a mutation's python
# asserts it finds exactly once (`assert s.count(X) == 1`), read by python's
# own parser — bound to a name, built from others, or a row of an edit table
# — and looked for byte for byte in the file the mutation opens, or, where
# the harness hands the path in as a word, in every source under src/ and
# tools/. This used to ask grep, and grep -F splits a pattern at every
# newline: an anchor ending in one matched every file, twenty-three anchors
# could never go stale, and two of handset's had. So the check is first shown
# a stale anchor of each kind, and vouches for nothing if it does not see
# them. The harnesses are never searched: every anchor is found in the line
# that declares it.
python3 - "$0" "$STAND_NAME" <<'ANCHOR_EOF' || exit 1
import ast, os, re, sys
HARNESSES = {"logcheck.sh", "powercut.sh", "powercut_mut.sh", "bindery.sh", "stand.sh"}
SOURCES = (".c", ".h", ".cpp", ".hpp", ".S", ".asm", ".ld", ".py", ".sh", ".txt", ".mk")
CACHE = {}

def Read(path):
    if path not in CACHE:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                CACHE[path] = fh.read()
        except OSError:
            CACHE[path] = ""
    return CACHE[path]

def Corpus():
    for top in ("src", "tools"):
        for d, _dirs, files in os.walk(top):
            for f in sorted(files):
                if f not in HARNESSES and (f.endswith(SOURCES) or f == "Makefile"):
                    yield os.path.join(d, f)

def One(n, env):
    """A string the python would compute, or None."""
    if isinstance(n, ast.Constant) and isinstance(n.value, str):
        return n.value
    if isinstance(n, ast.Name):
        return env.get(n.id) if isinstance(env.get(n.id), str) else None
    if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Add):
        a, b = One(n.left, env), One(n.right, env)
        return a + b if a is not None and b is not None else None
    return None

def Rows(n, env):
    """The rows of an edit table: a list or tuple of tuples, or every value
    of a dict the table is picked out of."""
    if isinstance(n, ast.Name):
        return env.get(n.id) if isinstance(env.get(n.id), list) else []
    if isinstance(n, ast.Subscript) and isinstance(n.value, ast.Dict):
        return [r for v in n.value.values for r in Rows(v, env)]
    if isinstance(n, (ast.Tuple, ast.List)):
        return [tuple(One(e, env) for e in x.elts) if isinstance(x, (ast.Tuple, ast.List))
                else One(x, env) for x in n.elts]
    return []

def Walk(body, env, out):
    for st in body:
        if isinstance(st, ast.Assign) and len(st.targets) == 1 and isinstance(st.targets[0], ast.Name):
            v, name = st.value, st.targets[0].id
            if (isinstance(v, ast.Call) and isinstance(v.func, ast.Attribute) and v.func.attr == "read"
                    and isinstance(v.func.value, ast.Call) and getattr(v.func.value.func, "id", "") == "open"):
                env["<opened>" + name] = One(v.func.value.args[0], env)
            else:
                env[name] = Rows(v, env) or One(v, env)
        elif isinstance(st, ast.For):
            names = [t.id for t in (st.target.elts if isinstance(st.target, ast.Tuple) else [st.target])]
            for row in Rows(st.iter, env):
                Walk(st.body, dict(env, **dict(zip(names, row if isinstance(row, tuple) else (row,)))), out)
        elif isinstance(st, ast.Assert):
            for n in ast.walk(st.test):
                if isinstance(n, ast.Call) and getattr(n.func, "attr", "") == "count" and len(n.args) == 1:
                    text = One(n.args[0], env)
                    if text is not None and len(text.strip()) >= 6:
                        out.append((text, env.get("<opened>" + getattr(n.func.value, "id", ""))))
        elif isinstance(st, (ast.If, ast.With, ast.While, ast.Try)):
            Walk(getattr(st, "body", []) + getattr(st, "orelse", []), env, out)

def Stale(script):
    found = []
    for _tag, body in re.findall(r"<<'(\w+)'[^\n]*\n(.*?)\n\1\n", script, re.S):
        try:
            Walk(ast.parse(body).body, {}, found)
        except SyntaxError:
            continue
    stale = []
    for text, path in sorted(set(found), key=lambda x: (x[0], x[1] or "")):
        where = [path] if path and path.startswith(("src/", "tools/")) else list(Corpus())
        if not any(text in Read(f) for f in where):
            stale.append((text, path or "src/ and tools/"))
    return stale

probe = ("x <<'EOF'\np = \"src/kernel/tagfs/tagfs.c\"\ns = open(p).read()\n"
         "a = \"zz no such line, ending in a newline\\n\"\nassert s.count(a) == 1\n"
         "for a, b in ((\"zz no such line, in an edit table\", \"b\"),):\n    assert s.count(a) == 1\nEOF\n")
if len(Stale(probe)) != 2:
    print("%s: REFUSING TO RUN — preflight 3 did not see the two stale anchors it is shown "
          "first, so its silence about the harness's would mean nothing" % sys.argv[2])
    sys.exit(1)
harness = open(sys.argv[1], encoding="utf-8").read()
for sib in re.findall(r'^\. "\$\(dirname "\$0"\)/(\w+_mut\.sh)"', harness, re.M):
    harness += "\n" + open(os.path.join(os.path.dirname(sys.argv[1]), sib), encoding="utf-8").read()
stale = Stale(harness)
if stale:
    print("%s: REFUSING TO RUN — %d mutation anchor(s) name code that is"
          % (sys.argv[2], len(stale)))
    print("          no longer in the tree. The oracle that uses one cannot put")
    print("          its defect back, so its green half proves nothing. Fix the")
    print("          anchor; do not skip the oracle.")
    for text, where in stale:
        print("          stale in %s: %r" % (where, text[:100]))
    sys.exit(1)
ANCHOR_EOF
