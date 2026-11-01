#!/usr/bin/env python3
"""loggerhour — the oracles of logcheck's loggerhour (tools/logcheck_book_logger.sh):
a logger of one line a second on a format-v2 book, judged off the record
blklogwrites kept of the boot that ran it (spec_v2 §10.4 step 2.4, §12.1).

    loggerhour.py BASE RECORD IMG GUESTLOG

BASE is the image before the boot, RECORD its blklogwrites log, IMG what the
boot left, GUESTLOG the serial log with quilltest's `logger` line and the
kernel's Tally lines. One line a verdict, `ok TEXT` or `bad TEXT`.

The record's writes are named by blklog.Classifier (a stitch, a Tally's node
or slot, a block of a file's data); the stitches are read off the record's own
sectors, each by its StitchHead (made_ns, whole seconds), its useful bytes
(§4.2) and its records, so a Tally's replay has every stitch since the
boot, the ribbon the image left having let the older go.

The numbers come from the tree's headers and the format, not from a run:
SETTLE_WINDOW_US (settle.h), TALLY_SLACK (binder.h), the atom of a book on a
medium of 512-byte sectors, one block.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from volumeread import binder, blklog, bookread, stitch
from volumeread.bookground import BookGround
from volumeread.bookmount import BookMount
from volumeread.bookread import HEAD_BYTES, SECTOR_HEAD, UNIT_STITCH
from volumeread.medium import Medium
from volumeread.ondisk import BLOCK, SECTOR, SECTORS_PER_BLOCK

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
LINE_BYTES = 80
# A stamp is a whole second (StitchHead made_ns), and a stitch's cycle runs a
# few milliseconds past the window: the gap between two stamped cycles a
# window apart reads one second short at the least, and the window's promise
# is kept to a second past it.
STAMP_S = 1
PROMISE_S = 2
# After a Tally falls owed the writer lays its tail in a block (Engross) in a
# cycle of its own, and the slice is taken after it: where the Tally says it
# stands, at most this many stitches past the one the bytes reached the cost.
TALLY_LATE = 2
# A Tally owed asks cycles outside the window: the Engross one, and the first
# of the lines written while the slice was taken (seen on the record).
TALLY_EARLY = 2


def Header(path, pattern):
    with open(os.path.join(ROOT, path)) as f:
        found = re.search(pattern, f.read())
    if not found:
        raise SystemExit("loggerhour: %s carries no %s" % (path, pattern))
    return int(found.group(1))


WINDOW_S = Header("src/kernel/tagfs/settle/settle.h", r"#define SETTLE_WINDOW_US\s+(\d+)ull") / 10 ** 6
SLACK = Header("src/kernel/tagfs/settle/binder.h", r"#define TALLY_SLACK\s+(\d+)u")


class Stitch:
    __slots__ = ("seq", "made", "useful", "flags", "sectors", "fids")

    def __init__(self, seq, parsed, sectors):
        self.seq, self.made, self.useful, self.flags = seq, parsed.made_ns // 10 ** 9, parsed.end, parsed.flags
        self.sectors, self.fids = sectors, set()
        for kind, _ignorable, body, _at in parsed.records:
            if kind in (stitch.BORN, stitch.SHAPED):
                self.fids.add(stitch.RecordDecode(kind, body, seq)["fid"])


def RecordStitches(log, stand):
    """{seq: Stitch} for every stitch the record holds every sector of."""
    parts = {}
    for w in log.Writes():
        if w.discard:
            continue
        for i in range(w.sectors):
            vlba = w.lba + i - stand.start
            raw = bytes(log.SectorData(w, i))
            if vlba < 0 or raw[4:HEAD_BYTES] == bytes(HEAD_BYTES - 4) or \
                    not bookread.SectorCheck(raw, stand.deed["uuid"], vlba):
                continue
            h = SECTOR_HEAD.Unpack(raw)
            if h["kind"] == UNIT_STITCH:
                parts.setdefault(h["seq"], {})[h["at"]] = (raw[HEAD_BYTES:], h["of"])
    out = {}
    for seq, held in parts.items():
        of = next(iter(held.values()))[1]
        if sorted(held) != list(range(of)):
            continue
        parsed = stitch.Parsed(b"".join(held[k][0] for k in range(of)))
        if parsed.why is None:
            out[seq] = Stitch(seq, parsed, of)
    return out


def Guest(path):
    text = open(path, errors="replace").read()
    said = re.search(r"\[QUILL\] logger: PASS — (\S+) \(fid (\d+)\): (\d+) lines in (\d+) ms, (\d+) anchor\(s\) the "
                     r"longest answered in (\d+) ms, (\d+) bytes, print 0x([0-9A-F]+)", text)
    if not said:
        raise SystemExit("loggerhour: the guest's log carries no logger verdict")
    name, fid, lines, ms, anchors, longest_ms, size = said.group(1), *map(int, said.groups()[1:7])
    tallies = [tuple(map(int, t)) for t in re.findall(
        r"\[TagFS\] Tally (\d+) is written over slot (\d+): the state after stitch (\d+), \d+ node\(s\) laid in "
        r"(\d+) block\(s\) a copy", text)]
    return {"name": name, "fid": fid, "lines": lines, "ms": ms, "anchors": anchors, "longest_ms": longest_ms,
            "bytes": size, "print": int(said.group(8), 16), "tallies": tallies}


def Cycles(stitches, fid):
    """The stitches of the logger's phase in order: from the one that makes
    its file on."""
    born = min((s.seq for s in stitches.values() if fid in s.fids), default=None)
    return [s for s in sorted(stitches.values(), key=lambda s: s.seq) if born is not None and s.seq >= born]


def Replay(guest, stitches, carried, cost_next):
    """The rule of binder.c over the record's stitches: a Tally falls owed at
    the first stitch by which the useful bytes of the stitches past the newest
    slot's `at` reach TALLY_SLACK times what it costs — (2 * blocks a copy + 1)
    blocks, its nodes in both copies and its slot — and the writer then lays
    its tail in a block (Engross) before the slice is taken. `carried` are the
    useful bytes the base's ribbon held already.

    Returns (owed, rewrites, waiting): `owed` each Tally the bytes owed, as
    (Tally, k, cost, due stitch, stitches from the due one to k, bytes at k);
    `rewrites` the Tallies that took in no stitch the one before had not
    (Glean's, §4.8: the bytes do not owe them); `waiting` the stitches that
    stood past the last slice's `at` when the bytes had reached the next
    Tally's cost, none if they had not."""
    order, owed, rewrites, at = sorted(stitches), [], 0, 0
    for tally, _slot, k, blocks in guest["tallies"]:
        if k == at and at:
            rewrites += 1
            continue
        cost, cum, due, seen = (2 * blocks + 1) * BLOCK, carried, None, []
        for seq in (q for q in order if q > at):
            cum += stitches[seq].useful
            if due is None and cum >= SLACK * cost:
                due = seq
            if due is not None:
                seen.append(seq)
            if seq == k:
                break
        owed.append((tally, k, cost, due, len(seen) - 1 if due is not None else -1, cum))
        at, carried = k, 0
    cum, waiting = carried, []
    for seq in (q for q in order if q > at):
        cum += stitches[seq].useful
        if (cum >= SLACK * cost_next or waiting) and not stitches[seq].flags & stitch.CLEAN:
            waiting.append(seq)
    return owed, rewrites, waiting


def TouchBytes(log, w, t):
    return b"".join(bytes(log.SectorData(w, t.first_lba - w.lba + i)) for i in range(t.sectors))


def TailsLaidAgain(log, classifier, fid, owed):
    """The writes of the record's free ground that are the logger's tail the
    Helm's service laid for a Tally owed and the logger laid again before a
    cycle named it: {(log seq of the write, its first sector): (log seq of the
    one that laid it again, the Tally)}.

    A Tally owed asks the Helm's service to lay every tail in its block at the
    DONE of the cycle the bytes reached its cost in (helm/engross.h). §16.5
    п.14 gives the service the files with no writer and the holder of the
    handle its own tail; the kernel reads the writer as the Helm's holder, and
    a program holds it for one write — each fwrite opens the file for its own
    job (write_job.c wjob_make, tagfs_open) — so between two lines the
    logger's file has no writer and the service lays its tail. And while the
    Tally is owed no Slip is given (plot_slip.c), so a line the logger appends
    before the cycle that carries the Engross lays the block again on fresh
    ground, its tail folded in, and the Engross's block is let go before any
    stitch named it: free at once (§3.4 L-8, blklog_book.py). One is that
    block and no other write: a single block of free ground, the file's bytes
    up to the size the Engross saw and zeros past it, laid past the stitch the
    Tally fell owed at and before the one its slice is taken after, and laid
    again — the same bytes and more — by the next write of the logger's data
    before a stitch; one for each Tally at the most. It rests on that reading
    of п.14: a service that yields to a writer who writes again within the
    window lays no such block, and this is then no longer the logger's.
    `owed` is Replay's; loggerhour_selftest.py holds the rule to its cases."""
    writes = [w for w in log.Writes() if not w.discard]
    again, after = {}, None
    for k, w in enumerate(writes):
        ts = classifier.Classify(w)
        for t in ts:
            said = re.match(r"stitch seq (\d+)", t.what) if t.kind == "stitch" else None
            after = int(said.group(1)) if said else after
        tally = next((o[0] for o in owed if o[3] is not None and after is not None and o[3] <= after < o[1] and
                      o[0] not in {v[1] for v in again.values()}), None)
        if tally is None:
            continue
        for t in ts:
            if t.kind != "free" or t.sectors != SECTORS_PER_BLOCK:
                continue
            tail = TouchBytes(log, w, t)
            size = len(tail.rstrip(b"\0"))
            if not 0 < size < BLOCK or b"\0" in tail[:size]:
                continue
            for later in writes[k + 1:]:
                us = classifier.Classify(later)
                if any(u.kind == "stitch" for u in us):
                    break
                laid = [TouchBytes(log, later, u) for u in us if u.kind == "file" and
                        re.search(r"fid %u data" % fid, u.what)]
                if any(b[:size] == tail[:size] and len(b.rstrip(b"\0")) > size for b in laid):
                    again[(w.seq, t.first_lba)] = (later.seq, tally)
                    break
            if (w.seq, t.first_lba) in again:
                break
    return again


def Fnv(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def Judge(base_path, record_path, img_path, guest):
    out = []

    def say(held, text):
        out.append(("ok " if held else "bad ") + text)

    base = Medium(base_path)
    log = blklog.BlkLog(record_path, base.SizeSectors())
    classifier = blklog.Classifier(log, base)
    stitches = RecordStitches(log, classifier.stand)
    fid, lines, anchors = guest["fid"], guest["lines"], guest["anchors"]
    owed, rewrites, waiting = Replay(guest, stitches, RibbonUseful(base_path), NextCost(img_path))
    again = TailsLaidAgain(log, classifier, fid, owed)
    took = {}
    for w in log.Writes():
        for t in ([] if w.discard else classifier.Classify(w)):
            own = t.kind != "file" or re.search(r"fid %u data" % fid, t.what)
            kind = "file" if t.kind == "free" and (w.seq, t.first_lba) in again else t.kind
            took.setdefault(kind if own else "file of another", []).append(t.sectors)
    sectors = {kind: sum(n) for kind, n in took.items()}
    cycles = Cycles(stitches, fid)

    other = {k: sum(n) for k, n in took.items() if k not in ("stitch", "sheet", "slot", "file")}
    say(not other and bool(cycles),
        "every write of the record is a stitch, a node or a slot of a Tally, or a block of the logger's file (fid %u): "
        "%u stitch(es), %u node sector(s), %u slot sector(s), %u data sector(s) — %u block(s) of them a tail the "
        "Helm's service laid for a Tally owed and the logger's next line laid again before a cycle named it (%s), "
        "one for each of the %u Tally(ies) owed at the most; anything else: %s" % (
            fid, len(took.get("stitch", ())), sectors.get("sheet", 0), sectors.get("slot", 0), sectors.get("file", 0),
            len(again), ", ".join("log seq %u laid again by %u, Tally %u" % (k[0], v[0], v[1])
                                  for k, v in sorted(again.items())) or "none", len(owed), other or "nothing"))
    big = [s.seq for s in stitches.values() if s.sectors != SECTORS_PER_BLOCK]
    say(not big and sectors.get("stitch", 0) == len(stitches) * SECTORS_PER_BLOCK,
        "a cycle costs the host one atom: %d stitch(es) of %u sector(s) = %u B each%s"
        % (len(stitches), SECTORS_PER_BLOCK, SECTORS_PER_BLOCK * SECTOR, ", longer: %s" % big[:4] if big else ""))
    filled = lines * LINE_BYTES // BLOCK
    blocks = sectors.get("file", 0) // SECTORS_PER_BLOCK
    low, high = filled, 2 * filled + len(guest["tallies"]) + 1
    say(low <= blocks <= high, "the lines ride the stitches (Slip) and go to blocks only as the file fills them or a "
        "Tally owes its tail a block: %u data block(s) for %u line(s) of %u B — %u block(s) filled, each laying the "
        "full block and the new tail at the most, and one for each of the %u Tally(ies): %u..%u block(s)"
        % (blocks, lines, LINE_BYTES, filled, len(guest["tallies"]), low, high))
    n = max(1, len(cycles))
    say(bool(cycles), "the host's bytes a cycle: %u B in %u cycle(s) of the logger's phase = one atom of %u B + the "
        "fill blocks %u B + the Tallies %u B" % (
            sum(sectors.values()) * SECTOR // n, len(cycles), BLOCK, sectors.get("file", 0) * SECTOR // n,
            (sectors.get("sheet", 0) + sectors.get("slot", 0)) * SECTOR // n))

    window = [s for s in cycles if not s.flags & stitch.CLEAN]
    gaps = [(b.seq, b.made - a.made) for a, b in zip(window, window[1:])]
    early = [seq for seq, gap in gaps if gap < WINDOW_S - STAMP_S]
    asked = anchors + 1 + TALLY_EARLY * len(owed)
    say(len(early) <= asked, "no cycle follows another by less than a window but those asked for at once: %u of %u "
        "cycle(s) came less than %u s after the one before (a stamp is a whole second: the window less %u); allowed "
        "%u = %u anchor(s) + the cycle of the first lines + %u for each of the %u Tally(ies), its Engross and the "
        "cycle after its slice" % (len(early), len(window), WINDOW_S - STAMP_S, STAMP_S, asked, anchors, TALLY_EARLY,
                                   len(owed)))
    span = window[-1].made - window[0].made if window else 0
    most = int(span // WINDOW_S) + asked
    say(len(window) <= most, "at most a cycle a window of %u s: %u cycle(s) over the %u s the logger's cycles span; "
        "the bound is %u = %u window(s) + the %u asked for at once" % (WINDOW_S, len(window), span, most,
                                                                      span // WINDOW_S, asked))
    longest = max((gap for _seq, gap in gaps), default=0)
    say(longest <= WINDOW_S + PROMISE_S, "and no line waits past the window: the longest gap between two cycles is "
        "%u s against %u = the window + %u s, the stamp's second and a cycle's own time" % (
            longest, WINDOW_S + PROMISE_S, PROMISE_S))
    say(guest["longest_ms"] * 1000 < WINDOW_S * 10 ** 6 // 2, "an anchor is answered at once, not at the window: the "
        "longest of the %u took %u ms against %u ms, half the window" % (anchors, guest["longest_ms"],
                                                                         int(WINDOW_S * 500)))

    for tally, k, cost, due, late, cum in owed:
        say(due is not None and 0 <= late <= TALLY_LATE,
            "Tally %u (after stitch %u) falls owed where the useful bytes reach TALLY_SLACK %u x %u B, its cost: "
            "at stitch %s, and its slice is taken %s stitch(es) after it, %u at the most" % (
                tally, k, SLACK, cost, due, late, TALLY_LATE))
    cost_next = NextCost(img_path)
    useful = [s.useful for s in window]
    mean = sum(useful) // max(1, len(useful))
    say(len(waiting) <= TALLY_LATE + 1,
        "and no Tally is owed by the bytes and left unwritten: %u cycle(s) of %u..%u useful B, %u B on the mean "
        "(a cycle a line is 135 B, a window's five or six lines 455..535 B); a Tally costs %u B, so one is owed "
        "every %u cycle(s) here — the spec's ≈ 200 is the end of that range with 135 B a cycle (%u) — and %u "
        "stand written (%u more rewrote the state the one before took in)" % (
            len(window), min(useful, default=0), max(useful, default=0), mean, cost_next,
            SLACK * cost_next // max(1, mean), SLACK * cost_next // 135, len(owed), rewrites))

    name = guest["name"]
    got = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "volume-read.py"), img_path, "get", name],
                         capture_output=True).stdout
    say(len(got) == guest["bytes"] and Fnv(got) == guest["print"], "after the halt every line is in %s: %u of %u "
        "bytes, print 0x%016X against the guest's 0x%016X" % (name, len(got), guest["bytes"], Fnv(got),
                                                             guest["print"]))
    return out


def MountOf(path):
    m = BookMount(BookGround(Medium(path), None))
    if m.refusal is not None:
        raise SystemExit("loggerhour: %s does not mount: %s" % (path, m.refusal))
    return m


def RibbonUseful(path):
    return binder.BindSaid(MountOf(path))[3]


def NextCost(path):
    return binder.BindSaid(MountOf(path))[1]


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    for line in Judge(sys.argv[1], sys.argv[2], sys.argv[3], Guest(sys.argv[4])):
        print(line)
