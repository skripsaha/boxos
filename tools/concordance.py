#!/usr/bin/env python3
"""
tools/concordance.py — Concordance: a startup test written for format v1 names
the test that holds a format-v2 book to the same property.

The stand runs on a book as it runs on format v1 (spec_v2 §16.5 п.22): a test
written for neither format runs on both. A test that holds format v1's own
records — the Ledger, the chains, the pool, the Spares — is written for format
v1 (`.format = DEED_FORMAT_V1`) and stands aside on a book. Standing aside is
how a property quietly stops being held on the format the machine now makes,
so each such test names, in `.analog`, the test that holds a book to what it
holds format v1 to — and this check holds the name to the tree:

  NAMED.   Every test written for format v1 names its analog.
  WALKED.  The analog is a test the runner walks: a TestCase of an array that a
           suite in TEST_SUITES (tests.h) holds — a test written and never
           walked holds nothing.
  ON A BOOK. The analog runs on a book: written for format v2, or for either.
  ONE NAME. No two tests the runner walks share a name, and a test that runs on
           a book names no analog — on a book it is its own.

A note, not a fault: a test written for format v1 in a suite every build walks
whose analog only a VOLUME_TESTS=on build walks — the startup tests of a plain
build hold a book to that property nowhere; the stand's gates (stand, standusb)
build with VOLUME_TESTS=on and hold it there. Said, by name.

    tools/concordance.py              the tree this file is in
    tools/concordance.py --self-test  each rule shown a tree that breaks it

Pure source: no build, no machine. Exit 0 when every rule holds, 1 otherwise.
"""
import os
import re
import sys
import tempfile

TESTS_DIR = os.path.join("src", "kernel", "tagfs", "tests")

SUITE_LIST = re.compile(r"#define\s+TEST_SUITES\(SUITE\)(.*?)/\*\s*the walk ends here", re.S)
SUITE_NAME = re.compile(r"SUITE\((\w+)\)")
SUITE_DEF = re.compile(r"const\s+TestSuite\s+g_(\w+)_suite\s*=\s*\{(.*?)\};", re.S)
ARRAY_DEF = re.compile(r"TestCase\s+(\w+)\s*\[\s*\]\s*=\s*\{", re.S)
CASE = re.compile(r"\{\s*\.name\s*=\s*\"([^\"]+)\"(.*?)\}", re.S)
FORMAT = re.compile(r"\.format\s*=\s*DEED_FORMAT_(V1|V2|NONE)")
ANALOG = re.compile(r"\.analog\s*=\s*\"([^\"]*)\"")


def ArrayBody(text, start):
    """The text of an initializer list from its opening brace to its close."""
    depth, i = 0, start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    return text[start:]


def Cases(root):
    """Every TestCase the runner walks: name → (file:line, format, analog).

    Returns (cases, faults): a name walked twice is a fault, and so is a suite
    TEST_SUITES lists that no file defines."""
    tdir = os.path.join(root, TESTS_DIR)
    header = open(os.path.join(tdir, "tests.h")).read()
    listed = SUITE_LIST.search(header)
    if not listed:
        return {}, ["tests.h: TEST_SUITES and the line that ends its walk are not where this check reads them"]
    walked = SUITE_NAME.findall(listed.group(1))

    arrays, suites = {}, {}
    for fname in sorted(os.listdir(tdir)):
        if not fname.endswith(".c"):
            continue
        path = os.path.join(tdir, fname)
        text = open(path).read()
        for m in SUITE_DEF.finditer(text):
            ref = re.search(r"\b(g_\w+)\b", m.group(2))
            if ref:
                suites[m.group(1)] = ref.group(1)
        for m in ARRAY_DEF.finditer(text):
            brace = text.index("{", m.end() - 1)
            arrays[m.group(1)] = (path, text, brace, ArrayBody(text, brace))

    cases, faults = {}, []
    for suite in walked:
        if suite not in suites or suites[suite] not in arrays:
            faults.append(f"suite {suite}: TEST_SUITES walks it and no file under {TESTS_DIR} defines it")
            continue
        path, text, brace, body = arrays[suites[suite]]
        for c in CASE.finditer(body):
            name, rest = c.group(1), c.group(2)
            line = text.count("\n", 0, brace + c.start()) + 1
            where = f"{os.path.relpath(path, root)}:{line}"
            fmt = FORMAT.search(rest)
            analog = ANALOG.search(rest)
            if name in cases:
                faults.append(f"{where}: {name} — walked twice, first at {cases[name][0]}: an analog that names it names two tests")
                continue
            cases[name] = (where, fmt.group(1) if fmt else "NONE", analog.group(1) if analog else None)
    return cases, faults


def EveryBuild(root):
    """The tests a build of any key walks: the suites TEST_SUITES lists bare,
    not inside VOLUME_TESTS_ONLY(...)."""
    tdir = os.path.join(root, TESTS_DIR)
    listed = SUITE_LIST.search(open(os.path.join(tdir, "tests.h")).read())
    bare = set(re.findall(r"^\s*SUITE\((\w+)\)", listed.group(1), re.M)) if listed else set()
    arrays, suites = {}, {}
    for fname in sorted(os.listdir(tdir)):
        if not fname.endswith(".c"):
            continue
        text = open(os.path.join(tdir, fname)).read()
        for m in SUITE_DEF.finditer(text):
            ref = re.search(r"\b(g_\w+)\b", m.group(2))
            if ref:
                suites[m.group(1)] = ref.group(1)
        for m in ARRAY_DEF.finditer(text):
            arrays[m.group(1)] = ArrayBody(text, text.index("{", m.end() - 1))
    return {c.group(1) for s in bare if suites.get(s) in arrays for c in CASE.finditer(arrays[suites[s]])}


def Notes(root):
    """Tests written for format v1 that every build walks, whose analog only a
    VOLUME_TESTS=on build walks."""
    cases, _ = Cases(root)
    every = EveryBuild(root)
    return [f"{name} — walked by every build, its analog {analog} by a VOLUME_TESTS=on build alone"
            for name, (where, fmt, analog) in sorted(cases.items())
            if fmt == "V1" and name in every and analog in cases and analog not in every]


def Judge(root):
    """The faults of the tree at ROOT, and how many tests are written for v1."""
    cases, faults = Cases(root)
    v1 = 0
    for name, (where, fmt, analog) in cases.items():
        if fmt != "V1":
            if analog is not None:
                faults.append(f"{where}: {name} — runs on a book and names an analog ({analog}); only a test written for format v1 names one")
            continue
        v1 += 1
        if not analog:
            faults.append(f"{where}: {name} — written for format v1 and names no analog: what holds a book to it?")
        elif analog not in cases:
            faults.append(f"{where}: {name} — its analog {analog} is no test the runner walks")
        elif cases[analog][1] == "V1":
            faults.append(f"{where}: {name} — its analog {analog} is written for format v1 too and never runs on a book")
    return faults, v1


SELF_HEADER = """#define TEST_SUITES(SUITE) \\
    SUITE(one) \\
    /* the walk ends here */
"""


def SelfTree(dirname, cases, extra_suite=""):
    tdir = os.path.join(dirname, TESTS_DIR)
    os.makedirs(tdir)
    with open(os.path.join(tdir, "tests.h"), "w") as f:
        f.write(SELF_HEADER.replace("SUITE(one) \\", "SUITE(one) \\\n    " + extra_suite + " \\")
                if extra_suite else SELF_HEADER)
    with open(os.path.join(tdir, "one_tests.c"), "w") as f:
        f.write("static const TestCase g_one_tests[] = {\n" + cases +
                "};\nconst TestSuite g_one_suite = {\n    .tests = g_one_tests, .count = 1,\n};\n"
                "static const TestCase g_unwalked_tests[] = {\n"
                "    { .name = \"unwalked\", .func = F, .format = DEED_FORMAT_V2 },\n};\n")


SELF_CASES = [
    # (what, cases, extra suite, the words a fault must carry; None = clean)
    ("a v1 test naming a book test", '    { .name = "a", .func = F, .format = DEED_FORMAT_V1,\n'
     '      .analog = "b" },\n    { .name = "b", .func = F, .format = DEED_FORMAT_V2 },\n', "", None),
    ("a v1 test naming a test of either format", '    { .name = "a", .format = DEED_FORMAT_V1, .analog = "b" },\n'
     '    { .name = "b", .func = F },\n', "", None),
    ("NAMED: a v1 test naming nothing", '    { .name = "a", .func = F, .format = DEED_FORMAT_V1 },\n', "", "names no analog"),
    ("WALKED: an analog nobody wrote", '    { .name = "a", .format = DEED_FORMAT_V1, .analog = "zz" },\n', "", "no test the runner walks"),
    ("WALKED: an analog in an array no suite walks", '    { .name = "a", .format = DEED_FORMAT_V1, .analog = "unwalked" },\n',
     "", "no test the runner walks"),
    ("ON A BOOK: an analog written for v1", '    { .name = "a", .format = DEED_FORMAT_V1, .analog = "b" },\n'
     '    { .name = "b", .format = DEED_FORMAT_V1, .analog = "a" },\n', "", "never runs on a book"),
    ("ONE NAME: an analog on a book test", '    { .name = "a", .format = DEED_FORMAT_V2, .analog = "a" },\n', "", "only a test written for format v1"),
    ("ONE NAME: a name walked twice", '    { .name = "a", .func = F },\n    { .name = "a", .func = F },\n', "", "walked twice"),
    ("a suite listed and never defined", '    { .name = "a", .func = F },\n', "SUITE(gone)", "no file under"),
]


def SelfTest():
    failed = 0
    for what, cases, extra, words in SELF_CASES:
        with tempfile.TemporaryDirectory() as d:
            SelfTree(d, cases, extra)
            faults, _ = Judge(d)
        if words is None:
            good = not faults
        else:
            good = any(words in f for f in faults)
        print(f"  {'PASS' if good else 'FAIL'} {what}" + ("" if good else f" — said: {faults or 'nothing'}"))
        failed += not good
    print(f"concordance self-test: {len(SELF_CASES) - failed} of {len(SELF_CASES)} cases")
    return 1 if failed else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return SelfTest()
    if sys.argv[1:]:
        print(__doc__.strip().split("\n\n")[-2])
        return 2
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    faults, v1 = Judge(root)
    for f in faults:
        print(f"concordance: {f}")
    if faults:
        print(f"concordance: FAIL — {len(faults)} fault(s); {v1} test(s) written for format v1")
        return 1
    print(f"concordance: {v1} test(s) written for format v1, each naming the test that holds a book to it")
    for n in Notes(root):
        print(f"concordance: note — {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
