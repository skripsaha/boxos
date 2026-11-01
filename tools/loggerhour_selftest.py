#!/usr/bin/env python3
"""loggerhour_selftest — loggerhour's judge judging itself: the rule by which a
block of free ground is the logger's (loggerhour.py TailsLaidAgain) shown
records of two Tallies made up here.

    loggerhour_selftest.py

One line a case, `ok TEXT` or `bad TEXT`, and 1 for any `bad`.

A record here is its writes in order, each one touch named as the case names
it — a stitch, a block of the logger's data, a block of free ground — and
carrying the bytes the case lays, so what is held is the judge's rule and
nothing of blklog's reading of a medium (blklog.Classifier, which the hour's
own record goes through). The Tallies are Replay's tuples, of which the rule
reads the Tally, the stitch its slice is taken after and the stitch it fell
owed at.
"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import loggerhour
from volumeread.blklog import Touch
from volumeread.ondisk import BLOCK, SECTOR

FID = 81
# Tally 7 falls owed at stitch 10 and is taken after stitch 13; Tally 9 at
# stitch 20, after stitch 23 — (Tally, k, cost, due, late, cum).
OWED = [(7, 13, 0, 10, 3, 0), (9, 23, 0, 20, 3, 0)]


def Lines(n, flip=False):
    """The logger's first n lines of 80 bytes (quilltest_book.c
    QuillLoggerLine), one byte of the last changed with `flip`."""
    out = bytearray(b"".join(b"%08u" % i + b"." * 71 + b"\n" for i in range(n)))
    if flip:
        out[-2] ^= 0x01
    return bytes(out)


class Write:
    __slots__ = ("seq", "lba", "sectors", "discard", "data", "touches")

    def __init__(self, seq, lba, kind, what, data):
        self.seq, self.lba, self.sectors, self.discard = seq, lba, BLOCK // SECTOR, False
        self.data = data.ljust(BLOCK, b"\0")
        self.touches = [Touch(lba, self.sectors, kind, what)]


class Record:
    """The record and its classifier at once. SHAPE is ("stitch", seq),
    ("file", lines) or ("free", lines[, flip]), each one block's write."""

    def __init__(self, shape):
        self.writes = []
        for k, (kind, n, *flip) in enumerate(shape, 1):
            lba = 4096 + 8 * k
            if kind == "stitch":
                what, data = "stitch seq %u" % n, b"\xa5" * BLOCK
            elif kind == "file":
                what, data = "fid %u data, named by stitch seq 0" % FID, Lines(n)
            else:
                what, data = "data run block %u, free ground as of its epoch" % (lba // 8), Lines(n, *flip)
            self.writes.append(Write(k, lba, kind, what, data))

    def Writes(self):
        return self.writes

    def SectorData(self, w, i):
        return w.data[i * SECTOR:(i + 1) * SECTOR]

    def Classify(self, w):
        return w.touches


def Stitches(first, last):
    return [("stitch", s) for s in range(first, last + 1)]


# (what the case is, its record, the free writes taken as the logger's: {log
# seq of the free block: (log seq of the write that laid it again, Tally)}).
# The hour's case is the first; each after it breaks one thing the rule holds.
CASES = [
    ("the hour's: each Tally's Engross laid again by the logger's next line before a stitch, in its window",
     Stitches(9, 10) + [("free", 5), ("file", 6)] + Stitches(11, 20) + [("free", 3), ("file", 4)] + Stitches(21, 24),
     {3: (4, 7), 15: (16, 9)}),
    ("a second such block in the window of a Tally that has had its one",
     Stitches(9, 10) + [("free", 5), ("file", 6), ("stitch", 11), ("free", 7), ("file", 8)] + Stitches(12, 14),
     {3: (4, 7)}),
    ("a block laid after the stitch the slice is taken after, out of the window",
     Stitches(9, 13) + [("free", 5), ("file", 6)] + Stitches(14, 15),
     {}),
    ("a block laid before the stitch the Tally falls owed at, out of the window",
     Stitches(8, 9) + [("free", 5), ("file", 6)] + Stitches(10, 11),
     {}),
    ("a tail one byte of which the logger's write does not lay again",
     Stitches(9, 10) + [("free", 5, True), ("file", 6)] + Stitches(11, 12),
     {}),
    ("a tail a stitch named before the logger's next line",
     Stitches(9, 10) + [("free", 5), ("stitch", 11), ("file", 6)] + Stitches(12, 13),
     {}),
    ("a tail the logger's write lays with nothing more",
     Stitches(9, 10) + [("free", 5), ("file", 5)] + Stitches(11, 12),
     {}),
]


def SelfTest():
    failed = 0
    for what, shape, want in CASES:
        record = Record(shape)
        lba = {w.seq: w.lba for w in record.Writes()}
        want = {(seq, lba[seq]): laid for seq, laid in want.items()}
        got = loggerhour.TailsLaidAgain(record, record, FID, OWED)
        held = got == want
        failed += not held
        print("%s the judge's rule — %s: the logger's %s (%s)" % (
            "ok" if held else "bad", what,
            ", ".join("log seq %u laid again by %u, Tally %u" % (k[0], v[0], v[1]) for k, v in sorted(got.items()))
            or "none", "as the case holds" if held else "the case holds %s" % (
                ", ".join("log seq %u by %u, Tally %u" % (k[0], v[0], v[1]) for k, v in sorted(want.items()))
                or "none")))
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) != 1:
        raise SystemExit(__doc__)
    sys.exit(SelfTest())
