"""selftest_book_keep — the landings of a file deleted for a term (volume_ribbon.h,
Labeled KEEP; volume_tally.h, the FILES header's keep_ns). The deleted state is
the record's own — the bit TAGFS_FILE_DELETED and the time the file is kept
until — and the ribbon puts it on and takes it off by one verb: a Labeled that
names a term deletes the file, one that names 0 brings it back, and a Labeled
that names the bit in its FLAGS, or a Born that carries it, does not read. Each a
fork of the base selftest_book.py lays."""

from . import bookread, ondisk, stitch, tally
from .selftest_book import Case, Found

TERM_NS = 1700000000123456789


def KeepCases(bt):
    S = bt.S

    def Delete(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, keep=TERM_NS)], flags=stitch.DATA_FLUSHED)

    def DeleteThenRestore(o):
        pen = bt.Pen(o)
        q = pen.next_seq
        pen.Sew([stitch.Labeled(fid=S.fid, base=q - S.gen, keep=TERM_NS)], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=S.fid, base=1, keep=0)], flags=stitch.DATA_FLUSHED)

    def MoveTerm(o):
        pen = bt.Pen(o)
        q = pen.next_seq
        pen.Sew([stitch.Labeled(fid=S.fid, base=q - S.gen, keep=TERM_NS)], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=S.fid, base=1, keep=TERM_NS + 5)], flags=stitch.DATA_FLUSHED)

    def BitInFlags(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, flags=(ondisk.FILE_DELETED, 0))],
                flags=stitch.DATA_FLUSHED)

    def Kept(c, until):
        fs = c.mount.files[S.fid]
        return bool(fs.flags & ondisk.FILE_DELETED) and fs.keep_ns == until

    def HeadRoundTrip(c):
        """The Tally's FILES header of a deleted file carries its term after the tags, a header
        that is not deleted none, and a deleted header with no term, or cut short, does not read."""
        deleted = tally.FileState(7, 3, ondisk.FILE_ACTIVE | ondisk.FILE_DELETED, 9, 1, 2, b"kept", [4, 6], [],
                                  keep_ns=TERM_NS)
        plain = tally.FileState(7, 3, ondisk.FILE_ACTIVE, 9, 1, 2, b"kept", [4, 6], [])
        body = tally.FileHeadEncode(deleted)
        back = tally.FileHeadDecode(7, body, [])
        kept = back.keep_ns == TERM_NS and back.flags == deleted.flags and back.tags == [4, 6]
        bare = tally.FileHeadDecode(7, tally.FileHeadEncode(plain), []).keep_ns == 0
        refused = 0
        for bad in (body[:-1],
                    tally.FileHeadEncode(tally.FileState(7, 3, deleted.flags, 9, 1, 2, b"kept", [4, 6], [], keep_ns=0))):
            try:
                tally.FileHeadDecode(7, bad, [])
            except bookread.Short:
                refused += 1
        return kept and bare and refused == 2

    return [
        Case("K1 a Labeled KEEP deletes the file until its term", Delete, checks=[
            ("the record says it is deleted, and until when", lambda c: Kept(c, TERM_NS)),
            ("its name, tags and runs are as they were",
             lambda c: c.mount.files[S.fid].name == S.name and c.mount.files[S.fid].runs == S.runs),
            ("the volume is written on", lambda c: not c.mount.glass)]),
        Case("K2 a Labeled KEEP of 0 brings it back", DeleteThenRestore, checks=[
            ("the record is not deleted and names no term",
             lambda c: not c.mount.files[S.fid].flags & ondisk.FILE_DELETED and c.mount.files[S.fid].keep_ns == 0),
            ("the volume is written on", lambda c: not c.mount.glass)]),
        Case("K3 a Labeled KEEP of a file deleted already moves its term", MoveTerm, checks=[
            ("the record is deleted until the later term", lambda c: Kept(c, TERM_NS + 5))]),
        Case("K4 the deleted bit in a Labeled's FLAGS does not read", BitInFlags, checks=[
            ("the file is not deleted by it", lambda c: not c.mount.files[S.fid].flags & ondisk.FILE_DELETED)]),
        Case("K5 a FILES header of a deleted file carries its term", lambda o: None, checks=[
            ("a header's term is read back, and one cut short or with no term does not read", HeadRoundTrip)]),
    ]
