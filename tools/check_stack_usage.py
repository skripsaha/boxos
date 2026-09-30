#!/usr/bin/env python3
"""
tools/check_stack_usage.py — the stack guard for TagFS and the storage deck.

A block buffer belongs to the job that moves the block, or to its OpContext.
It does not belong to the kernel stack. An AP K-Core and every process run on
CONFIG_KERNEL_STACK_PAGES pages — 16 KiB (kernel_config.h) — and every device
interrupt lands on that same stack, because vectors 32..255 have no IST
(idt.c). A write that nests three 4 KiB frames before it reaches the medium
has spent most of that stack before the medium's own driver has run.

GCC measures every frame and records every direct call it emitted:
-fcallgraph-info=su writes one .ci beside each object, and changes no byte of
code. This tool reads those records for exactly the objects the Makefile
names and asks two questions.

  FRAMES. No function compiled into TagFS or the storage deck stands on more
  than LIMIT bytes, except the ones the held file names, each with the step of
  the Quill plan that removes it. A held frame may not grow; one that has
  fallen to LIMIT or under must leave the file.

  REACH. The heaviest chain of frames from a storage request to the medium
  may not exceed BUDGET. A reach over it is held by one line of the held
  file, with the step that brings it under; since step 1.10 there is none.

A held line is a Caveat in the sense the plan approved: while the defect is
there the build says "caveat held" and goes on; once it is gone, the line is a
failure until it is removed.

WHICH BUILD IS JUDGED ON WHAT. The held numbers are the default build's.

  In every build: a frame over LIMIT that nothing holds, a held file that
  does not read, an object with no call record, a medium the graph does not
  have, and — once the reach line is gone — a reach over BUDGET.

  Only on the build the numbers were measured on (all build keys at their
  defaults, and not HARNESS=on): a held frame that grew, one that fell to
  LIMIT or under or stopped being a function, and a reach that grew or came
  within BUDGET.

A build key compiles another kernel — DEBUG=on grows five held frames by 16 to
32 bytes — and its marker makes the next default build recompile every
object, which is then judged in full. HARNESS=on changes no object: it builds
a tree mutated on purpose, whose frames may move a few bytes for the mutant's
sake, and a scenario that cannot link over that fails for the wrong reason.
So under the harness what the held numbers would refuse is SAID, not judged,
and the stamp is not written: the first build without the harness judges the
same objects in full.

Usage: check_stack_usage.py --build DIR --held FILE --keys "KEYS" [--harness]
                            --stamp FILE OBJ...
"""
import sys

sys.dont_write_bytecode = True

import argparse
import os
import re

# A 4 KiB block buffer cannot hide under this, and neither can two sectors.
LIMIT = 1024

# What the storage path may spend before its bytes reach the medium.
#   16384  the K-Core and process kernel stack (4 pages, kernel_config.h)
#  -  608  the dispatch in front of a handler (syscall_handler ->
#          sync_syscall_dispatch -> guide -> guide_process_pocket ->
#          ManifestExecuteOnce; kcore_run_loop's is 592). No recursion: exact.
#  - 5664  the deepest the medium's own drivers go below it
#          (BoardroomWrite -> xhci_msd_write -> ...)
#  - 5016  the deepest interrupt that lands on the same stack
#          (IrqTheTickAnswers)
#  = 5096, of which 4096 is spent and the rest is margin.
# Measured with GCC 15.2.0 at e7aa9345. The two driver terms walk each
# recursion once and stop where it would repeat — the heaviest of 23 walk
# orders, because that walk is order-dependent (3928..5664 below the medium).
# Counted the way the reach counts one, every frame of a recursion at once,
# they are 8608 and 7808 and no split of 16 KiB fits at all: one recursion of
# fifty kernel functions (Touch, the manifests, the heap, the page tables)
# weighs 5840 B by itself. The terms move only with a reason written here.
BUDGET = 4096

# Where a request's bytes leave the filesystem for the medium.
MEDIUM = ("BoardroomRead", "BoardroomWrite", "BoardroomReadAsync",
          "BoardroomFlush", "ahci_submit_read_async", "ahci_submit_write_async")

# Objects whose every frame is judged, and the sources whose functions nothing
# calls directly — the handlers the OpRegistry dispatches and the callbacks a
# medium answers — which are where a request begins.
FRAME_SCOPE = ("kernel/tagfs/", "kernel/core/decks/storage/")
ROOT_SCOPE = "src/kernel/core/decks/storage/"

CLONE = re.compile(r"\.(part|constprop|isra|cold)(\.\d+)*")
NODE = re.compile(r'^node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE = re.compile(r'^edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')


class Verdict:
    """What this run refuses, and what it only says.

    exact    — the build the held numbers were measured on: every rule judged.
    harness  — HARNESS=on at default keys: a refusal that rests on the held
               numbers is said and not made, and no stamp is written.
    Neither  — a build key: rules that rest on the held numbers are not asked,
               because the numbers are not this kernel's."""

    def __init__(self, keys, harness):
        self.keyed = keys != "all defaults"
        self.harness = harness
        self.exact = not self.keyed and not harness
        self.failures = 0
        self.unjudged = 0

    def fail(self, line):
        self.failures += 1
        print("[stack] FAIL " + line)

    def against_record(self, line):
        """A refusal that rests on the held numbers. True if it was made."""
        if self.exact:
            self.fail(line)
            return True
        if self.harness and not self.keyed:
            self.unjudged += 1
            print("[stack] under the harness, said and not judged: " + line)
        return False


def read_records(build, objects, verdict):
    """frames: {source:function -> bytes} for FRAME_SCOPE objects;
    graph: size/owner/calls/callers over every object.

    A node with a size is a definition; one without is a declaration of a
    function defined in another object, or __indirect_call. An edge from a
    function to its own split part (f -> f.part.0) carries no label, so the
    label is not part of what an edge has to match."""
    frames, size, owner, calls, callers = {}, {}, {}, {}, {}
    scope = tuple(os.path.join(build, d) for d in FRAME_SCOPE)
    for obj in objects:
        ci = obj[:-2] + ".ci"
        try:
            with open(ci, errors="replace") as f:
                text = f.read().splitlines()
        except OSError:
            verdict.fail("no call record beside %s — it was compiled before "
                         "-fcallgraph-info=su was in CFLAGS: make clean && make"
                         % obj)
            continue
        judged = obj.startswith(scope)
        for line in text:
            m = NODE.match(line)
            if m:
                title, label = m.groups()
                parts = label.split("\\n")
                if len(parts) < 3:
                    continue
                b = int(parts[2].split(" ")[0])
                src = parts[1].split(":")[0]
                size[title] = max(size.get(title, 0), b)
                owner[title] = src
                if judged:
                    key = "%s:%s" % (src, CLONE.sub("", parts[0]))
                    frames[key] = max(frames.get(key, 0), b)
                continue
            m = EDGE.match(line)
            if m:
                s, t = m.groups()
                calls.setdefault(s, []).append(t)
                callers.setdefault(t, set()).add(s)
    return frames, size, owner, calls, callers


def strong_components(nodes, succ):
    """Tarjan, iteratively: the graph is the whole kernel's and a recursive
    walk of it is a Python recursion limit waiting to be met. Components come
    out successors first."""
    index, low, onstack, stack, comp = {}, {}, set(), [], {}
    comps, counter = [], 0
    for v0 in sorted(nodes):
        if v0 in index:
            continue
        index[v0] = low[v0] = counter
        counter += 1
        stack.append(v0)
        onstack.add(v0)
        work = [(v0, iter(succ(v0)))]
        while work:
            v, it = work[-1]
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    onstack.add(w)
                    work.append((w, iter(succ(w))))
                    break
                if w in onstack:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    parent = work[-1][0]
                    low[parent] = min(low[parent], low[v])
                if low[v] == index[v]:
                    members = []
                    while True:
                        w = stack.pop()
                        onstack.discard(w)
                        comp[w] = len(comps)
                        members.append(w)
                        if w == v:
                            break
                    comps.append(sorted(members))
    return comps, comp


def reach_to_medium(roots, size, calls):
    """The heaviest chain of frames from any root to the medium.

    Only functions that can reach the medium take part, and the walk stops
    AT the medium: what its drivers spend below it is BUDGET's business.
    Recursion is folded, not unrolled — every strongly connected set of
    functions counts each of its frames once, and is named, because a lap
    GCC cannot see the end of is a lap this tool will not pretend to bound.
    An indirect call (__indirect_call) and code with no frame record
    (assembly) end a chain: the reach is what GCC can prove, no more.

    Returns (bytes, path, loops): path is the chain as a list of components,
    loops the components that are recursion."""
    medium = set(m for m in MEDIUM if m in size)
    back = {}
    for s, targets in calls.items():
        if s in medium:
            continue
        for t in targets:
            back.setdefault(t, set()).add(s)
    live, todo = set(), list(medium)
    while todo:
        x = todo.pop()
        if x in live or x not in size:
            continue
        live.add(x)
        todo.extend(back.get(x, ()))

    def succ(v):
        if v in medium:
            return []
        return sorted(set(w for w in calls.get(v, ()) if w in live))

    comps, comp = strong_components(live, succ)
    best, nxt = [], []
    for c, members in enumerate(comps):
        out = set(comp[w] for v in members for w in succ(v)) - {c}
        tail = max(sorted(out), key=lambda o: best[o], default=None)
        best.append(sum(size[v] for v in members) +
                    (best[tail] if tail is not None else 0))
        nxt.append(tail)
    starts = sorted(set(comp[r] for r in roots if r in live))
    if not starts:
        return None, [], []
    c = max(starts, key=lambda s: best[s])
    total, path, loops = best[c], [], []
    while c is not None:
        members = comps[c]
        if len(members) > 1 or members[0] in calls.get(members[0], ()):
            loops.append(members)
        path.append(members)
        c = nxt[c]
    return total, path, loops


def read_held(path, verdict):
    """{key: (bytes, step, what)} and the reach line, or None."""
    held, reach = {}, None
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError as e:
        verdict.fail("the held file %s cannot be read: %s" % (path, e.strerror))
        return held, reach
    for n, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split(None, 3)
        if len(f) < 4 or not f[1].isdigit():
            verdict.fail("%s:%d: a caveat is a name, its bytes, its step and "
                         "what it holds" % (path, n))
            continue
        entry = (int(f[1]), f[2], f[3])
        if f[0] in held or (f[0] == "reach" and reach is not None):
            verdict.fail("%s:%d: %s is held twice" % (path, n, f[0]))
        elif f[0] == "reach":
            reach = entry
        else:
            held[f[0]] = entry
    return held, reach


def judge_frames(frames, held, verdict):
    """Every frame over LIMIT is held or refused; every held one is asked
    whether it is still the defect its line names. Returns the offenders."""
    over = sorted((k for k in frames if frames[k] > LIMIT),
                  key=lambda k: (-frames[k], k))
    for key in over:
        if key not in held:
            verdict.fail("new offender: %s stands on %u bytes, over %u — a "
                         "block buffer belongs to the job or its OpContext, "
                         "not to the kernel stack" % (key, frames[key], LIMIT))
    for key, (b, step, what) in sorted(held.items()):
        now = frames.get(key)
        if now is None or now <= LIMIT:
            state = ("is no longer a function of its own" if now is None
                     else "is %u bytes, no longer over %u" % (now, LIMIT))
            verdict.against_record(
                "caveat lifted early: stack %s — %s — %s: the test does not "
                "test what it claims, or step %s already landed — lift the "
                "caveat" % (key, what, state, step))
            continue
        if now > b and verdict.against_record(
                "%s grew to %u bytes while its caveat holds it at %u — a "
                "caveat names a defect, it does not license a bigger one"
                % (key, now, b)):
            continue
        print("caveat held: stack %s %u B — %s — discharged by step %s"
              % (key, now, what, step))
        if verdict.exact and now < b:
            print("[stack] %s is %u bytes, held at %u — write %u into its "
                  "line" % (key, now, b, now))
    return over


def said(members, size):
    names = " ".join(m.split(":")[-1] for m in members)
    if len(members) == 1:
        return "%s %u" % (names, size[members[0]])
    return "{%s} %u" % (names, sum(size[m] for m in members))


def judge_reach(size, owner, calls, callers, reach_held, verdict):
    """The heaviest chain from a storage request to the medium, against the
    reach line while there is one and against BUDGET once there is not.
    Returns the reach, or None when it could not be measured."""
    for m in MEDIUM:
        if m not in size:
            verdict.fail("%s is not in the call graph — the reach cannot be "
                         "measured" % m)
    roots = [t for t in size
             if owner[t].startswith(ROOT_SCOPE) and not callers.get(t)]
    reach, path, loops = reach_to_medium(roots, size, calls)
    if reach is None:
        verdict.fail("no storage request reaches the medium — the call graph "
                     "is not the one this tool was written against")
        return None
    chain = " -> ".join(said(m, size) for m in path)
    for members in loops:
        print("[stack] recursion on the way to the medium, each frame counted "
              "once: %s" % " ".join(m.split(":")[-1] for m in members))
    if reach_held is None:
        if reach > BUDGET:
            verdict.fail("reach %u bytes, over its budget of %u: %s"
                         % (reach, BUDGET, chain))
    else:
        b, step, what = reach_held
        if reach <= BUDGET:
            verdict.against_record(
                "caveat lifted early: stack reach — %s — is %u bytes, within "
                "%u: the test does not test what it claims, or step %s "
                "already landed — lift the caveat" % (what, reach, BUDGET, step))
        elif reach > b and verdict.against_record(
                "reach grew to %u bytes while its caveat holds it at %u: %s"
                % (reach, b, chain)):
            pass
        else:
            print("caveat held: stack reach %u B > %u — %s — discharged by "
                  "step %s" % (reach, BUDGET, what, step))
            if verdict.exact and reach < b:
                print("[stack] the reach is %u bytes, held at %u — write %u "
                      "into its line" % (reach, b, reach))
    print("[stack]   %s" % chain)
    return reach


def main(argv):
    ap = argparse.ArgumentParser(
        description="The stack guard for TagFS and the storage deck.")
    ap.add_argument("--build", required=True)
    ap.add_argument("--held", required=True)
    ap.add_argument("--keys", default="all defaults")
    ap.add_argument("--harness", action="store_true")
    ap.add_argument("--stamp", required=True)
    ap.add_argument("objects", nargs="+")
    args = ap.parse_args(argv[1:])

    # The stamp says these objects were judged and passed. Until this run
    # says so again, they were not.
    try:
        os.remove(args.stamp)
    except FileNotFoundError:
        pass

    verdict = Verdict(args.keys, args.harness)
    if verdict.keyed:
        print("[stack] build keys '%s' compile another kernel; the held numbers "
              "are the default build's, so here only new offenders and the "
              "budget are judged" % args.keys)

    frames, size, owner, calls, callers = read_records(
        args.build, args.objects, verdict)
    held, reach_held = read_held(args.held, verdict)
    over = judge_frames(frames, held, verdict)
    reach = judge_reach(size, owner, calls, callers, reach_held, verdict)
    if reach is not None:
        print("[stack] %u frame(s) over %u B in TagFS and the storage deck, "
              "%u held; reach %u B against a budget of %u B"
              % (len(over), LIMIT, len(held), reach, BUDGET))

    if verdict.failures:
        return 1
    if verdict.unjudged:
        print("[stack] %u judgment(s) against the held numbers were said and "
              "not made under the harness; no stamp, so the next build "
              "without HARNESS=on makes them" % verdict.unjudged)
        return 0
    with open(args.stamp, "w"):
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
