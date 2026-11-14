"""powercut_judge_book — the oracles PowerCut's judge holds a book's run to
beside the ones it holds every run to (powercut_judge.py): what spec_v2 §3.10
promises of a book that format v1 never did.

Not a program of its own, nor a second judge: each function here takes the
Judge (powercut_judge.py) and says its verdicts through it, one line each, on
the run that Judge reads.

    Whole       C1, C15, C3: a file of a book is the state a stitch left it in.
                Every mountable cut shows each file as one state the base or a
                flush stood behind left it — its old bytes or its new, never a
                mix of the two and never a rot — or absent where one of them
                had it absent; a file rolled back by its splice (NOT_LANDED) is
                its state before.
    Order       §3.3: the disk the run left holds every stitch behind the flush
                its own word promises (volumeread/blklog_order.py, the rule
                logcheck's book_circle holds as well).
    Lines       C21: a logger's file, in every cut, whole lines and no fewer
                than the state the flush before the cut's epoch left it with.
    SlipBlocks  C21: a logger's tail rides its stitches as a Slip — the file
                takes a block of data as its lines fill one, and once more for
                the Engross a Tally owes, never one a line.
    Dedup       §8.2: the power in the middle of the pass that lays a copy on
                its original — every cut shows the copy on its own blocks or
                on the original's, block for block, and never elsewhere.
"""

import hashlib
import json
import os
import re
import subprocess
import sys

from powercut import RecordBytes

LOG_LINE = 48                   # apps/quilltest_book.c QUILL_LOG_LINE
BLOCK_SECTORS = 8               # a 4 KiB block in 512-byte sectors
SLOT_SEQ = re.compile(r"Spine slot of seq (\d+)")
LAID_IN = re.compile(r"\[TagFS\] Tally \d+ is written over slot \d+: .*\(laid in\)")   # settle/binder_done.c
ROLLED_BACK = re.compile(r"fid (\d+)'s data in stitch \d+ did not land")                  # bookmount.py _Verify


def Mountable(run):
    """The judged cuts the kernel would mount that say what files they show."""
    return [c["id"] for c in run.canon if run.Judged(c["id"]) and run.results[c["id"]]["mounts"]
            and "files" in run.results[c["id"]]]


def View(r):
    return {f[0]: (f[1], f[2], f[3]) for f in r["files"]}


def States(j):
    """Every state the base or a flush stood behind left each file in — fid ->
    the set of (name, size, sha256), None for absent — and how many states."""
    run = j.run
    ends = {run.Canon("e%d-all" % e["epoch"]) for e in run.listing["epochs"] if e["flush_seq"]} | {run.final}
    views = [{f["fid"]: (f.get("name"), f.get("size"), f.get("sha256")) for f in j.base["files"] if f["visible"]}]
    views += [View(run.results[c]) for c in sorted(x for x in ends if x) if c in set(Mountable(run))]
    states = {}
    for fid in set().union(*views):
        states[fid] = {v.get(fid) for v in views}
    return states, len(views)


def Whole(j):
    states, n = States(j)
    cuts, bad = Mountable(j.run), []
    for cid in cuts:
        view = View(j.run.results[cid])
        for fid in sorted(set(view) | set(states)):
            if view.get(fid) not in states.get(fid, {None}):
                got = view.get(fid)
                bad.append("%s (%s): fid %d %s" % (cid, j.run.Kept(cid), fid, "absent" if got is None else
                                                   "%s, %s bytes, sha256 %s" % (got[0], got[1], (got[2] or "none")[:12])))
    j.Out("files:whole", "fail" if bad else "behaves", "%d place(s) in %d mountable cuts show a file as neither the "
          "base nor any flush of the run left it, e.g. %s" % (len(bad), len(cuts), "; ".join(bad[:2])) if bad else
          "every one of %d mountable cuts shows each of its files as one of %d states the base and the flushes "
          "of the run left it in" % (len(cuts), n))


def Order(j):
    from volumeread.blklog_order import Order as Rule
    img, record = (os.path.join(j.run.dir, x) for x in ("run.img", "run.log"))
    if not os.path.exists(img):
        return j.Out("order", "harness", "the disk the run left is not in %s" % j.run.dir)
    said = Rule(img, record, ended=False)
    wrong = [text for held, text in said if not held]
    j.Out("order", "fail" if wrong else "behaves", "the disk the run left breaks §3.3: %s" % "; ".join(wrong)
          if wrong else "; ".join(text for _held, text in said))


def LineCount(view, name, most):
    """How many whole lines of quilltest log the file NAME of a view holds:
    0 when there is none, None when it holds other bytes."""
    got = next((v for v in view.values() if v[0] == name), None)
    if got is None:
        return 0
    size, sha = got[1], got[2]
    if size is None or size % LOG_LINE or size > most * LOG_LINE:
        return None
    return size // LOG_LINE if hashlib.sha256(RecordBytes("log", size // LOG_LINE)).hexdigest() == sha else None


def Lines(j, name, most):
    run, bad, cuts = j.run, [], Mountable(j.run)
    for cid in cuts:
        n = LineCount(View(run.results[cid]), name, most)
        k = run.cut[cid]["epoch"]
        before = run.Canon("e%d-all" % (k - 1)) if k else None
        floor = LineCount(View(run.results[before]), name, most) if before in set(cuts) else 0
        if n is None or (floor is not None and n < floor):
            bad.append("%s (%s): %s" % (cid, run.Kept(cid), "not whole lines" if n is None else
                                        "%d line(s), and the flush before its epoch left %d" % (n, floor)))
    j.Out("lines:" + name, "fail" if bad else "behaves", "%d of %d mountable cuts show %s otherwise than whole lines "
          "no fewer than the flush before them left: %s" % (len(bad), len(cuts), name, "; ".join(bad[:2])) if bad
          else "every one of %d mountable cuts shows %s as whole lines, as many as the flush before it left or more"
          % (len(cuts), name))


def SlipBlocks(j, fid, lines):
    """The logger's file took, over the whole record, no more blocks of data
    than the blocks its lines fill, the tail each line that crosses a block's
    end begins in the next — a write that does not lie wholly in the file's
    last block lays blocks, the one it fills and the one it begins
    (plot_slip.c PlotSlipBegin), where a line that ends on a block's end lays
    that block alone and the next begins a Slip over a hole — two for every
    Tally the record laid (the Engross of its tail, and the block that tail
    goes on in), and one for the run's end. Data no stitch of the record
    names (`free`) is counted against it too: a writer of a block a line names
    nothing until the next line."""
    sectors, tallies = 0, set()
    for w in j.run.listing["writes"]:
        for t in w["touches"]:
            if (t["kind"] == "file" and t["what"].startswith("fid %d data" % fid)) or t["kind"] == "free":
                sectors += t["sectors"]
            hit = SLOT_SEQ.match(t["what"]) if t["kind"] == "slot" else None
            if hit:
                tallies.add(int(hit.group(1)))
    block = BLOCK_SECTORS * 512
    blocks, full = (sectors + BLOCK_SECTORS - 1) // BLOCK_SECTORS, lines * LOG_LINE // block
    crossed = sum(1 for k in range(1, full + 1) if k * block % LOG_LINE)
    most = full + crossed + 2 * len(tallies) + 1
    j.Out("slip:blocks", "fail" if blocks > most else "behaves", "the logger's file (fid %d) took %d block(s) of "
          "data over %d line(s) and %d Tally(ies) — %s %d: its %d full block(s), the %d tail(s) a line crossing a "
          "block's end began, two a Tally and one for the end"
          % (fid, blocks, lines, len(tallies), "more than" if blocks > most else "within", most, full, crossed))


def Logger(j, arg):
    """`--verify logger:NAME:LINES:STEP` — the logger STEP said its file NAME
    and its fid, and wrote LINES."""
    name, lines, step_name = arg.split(":")
    step = j.run.Step(step_name)
    m = re.search(r"\(fid (\d+)\)", step["line"]) if step else None
    Lines(j, name, int(lines))
    if not m:
        return j.Out("slip:blocks", "fail", "the logger's step %s said no fid" % step_name)
    SlipBlocks(j, int(m.group(1)), int(lines))


# ---------------------------------------------------------------------------
# The book's scenarios' own questions (powercut_book.sh), as --verify kinds
# ---------------------------------------------------------------------------


def NotLanded(run):
    """The judged cuts of the run whose census rolls a file back because data
    a stitch named did not land (C3), in plan order."""
    return [c["id"] for c in run.canon if run.Judged(c["id"]) and run.results[c["id"]]["mounts"]
            and any(f[0] == "NOT_LANDED" for f in run.results[c["id"]]["findings"])]


def FirstNotLanded(d):
    """For powercut_book.sh: the first cut of the run in DIR that is C3, as
    the boot list gives a cut — id, epoch, keep, tear — or nothing."""
    from powercut import Run
    run = Run(d)
    for cid in NotLanded(run)[:1]:
        c = run.cut[cid]
        print("\t".join((cid, str(c["epoch"]), c["keep"] or "none", "%d:0x%x" % tuple(c["tear"]) if c["tear"]
                         else "-", "the first cut whose stitch landed and some of its data did not")))


def Spared(j):
    """C3: the machine stopped before the last flush of a stitch that spared
    the flush of its data, so cuts of its last epoch keep the stitch and lose
    data it names — and the census of each rolls those files back (Whole holds
    them to it); the state QEMU left, all of it landed, rolls nothing back."""
    run = j.run
    last = run.listing["epochs"][-1]["epoch"]
    mine = [cid for cid in NotLanded(run) if run.cut[cid]["epoch"] == last]
    final = run.results.get(run.final or "", {})
    back = [f for f in final.get("findings", []) if f[0] == "NOT_LANDED"]
    j.Out("spared", "fail" if not mine or back else "behaves", "%d cut(s) of the last epoch keep the stitch whose "
          "toll spared its data's flush and lose data it names, first %s; the state the machine left %s"
          % (len(mine), mine[0] if mine else "none", "rolls back too: " + back[0][2] if back else "rolls nothing back"))


def Log(path):
    return open(path, errors="replace").read().replace("\r", "") if os.path.exists(path) else ""


def Bound(img):
    """volume-read bound of IMG: its exit and its lines."""
    from powercut import VOLUME_READ
    r = subprocess.run([sys.executable, VOLUME_READ, img, "bound"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    return r.returncode, r.stdout.strip().splitlines()


def Tipin(j):
    """C7–C10: the logger's run laid Tallies (two at least), one of them laid
    in among the nodes of the one before as the kernel says — a whole Tally
    every time, which a small book may well cost less as (§16.5 п.16), is no
    TipIn asked — every cut inside their nodes and slots judged; the newest
    one held to the rule of binding where the run left it (volume-read
    bound)."""
    slots = {int(m.group(1)) for w in j.run.listing["writes"] for t in w["touches"] if t["kind"] == "slot"
             for m in [SLOT_SEQ.match(t["what"])] if m}
    laid = [x for x in Log(os.path.join(j.run.dir, "run.serial.log")).splitlines() if LAID_IN.search(x)]
    rc, said = Bound(os.path.join(j.run.dir, "run.img"))
    j.Out("tipin", "behaves" if len(slots) >= 2 and laid and rc == 0 else "fail", "the run laid %d Tally slot(s) "
          "(two are due), %d of them laid in among the nodes of the one before (one is due), and volume-read "
          "bound says of the last: %s" % (len(slots), len(laid), "; ".join(said[-2:])))


def Engross(j):
    """C22: the machine stopped before the stitch that names the block an
    Engross laid, so the record's last write of data is that block, and no
    stitch of the record names it: it lies free, and the file is as before
    (Whole, Lines)."""
    data = [(w["seq"], t["kind"]) for w in j.run.listing["writes"] for t in w["touches"]
            if t["kind"] in ("file", "free")]
    held = bool(data) and data[-1][1] == "free"
    j.Out("engross", "behaves" if held else "fail", "the record's last write of data, entry %s, %s" % (
        data[-1][0] if data else "none", "lies free: the Engross's block, named by no stitch" if held else
        "is %s — not the Engross's block left to nobody" % (data[-1][1] if data else "absent")))


def TwoBoots(run, mid, r1, shell=True):
    """The chain of two boots a medium owes (powercut_book.sh pc_chain), as
    its logs and the census between them show it: (r2, what went otherwise).
    Neither boot panics, and each reaches a shell where its kernel is not one
    a SETTLECUT point stops (SHELL); the first mount is the one r1 predicts;
    what the first boot left (ID.after.check.json) holds no block of two
    owners; and the second mount is the one its census r2 predicts. r2 is
    None when what the first left was not judged."""
    from powercut import Compact
    from powercut_judge import Reconcile
    d = os.path.join(run.dir, "boots")
    logs = [Log(os.path.join(d, mid + x)) for x in (".serial.log", ".again.serial.log")]
    dead = [n for n, log in zip(("first", "second"), logs) if "KERNEL PANIC" in log or (shell and "BoxOS Hub" not in log)]
    if dead:
        return None, ["the %s boot panicked or never reached a shell" % " and ".join(dead)]
    wrong = ["first boot: " + w for w in Reconcile(r1, logs[0])]
    r2 = Compact(mid, 0, Log(os.path.join(d, mid + ".after.check.json")), "", {}, set())
    if "error" in r2:
        return None, wrong + ["what the first boot left could not be judged: %s" % r2["error"]]
    wrong += ["the session left two owners of a block: " + f[2] for f in r2["findings"] if f[0] == "DOUBLE_OWNER"]
    return r2, wrong + ["second boot: " + w for w in Reconcile(r2, logs[1])]


def Chain(j, variant):
    """C16: the first cut of the run whose stitch landed and some of its data
    did not, booted, its session run and its power button pressed, and booted
    again (powercut_book.sh pc_chain) — both boots as TwoBoots holds them.
    `laid`: the verdict settles and the session goes on past it — each file it
    rolled back as it was rolled back and wearing settle:undone, nothing rolled
    back again, no block of two owners. `cut`: the machine stopped before the
    verdict's stitch — the census of what it left gives the same verdict again
    and lets go of no block."""
    path = os.path.join(j.run.dir, "chain.txt")
    rows = [r.split("\t") for r in (open(path).read().splitlines() if os.path.exists(path) else [])]
    if not rows:
        return j.Out("chain", "fail", "no cut of the run kept a stitch and lost data it names, so no verdict was "
                     "laid down and the two sessions were never asked")
    cid = rows[0][0]
    r1 = j.run.results[cid]
    r2, wrong = TwoBoots(j.run, cid, r1, shell=variant == "laid")
    if r2 is None:
        return j.Out("chain", "fail", "%s (%s): %s" % (cid, j.run.Kept(cid), "; ".join(wrong)))
    codes = {f[0] for f in r2["findings"]}
    if variant == "laid":
        wrong += Undone(j.run.dir, cid, r1, r2)
        if "NOT_LANDED" in codes:
            wrong.append("what the session left rolls back again: codes %s" % sorted(codes))
        what = "the first session laid its verdict down and went on past it"
    else:
        if not ("NOT_LANDED" in codes and "UNDONE" not in codes and r2["says"]["free"] == r1["says"]["free"]):
            wrong.append("what the machine left does not give the verdict again with no block let go: %s free "
                         "blocks (%s before), codes %s" % (r2["says"]["free"], r1["says"]["free"], sorted(codes)))
        what = "the machine stopped before the verdict's stitch, and what it left gives the verdict again"
    j.Out("chain", "fail" if wrong else "behaves", "%s (%s) — %s%s" % (
        cid, j.run.Kept(cid), what, ": " + "; ".join(wrong[:3]) if wrong else
        ", both mounts the ones predicted, no block of two owners"))


def Undone(d, cid, r1, r2):
    """C16: each file the first mount rolled back (r1's NOT_LANDED) is, in
    what the session left, the state the verdict rolled it back to, and wears
    settle:undone — what went otherwise."""
    fids = {int(m.group(1)) for f in r1["findings"] if f[0] == "NOT_LANDED" for m in [ROLLED_BACK.search(f[2])] if m}
    raw = json.loads(Log(os.path.join(d, "boots", cid + ".after.check.json")))
    tags = {f["fid"]: f.get("tag_text", []) for f in raw.get("files", []) if f.get("visible")}
    if not fids:
        return ["the first mount's NOT_LANDED names no fid it rolled back: %s" % [f[2][:80] for f in r1["findings"]
                                                                                if f[0] == "NOT_LANDED"]]
    was, now = View(r1), View(r2)
    wrong = ["fid %d rolled back is %s in what the session left" % (fid, now.get(fid)) for fid in sorted(fids)
             if now.get(fid) != was.get(fid)]
    return wrong + ["fid %d rolled back does not wear settle:undone (%s)" % (fid, tags.get(fid)) for fid in sorted(fids)
                    if "settle:undone" not in tags.get(fid, [])]


# ---------------------------------------------------------------------------
# Dedup: the power in the middle of the pass that lays a copy on its original
# ---------------------------------------------------------------------------


def Lies(r, name):
    """Where a cut's file NAME, one the base did not have, lies — [(kind,
    block)] in the file's order, holes left out — or None when the cut shows
    no such file."""
    fid = next((f[0] for f in r.get("files", []) if f[1] == name), None)
    runs = next((x[1] for x in r.get("new_runs", []) if x[0] == fid), None)
    if fid is None or runs is None:
        return None
    return [(kind, start + i) for kind, start, count in runs if kind != "hole" for i in range(count)]


def DedupStates(run, arg):
    """Every mountable cut of the run in plan order, as (id, state, what it
    shows otherwise or None), for original A and copy B (`A:B`, their names):
    `none` while B is not whole, then `own`, `part` or `laid` as none, some or
    all of B's blocks lie on A's at the same place. A lies on its own ground in
    every cut; B's own blocks are where the first cut that shows it whole on
    its own puts them, and B never lies anywhere else."""
    a, b = arg.split(":")
    rows, own = [], None
    for cid in Mountable(run):
        la, lb = Lies(run.results[cid], a), Lies(run.results[cid], b)
        if lb and la is None:
            rows.append((cid, "none", "%s shows and %s does not" % (b, a)))
            continue
        if not lb or len(lb) < len(la):
            rows.append((cid, "none", None))
            continue
        if own is None and all(k == "data" for k, _x in lb):
            own = [x for _k, x in lb]
        wrong = []
        if any(k != "data" for k, _x in la):
            wrong.append("%s lies otherwise than on its own ground: %s" % (a, sorted({k for k, _x in la})))
        if len(lb) != len(la):
            wrong.append("%s has %d block(s) and %s %d" % (b, len(lb), a, len(la)))
        elsewhere = [i for i, (k, x) in enumerate(lb[:len(la)]) if not (
            (k == "shared" and x == la[i][1]) or (k == "data" and own is not None and x == own[i]))]
        if elsewhere:
            i = elsewhere[0]
            wrong.append("%d of %s's blocks lie on neither its own nor %s's at the same place, first block %d of "
                         "it on %d (%s)" % (len(elsewhere), b, a, i, lb[i][1], lb[i][0]))
        laid = sum(1 for k, _x in lb if k == "shared")
        rows.append((cid, "own" if laid == 0 else "laid" if laid == len(lb) else "part",
                     "; ".join(wrong) or None))
    return rows


def DedupPicks(run, arg):
    """For powercut.py boots: the final state, and the first cut that shows
    the copy laid in part and the first that shows it laid whole."""
    yield run.final, "the final state"
    rows = DedupStates(run, arg)
    for state, what in (("part", "laid on its original in part"), ("laid", "laid on its original whole")):
        cid = next((c for c, s, _w in rows if s == state), None)
        if cid is not None:
            yield cid, "the first cut that shows the copy " + what


def Dedup(j, arg):
    """`--verify dedup:A:B` — §8.2, §16.7: the pass after a Tally lays copy B
    on original A an act at a time, among a logger's stitches, and the power
    goes after it. Every mountable cut shows B on its own blocks or on A's at
    the same place, block for block, and A on its own (DedupStates); both
    whole in every cut after the flush the write of B stood behind; B laid
    whole where the run ended; some cut on its own before the pass; and every
    booted cut that shows B laid, in part or whole, reads both back."""
    run, (a, b) = j.run, arg.split(":")
    rows = DedupStates(run, arg)
    wrong = ["%s (%s): %s" % (cid, run.Kept(cid), why) for cid, _s, why in rows if why]
    count = {k: sum(1 for _c, s, _w in rows if s == k) for k in ("none", "own", "part", "laid")}
    step = run.Step("dedup")
    flush = run.AckFlush(step) if step else None
    if flush is None:
        wrong.append("the write of %s and %s has no flush behind its acknowledgement" % (a, b))
    else:
        floor = run.closes[flush]
        late = [cid for cid, s, _w in rows if s == "none" and
                (run.cut[cid]["epoch"] > floor or cid == run.Canon("e%d-all" % floor))]
        if late:
            wrong.append("%d cut(s) after the flush %s's write stood behind show it not whole, first %s (%s)"
                         % (len(late), b, late[0], run.Kept(late[0])))
    final = next((s for cid, s, _w in rows if cid == run.final), "not mountable")
    if final != "laid":
        wrong.append("where the run ended %s is %s, not laid whole on %s" % (b, final, a))
    if not count["own"]:
        wrong.append("no cut shows %s whole on its own blocks before the pass" % b)
    logs = {cid: os.path.join(run.dir, "boots", cid + ".serial.log") for cid, _s, _w in rows}
    booted = [(cid, s, re.search(r"\[QUILL\] dedup: PASS — ", Log(logs[cid])))
              for cid, s, _w in rows if s in ("part", "laid") and os.path.exists(logs[cid])]
    wrong += ["%s (%s), %s laid %s, booted: both files did not read back" % (cid, run.Kept(cid), b,
              "in part" if s == "part" else "whole") for cid, s, ok in booted if not ok]
    if not any(s == "laid" for _c, s, _ok in booted):
        wrong.append("no cut that shows %s laid whole was booted" % b)
    j.Out("dedup", "fail" if wrong else "behaves", "%s — %s" % (
        "; ".join(wrong[:3]), "%d mountable cut(s)" % len(rows)) if wrong else
        "of %d mountable cuts, %d show %s not yet whole, %d on its own blocks, %d laid on %s's in part and %d whole, "
        "every block of it on its own or on %s's at the same place; both whole after the flush its write stood "
        "behind; %d booted cut(s) that show it laid read both back"
        % (len(rows), count["none"], b, count["own"], count["part"], a, count["laid"], a, len(booted)))
