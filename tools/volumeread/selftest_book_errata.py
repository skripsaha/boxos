"""selftest_book_errata — the mount's verdict (spec_v2 §3.9, §2.8 Undone,
§16.5 п.4-7): a stitch whose data never landed, sewn without its first flush
(C3) or on a medium that lied (C5, C6), and a record that does not fit its
file (the rule of rot), are rolled back by a verdict the mount holds in
memory — Errata, a sorted list of (seq of the stitch undone, why, fids) the
session's first stitch lays down as an Undone each — and the volume is
written on: no glass. A file rolled back onto blocks another file holds is
locked, as ever, and no verdict names it. And an Undone names a file by all
64 bits of its id, one rule with the kernel's (RibbonFidsPut).

The verdict leaves the file as the next mount leaves it once the Undone is
laid (H-1): a record that does not fit rolls the file back to where its
stitch found it, its earlier records of the stitch passed over too; a mount
after the Undone settled on a medium that lied, which holds the stitch it
names again, finds nothing more to undo; and a stitch undone for both
reasons is undone once (§2.8: one Undone a stitch undone)."""

from . import stitch
from .selftest_book import Born, Found
from .selftest_cases import Case

# An Undone of the stitch one back, why 1, over fids 5 and 2^32 + 7: the bytes
# the kernel's ribbon suite lays with its own codec (tests/ribbon_tests.c).
UNDONE_WIDE_GOLDEN = "0609010102058180808010"


def Written(c):
    """The volume is written on: no reason of glass, in volume-read or in the
    kernel of this tree mounting it."""
    return not c.mount.glass and c.kernel["mounts"] is True and c.kernel["glass"] == 0


def ErrataCases(bt):
    f1, f2 = bt.fid, bt.fid + 1
    S = bt.S
    seqs = {}

    def Shaped(o):
        """S's first block put in anew at F by a stitch sewn without
        DATA_FLUSHED, its digest one its bytes do not hold."""
        bt.Fresh(o, 0, 1, 21)
        pen = bt.Pen(o)
        q = pen.next_seq
        seqs["shaped"] = q
        pen.Sew([stitch.Shaped(fid=S.fid, base=q - S.gen, size=S.size, mtime_ns=2,
                               splices=[(0, 1, [stitch.Data(bt.F, 1)])], digests=[0x5151])], flags=0)

    def BornUnlanded(o):
        bt.Fresh(o, 0, 1, 22)
        pen = bt.Pen(o)
        seqs["born"] = pen.next_seq
        pen.Sew([Born(bt.F, f2, [0x2222], b"unlanded")], flags=0)

    def Rot(o):
        pen = bt.Pen(o)
        seqs["rot"] = pen.next_seq
        pen.Sew([stitch.Labeled(fid=S.fid, base=1, name=b"stray")], flags=stitch.DATA_FLUSHED)

    def LiedTwo(o):
        """Two stitches on a medium that lied, both DATA_FLUSHED, neither one's
        data landed: the check reads both (M-2), and undoes each."""
        bt.Fresh(o, 0, 2, 23)
        pen = bt.Pen(o)
        lied = stitch.DATA_FLUSHED | stitch.MEDIUM_LIED
        seqs["lied"] = pen.next_seq
        pen.Sew([Born(bt.F, f1, [0x3131], b"lied-a")], flags=lied)
        pen.Sew([Born(bt.F + 1, f2, [0x3232], b"lied-b")], flags=lied)
        return pen

    def RotLate(o):
        """A Shaped of S that fits, then a Labeled of S that puts on a tag no
        Coined names — a Coined lost with a sector of a closed unit (C13): the
        rule of rot at S's second record of the stitch."""
        d = bt.Fresh(o, 0, 1, 25)
        pen = bt.Pen(o)
        seqs["late"] = pen.next_seq
        pen.Sew([stitch.Shaped(fid=S.fid, base=pen.next_seq - S.gen, size=S.size, mtime_ns=3,
                               splices=[(0, 1, [stitch.Data(bt.F, 1)])], digests=d),
                 stitch.Labeled(fid=S.fid, base=0, add=[bt.tag + 7])], flags=stitch.DATA_FLUSHED)
        return pen

    def RotLateLaid(o):
        """RotLate, and the session's first stitch after it laying its Undone."""
        RotLate(o).Sew([stitch.Undone(1, 2, [S.fid])], flags=stitch.DATA_FLUSHED)

    def LiedLaid(o):
        """LiedTwo, and the session's first stitch after it laying an Undone
        of each — on a medium that lied its flags say so: the next mount holds
        the newest two again, the second undone and the stitch that undid it."""
        lied = stitch.DATA_FLUSHED | stitch.MEDIUM_LIED
        LiedTwo(o).Sew([stitch.Undone(2, 1, [f1]), stitch.Undone(1, 1, [f2])], flags=lied)

    def BothWhys(o):
        """One stitch sewn without DATA_FLUSHED: a Born whose data never
        landed, and a Labeled of S on a base it does not stand at."""
        bt.Fresh(o, 0, 1, 26)
        pen = bt.Pen(o)
        seqs["both"] = pen.next_seq
        pen.Sew([Born(bt.F, f2, [0x2626], b"both"), stitch.Labeled(fid=S.fid, base=1, name=b"stray")], flags=0)

    def Wide(o):
        """A file of id 2^32 + 5 born, and an Undone of the next stitch over
        it: the Born is passed over by all 64 bits of the id."""
        d = bt.Fresh(o, 0, 1, 24)
        wide = (1 << 32) + 5
        pen = bt.Pen(o)
        pen.Sew([Born(bt.F, wide, d, b"wide")], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Undone(1, 1, [wide])], flags=stitch.DATA_FLUSHED)

    errata = lambda c: c.mount.errata
    return [
        Case("E1 a Shaped's data never landed: a verdict, not glass", Shaped, must={"NOT_LANDED"}, checks=[
            ("Errata holds the stitch, why 1, and fid %u" % S.fid,
             lambda c: errata(c) == [(seqs["shaped"], 1, [S.fid])]),
            ("the file stands as it was before the stitch", lambda c: c.mount.files[S.fid].runs == S.runs),
            ("the volume is written on", Written)]),
        Case("E2 a Born's data never landed: no file, its id burned", BornUnlanded, must={"NOT_LANDED"}, checks=[
            ("Errata holds the stitch, why 1, and fid %u" % f2, lambda c: errata(c) == [(seqs["born"], 1, [f2])]),
            ("there is no file %u, and its id is never given again" % f2,
             lambda c: f2 not in c.mount.files and c.mount.counts["next_fid"] == f2 + 1),
            ("the volume is written on", Written)]),
        Case("E3 a record that does not fit its file: the rule of rot, why 2", Rot, must={"BASE_MISMATCH"},
             checks=[("Errata holds the stitch, why 2, and fid %u" % S.fid,
                      lambda c: errata(c) == [(seqs["rot"], 2, [S.fid])]),
                     ("the file stays as it last matched", lambda c: c.mount.files[S.fid].name == S.name),
                     ("the volume is written on", Written)]),
        Case("E4 two stitches undone on a medium that lied (C6)", LiedTwo, must={"NOT_LANDED"}, checks=[
            ("Errata holds both stitches, each with its file",
             lambda c: errata(c) == [(seqs["lied"], 1, [f1]), (seqs["lied"] + 1, 1, [f2])]),
            ("neither file is born", lambda c: f1 not in c.mount.files and f2 not in c.mount.files),
            ("the volume is written on", Written)]),
        Case("E6 the rule of rot at a file's second record: the file where the stitch found it", RotLate,
             must={"BASE_MISMATCH"}, checks=[
                 ("Errata holds the stitch, why 2, and fid %u" % S.fid,
                  lambda c: errata(c) == [(seqs["late"], 2, [S.fid])]),
                 ("the Shaped before it is passed over too: S's runs and mtime are as they were",
                  lambda c: c.mount.files[S.fid].runs == S.runs and
                  c.mount.files[S.fid].modified_ns == S.modified_ns),
                 ("its fresh block rests in Fallow", lambda c: any(s <= bt.F < s + n for s, n in c.mount.fallow)),
                 ("the volume is written on", Written)]),
        Case("E6b the same once its Undone is laid: the next mount leaves S as this one did", RotLateLaid,
             must={"UNDONE"}, checks=[
                 ("S stands as before the stitch", lambda c: c.mount.files[S.fid].runs == S.runs and
                  c.mount.files[S.fid].modified_ns == S.modified_ns),
                 ("nothing does not fit, and no verdict is left to lay",
                  lambda c: not Found(c, "BASE_MISMATCH") and errata(c) == [])]),
        Case("E7 a medium that lied, mounted after the Undones settled: nothing undone twice", LiedLaid,
             must={"UNDONE"}, checks=[
                 ("the stitch the Undone names is held again and found nothing more to undo",
                  lambda c: not Found(c, "NOT_LANDED") and errata(c) == []),
                 ("neither file is born", lambda c: f1 not in c.mount.files and f2 not in c.mount.files)]),
        Case("E8 a stitch undone for both reasons: one Undone, every file of it", BothWhys,
             must={"NOT_LANDED", "BASE_MISMATCH"}, checks=[
                 ("Errata holds the stitch once, why 1 — the check comes first — with both files",
                  lambda c: errata(c) == [(seqs["both"], 1, sorted([f2, S.fid]))]),
                 ("the volume is written on", Written)]),
        Case("E5 an Undone of a file id past 32 bits", Wide, must={"UNDONE"}, checks=[
            ("its bytes are the kernel's own codec's",
             lambda c: stitch.Undone(1, 1, [5, (1 << 32) + 7]).Bytes().hex() == UNDONE_WIDE_GOLDEN),
            ("the Born it names is passed over: no file %u" % ((1 << 32) + 5),
             lambda c: (1 << 32) + 5 not in c.mount.files and Found(c, "UNDONE", "%u" % ((1 << 32) + 5))),
            ("and no verdict is left to lay", lambda c: errata(c) == [])]),
    ]
