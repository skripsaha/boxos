"""selftest_book_passport — the landings of the passport of the signed program a file holds
(volume_tally.h, the FILES header's passport; volume_ribbon.h, Born and Labeled PASSPORT). Who a signed
program is across every copy and version of it — its author and its name, as the kernel makes it of the
program's signature — is the card of its file: laid by a Born, laid anew by a Labeled PASSPORT, and kept by
every other change of the file, a change of what it holds among them; a Labeled PASSPORT of 0 takes it off
once a program with no signature that holds was started from the file. A passport is never a file's id: one
without bit 63 does not read, in a header, a Born or a Labeled, nor does a Labeled that names the bit in its
FLAGS. Each a fork of the base selftest_book.py lays."""

from . import bookread, ondisk, stitch, tally
from .ondisk import BLOCK
from .selftest_book import Case

GAME = ondisk.PASSPORT_BIT | 0x1234_5678_9ABC
NEXT = ondisk.PASSPORT_BIT | 0x0FED_CBA9_8765
SIGNED = ondisk.FILE_ACTIVE | ondisk.FILE_HEAD | ondisk.FILE_PASSPORT


def PassportCases(bt):
    S = bt.S
    fid = bt.fid

    def Signed(passport=GAME):
        d = [bookread.Integrity(bt.Pattern(9), bt.lanes)]
        return stitch.Born(fid=fid, flags=SIGNED, size=BLOCK, created_ns=1, modified_ns=1, name=b"game.box",
                           tags=(), runs=[stitch.Data(bt.F, 1)], digests=d, head=bt.Pattern(9)[:ondisk.HEAD_MOST],
                           passport=passport)

    def Born(o):
        bt.Fresh(o, 0, 1, 9)
        bt.Pen(o).Sew([Signed()], flags=stitch.DATA_FLUSHED)

    def Updated(o):
        d = bt.Fresh(o, 0, 2, 9)
        pen = bt.Pen(o)
        pen.Sew([Signed()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Shaped(fid=fid, base=1, size=BLOCK, mtime_ns=2, splices=[(0, 1, [stitch.Data(bt.F + 1, 1)])],
                               digests=d[1:]),
                 stitch.Labeled(fid=fid, base=0, head=bt.Block(bt.F + 1)[:ondisk.HEAD_MOST])],
                flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, name=b"game-2.box")], flags=stitch.DATA_FLUSHED)

    def Anew(o):
        bt.Fresh(o, 0, 1, 9)
        pen = bt.Pen(o)
        pen.Sew([Signed()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, passport=NEXT)], flags=stitch.DATA_FLUSHED)

    def Off(o):
        bt.Fresh(o, 0, 1, 9)
        pen = bt.Pen(o)
        pen.Sew([Signed()], flags=stitch.DATA_FLUSHED)
        pen.Sew([stitch.Labeled(fid=fid, base=1, passport=0)], flags=stitch.DATA_FLUSHED)

    def OnPlain(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, passport=GAME)], flags=stitch.DATA_FLUSHED)

    def BitInFlags(o):
        pen = bt.Pen(o)
        pen.Sew([stitch.Labeled(fid=S.fid, base=pen.next_seq - S.gen, flags=(ondisk.FILE_PASSPORT, 0))],
                flags=stitch.DATA_FLUSHED)

    def Card(c, f):
        fs = c.mount.files.get(f)
        if fs is None or bool(fs.flags & ondisk.FILE_PASSPORT) != bool(fs.passport):
            return None
        return fs.passport

    def Refused(read, bad):
        try:
            read(bad)
        except bookread.Short:
            return True
        return False

    def HeaderRoundTrip(c):
        """A header's passport closes it, after the first bytes; a header of a file no signed program was known
        in is the bytes it always was; one without bit 63, or cut short, does not read."""
        V = bookread.VarintPut
        read = lambda body: tally.FileHeadDecode(7, body, [])
        signed = tally.FileState(7, 3, SIGNED, 5, 1, 2, b"game", [4], [], head=b"hello", passport=GAME)
        plain = tally.FileState(7, 3, ondisk.FILE_ACTIVE | ondisk.FILE_HEAD, 5, 1, 2, b"game", [4], [], head=b"hello")
        no_bit = tally.FileState(7, 3, SIGNED, 5, 1, 2, b"game", [4], [], head=b"hello", passport=0x1234)
        body = tally.FileHeadEncode(signed)
        return (body.endswith(V(5) + b"hello" + V(GAME)) and read(body).passport == GAME and
            read(tally.FileHeadEncode(plain)).passport == 0 and
            tally.FileHeadEncode(plain).endswith(V(5) + b"hello") and
            Refused(read, tally.FileHeadEncode(no_bit)) and Refused(read, body[:-1]))

    def RecordsRoundTrip(c):
        """A Born carries its passport last, a Labeled PASSPORT its new one, and both are written again byte
        for byte, the one of 0 too; a Labeled PASSPORT without bit 63 does not read."""
        d = stitch.RecordDecode(stitch.BORN, Signed().body, 1)
        label = stitch.Labeled(fid=fid, base=2, passport=NEXT)
        back = stitch.RecordDecode(stitch.LABELED, label.body, 9)
        off = stitch.Labeled(fid=fid, base=2, passport=0)
        none = stitch.RecordDecode(stitch.LABELED, off.body, 9)
        bad = stitch.Labeled(fid=fid, base=2, passport=0x77)
        return (d["passport"] == GAME and stitch.RecordAgain(stitch.BORN, d).body == Signed().body and
                back["passport"] == NEXT and stitch.RecordAgain(stitch.LABELED, back).body == label.body and
                none["passport"] == 0 and stitch.RecordAgain(stitch.LABELED, none).body == off.body and
                Refused(lambda body: stitch.RecordDecode(stitch.LABELED, body, 9), bad.body) and
                Refused(lambda body: stitch.RecordDecode(stitch.BORN, body, 1), Signed(0x99).body))

    return [
        Case("PP1 a Born carries the passport of the signed program the file holds", Born, checks=[
            ("the card holds it, its bit on", lambda c: Card(c, fid) == GAME),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("PP2 a change of what the file holds, and a rename, keep it: the program written over is itself",
             Updated, checks=[
                 ("the card still holds it", lambda c: Card(c, fid) == GAME and c.mount.files[fid].name == b"game-2.box"),
                 ("and nothing is said", lambda c: not c.findings)]),
        Case("PP3 a Labeled PASSPORT lays one anew", Anew, checks=[
            ("the card holds the new one", lambda c: Card(c, fid) == NEXT)]),
        Case("PP3b a Labeled PASSPORT of 0 takes it off, the bit with it", Off, checks=[
            ("the card holds none", lambda c: Card(c, fid) == 0),
            ("and nothing is said", lambda c: not c.findings)]),
        Case("PP4 a Labeled PASSPORT on a file none was known in puts the bit on", OnPlain, checks=[
            ("the card holds it", lambda c: Card(c, S.fid) == GAME)]),
        Case("PP5 the passport bit in a Labeled's FLAGS does not read", BitInFlags, checks=[
            ("the file's card is as it was", lambda c: Card(c, S.fid) == S.passport)]),
        Case("PP6 a FILES header carries the passport last", lambda o: None, checks=[
            ("read back, none for a file no signed program was known in, refused without bit 63 or cut short",
             HeaderRoundTrip)]),
        Case("PP7 a Born and a Labeled PASSPORT carry it, written again byte for byte", lambda o: None, checks=[
            ("a Born's last field, a Labeled's new one and none, refused without bit 63", RecordsRoundTrip)]),
    ]
