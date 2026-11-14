"""selftest_book_medium — the medium under a book: the ground the kernel finds
on it, the sectors it will not give, and a record of what a machine wrote to it.

The partition table said again (G1-G8): the kernel's own survey of the medium
(medium.KernelGround, ground.c) refuses the ground or stands on it, and the
book's census says which, in the kernel's words. A sector the medium answers
it cannot read — UNC, MEDIUM ERROR: what the medium holds, not a refusal — is
judged by where it lies (U1-U4), as the kernel judges a sector of the book it
reads as BOOK_SECTOR_UNREAD (mount/book_unit.c). A volume with no Deed at
either end is no volume (C16). And the cut logic (L1-L5): a blklogwrites log
built in memory over the book, its epochs counted, its writes named, and the
states a cut leaves built from it — the reader every PowerCut stands on.
"""

import struct

from . import blklog, ondisk, stitch, tally
from .bookcensus import BookCensus
from .medium import Unreadable
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK
from .selftest_book import Born, Case, Found, Label


# ---------------------------------------------------------------------------
# The plants
# ---------------------------------------------------------------------------


def PlantMbrEntry(over, kind=None, extra=0):
    """MBR entry 0, the volume's ground: its type byte, or its sector count
    `extra` sectors longer."""
    raw = bytearray(over.sectors(0, 1))
    if kind is not None:
        raw[446 + 4] = kind
    count = struct.unpack_from("<I", raw, 446 + 12)[0]
    struct.pack_into("<I", raw, 446 + 12, count + extra)
    over.Put(0, bytes(raw))


def PlantGpt(over, start, count=128, primary="good", backup="good", ends=None):
    """The volume's ground, from sector `start`, said again as a GPT (UEFI 2.10
    §5.3): a protective MBR, the primary header at LBA 1 with its entries from
    LBA 2, and the far copy's header in the medium's last sector — the unused
    tail of the far deed's block — with its entries at sector 1024, before the
    ESP. One BoxOS entry, from `start` to `ends` (the medium's last sector by
    default). `primary` and `backup` are "good", "array" (the entry array
    written with one byte off its sum) or "gone" (no header at all)."""
    size = over.SizeSectors()
    entry = bytearray(128)
    entry[0:16] = ondisk.BOXOS_TYPE_GUID
    entry[16:32] = bytes(range(1, 17))
    struct.pack_into("<QQ", entry, 32, start, size - 1 if ends is None else ends)
    array = bytes(entry) + bytes(128 * (count - 1))
    mbr = bytearray(over.sectors(0, 1))
    mbr[446:510] = bytes(64)
    mbr[446 + 4] = ondisk.MBR_TYPE_PROTECTIVE
    struct.pack_into("<II", mbr, 446 + 8, 1, min(size - 1, 0xFFFFFFFF))
    over.Put(0, bytes(mbr))
    for state, lba, alt, at in ((primary, 1, size - 1, 2), (backup, size - 1, 1, 1024)):
        if state == "gone":
            over.Put(lba, bytes(SECTOR))
            continue
        hdr = bytearray(SECTOR)
        struct.pack_into("<8sIII4xQQQQ16sQIII", hdr, 0, b"EFI PART", 0x00010000, 92, 0, lba, alt,
                         34, size - 34, bytes(range(16, 32)), at, count, 128, ondisk.Crc32(array))
        struct.pack_into("<I", hdr, 16, ondisk.Crc32(bytes(hdr[:92])))
        over.Put(lba, bytes(hdr))
        body = bytearray(array) if state == "good" else bytearray([array[0] ^ 1]) + array[1:]
        over.Put(at, bytes(body.ljust((len(body) + SECTOR - 1) // SECTOR * SECTOR, b"\0")))


def SyntheticLog(entries, super_entries):
    """A blklogwrites log (blklogwrites.c:38-50, 497-509): the super, then per
    entry a header sector followed by its data sectors."""
    out = bytearray(blklog.LOG_SUPER.pack(blklog.LOG_MAGIC, blklog.LOG_VERSION,
                                          super_entries, SECTOR).ljust(SECTOR, b"\0"))
    for lba, flags, data in entries:
        n = len(data) // SECTOR
        out += blklog.LOG_ENTRY.pack(lba, n if flags != blklog.LOG_FLUSH else 0, flags, 0).ljust(SECTOR, b"\0")
        out += data
    return bytes(out)


# ---------------------------------------------------------------------------
# G1-G8: the ground the kernel finds
# ---------------------------------------------------------------------------


def GroundCases(bt):
    start = bt.ground.base

    def Refused(*words):
        return ("the kernel refuses at the ground, saying %s, and the census with it" % " and ".join(words),
                lambda c: not c.g.stand.kernels and all(w in c.g.stand.kernel_why for w in words) and
                Found(c, "GROUND_REFUSED", *words) and c.kernel["mounts"] is False)

    def Stands(how, exact=False):
        return ("the kernel stands on %s and mounts the book" % how,
                lambda c: c.g.stand.kernels and (c.g.stand.how == how if exact else how in c.g.stand.how) and
                c.kernel["mounts"] is True)

    return [
        Case("G1 MBR type", lambda o: PlantMbrEntry(o, kind=0x83), must={"GROUND_REFUSED"}, checks=[
            Refused("0:0x83"), ("check exits 1", lambda c: c.ExitCode() == 1)]),
        Case("G2 MBR past the end", lambda o: PlantMbrEntry(o, extra=1), must={"GROUND_REFUSED"},
             checks=[Refused("not using it")]),
        Case("G3 GPT", lambda o: PlantGpt(o, start), checks=[Stands("GPT entry 0 (BoxOS type)", exact=True)]),
        Case("G4 GPT primary array", lambda o: PlantGpt(o, start, primary="array"),
             checks=[Stands("from the copy at the far end")]),
        Case("G5 GPT primary gone", lambda o: PlantGpt(o, start, primary="gone"),
             checks=[Stands("from the copy at the far end")]),
        Case("G6 GPT both arrays", lambda o: PlantGpt(o, start, primary="array", backup="array"),
             must={"GROUND_REFUSED"}, checks=[Refused("neither copy of its GPT")]),
        Case("G7 GPT ends first", lambda o: PlantGpt(o, start, ends=start - 1), must={"GROUND_REFUSED"},
             checks=[Refused("ends before it begins")]),
        Case("G8 GPT of 1024", lambda o: PlantGpt(o, start, count=1024),
             checks=[Stands("GPT entry 0 (BoxOS type)", exact=True)]),
    ]


# ---------------------------------------------------------------------------
# U1-U4: a sector the medium cannot read, judged by where it lies
# ---------------------------------------------------------------------------


def UnreadCases(bt):
    g, S, leaf = bt.ground, bt.S, bt.Node(tally.FILES)
    run, f1, f2 = S.runs[0], bt.fid, bt.fid + 1
    sewn = {}

    def Bad(o, vlba):
        o.Bad(g.base + vlba, 1)

    def Stitched(o):
        """Stitch 3 — a Use Context over more than a sector and f1 born — and
        stitch 4 after it, born f2: stitch 3's sector 1 will not read."""
        d = bt.Fresh(o, 0, 2, 4)
        pen = bt.Pen(o)
        s3 = pen.Sew([stitch.UseContext(b"x" * 1000), Born(bt.F, f1, d[:1], b"kept")], flags=stitch.DATA_FLUSHED)
        pen.Sew([Born(bt.F + 1, f2, d[1:], b"after")], flags=stitch.DATA_FLUSHED)
        Bad(o, s3.vlbas[1])
        sewn.update(s3=s3)

    return [
        Case("U1 a sector of a FILES node's first copy will not read",
             lambda o: Bad(o, leaf.blocks[0] * SECTORS_PER_BLOCK), must={"NODE_COPY_ROTTEN"}, checks=[
                 ("copy 1 is named, its sector 0 one that will not read, and copy 2 is read",
                  lambda c: Found(c, "NODE_COPY_ROTTEN", "FILES", "copy 1", "will not read")),
                 ("every file reads", lambda c: all(c.Sha256(fid) == bt.c0.Sha256(fid) for fid in bt.content.files)),
                 ("and the kernel of this tree mounts it, the copy read in the other's place",
                  lambda c: c.kernel["mounts"] is True and c.kernel["mended"] == 1)]),
        Case("U2 a sector of the one slot that holds will not read",
             lambda o: Bad(o, tally.SlotBlock(g, 0) * SECTORS_PER_BLOCK), must={"SPINE_NONE"}, checks=[
                 ("no slot holds: slot 0's sector 0 will not read, and the book is not read",
                  lambda c: Found(c, "SPINE_NONE", "slot 0", "will not read")),
                 ("and the kernel of this tree refuses it, no slot of its Spine holding",
                  lambda c: c.kernel["mounts"] is False and "slots of its Spine holds" in c.kernel["refusal"])]),
        Case("U3 a sector of a settled stitch will not read", Stitched, must={"STITCH_TORN"}, checks=[
            ("stitch 3 is closed and damaged, its sector 1 one that will not read, its lost bytes counted",
             lambda c: Found(c, "STITCH_TORN", "stitch 3 ", "sector(s) 1 of", "will not read",
                             "%u byte" % sewn["s3"].LostAt(1))),
            ("the record wholly in its other sectors stands: %u is born, and so is %u" % (f1, f2),
             lambda c: f1 in c.mount.files and f2 in c.mount.files),
            ("the Use Context that crossed the sector is lost", lambda c: c.mount.use == b"")]),
        Case("U4 a sector of a file's block will not read", lambda o: Bad(o, run.start * SECTORS_PER_BLOCK + 3),
             must={"DIGEST_MISMATCH"}, checks=[
                 ("%s's block %u is named, refused to a read" % (Label(S), run.start),
                  lambda c: Found(c, "DIGEST_MISMATCH", Label(S), "%u+1" % run.start)),
                 ("its bytes are not given", lambda c: c.Content(S.fid) is None),
                 ("and the book mounts with every file", lambda c: c.kernel["mounts"] is True and
                  set(c.mount.files) == set(bt.content.files))]),
    ]


# ---------------------------------------------------------------------------
# C16: no deed at either end
# ---------------------------------------------------------------------------


def NoDeed(bt):
    """(name, ok, said): both copies of the Deed zeroed — no ground the kernel
    could stand on, and the census reads no volume."""
    g, o = bt.ground, bt.Fork()
    o.Put(g.base, bytes(ondisk.DEED_SECTORS * SECTOR))
    o.Put(g.base + g.deed["tail_sector"], bytes(ondisk.DEED_SECTORS * SECTOR))
    try:
        BookCensus(o)
        return "C16 no deed", False, "the census read a volume with no deed at either end"
    except Unreadable as e:
        return "C16 no deed", True, "unreadable: %s" % e


# ---------------------------------------------------------------------------
# L1-L5: the cut logic, on a log built in memory over the book
# ---------------------------------------------------------------------------


def LogCases(bt):
    """(name, ok, said) of each: a front-matter sector written twice in two
    epochs, the head Deed written as it lies, the first free block of the
    landings' room and the one after it written as data no stitch names, and
    a sector of the loader's code outside every partition."""
    medium, g = bt.base, bt.ground
    front = g.base + SECTORS_PER_BLOCK                  # block 1: the front matter, below the Spine
    data = g.base + bt.F * SECTORS_PER_BLOCK

    def At(seq):
        return struct.pack("<Q", seq).ljust(SECTOR, b"\xf5")

    w3 = bytes((0xA0 + s) for s in range(SECTORS_PER_BLOCK) for _ in range(SECTOR))
    w4 = b"\x5c" * BLOCK
    flush = (0, blklog.LOG_FLUSH, b"")
    entries = [(front, 0, At(100)), (g.base, 0, medium.sectors(g.base, 1)), flush,
               (front, 0, At(101)), (data, 0, w3), flush,
               (data + SECTORS_PER_BLOCK, 0, w4), (1, 0, b"\x44" * SECTOR)]
    size = medium.SizeSectors()
    log = blklog.BlkLog(SyntheticLog(entries, 2), size)
    kinds = [[x.kind for x in blklog.Classifier(log, medium).Classify(w)] for w in log.Writes()]
    ep1 = log.Epoch(1)
    keep1 = log.Cut(medium, 1, {1}, {})
    keep_all = log.Cut(medium, 1, {0, 1}, {})
    # Two writes to one sector inside ONE epoch: kept together, the later wins.
    twice = blklog.BlkLog(SyntheticLog([(front, 0, At(200)), (front, 0, At(201))], 2), size).Cut(medium, 0, {0, 1}, {})
    torn = log.Cut(medium, 1, set(), {1: 0x0F})
    base_f = medium.sectors(data, SECTORS_PER_BLOCK)
    by_bits = log.Cut(medium, 1, blklog.ParseSubsetBits("0b01", ep1), {})
    by_keep = log.Cut(medium, 1, {0}, {})
    stop = blklog.BlkLog(SyntheticLog(entries[:3] + [(0, 0, b"")] + entries[3:], 2), size)
    bad_flags = bytearray(SyntheticLog(entries, 8))
    struct.pack_into("<Q", bad_flags, (1 + 2 + 2 + 1) * SECTOR + 16, 0x10)   # the fourth entry's flags
    flagged = blklog.BlkLog(bytes(bad_flags), size)
    want = [["front"], ["deed"], ["front"], ["free"], ["free"], ["outside"]]
    return [
        ("L1 count and kinds", [e["writes"] for e in log.EpochFacts()] == [2, 2, 2] and
         len(log.flush_seqs) == 2 and kinds == want,
         "epochs %s, kinds %s" % ([e["writes"] for e in log.EpochFacts()], kinds)),
        ("L2 keep and order", keep1.sectors(front, 1) == At(100) and
         keep1.sectors(data, SECTORS_PER_BLOCK) == w3 and keep_all.sectors(front, 1) == At(101) and
         twice.sectors(front, 1) == At(201),
         "keep {1} leaves W0's front sector and applies W3; keep all leaves W2's; of two kept writes "
         "to one sector in one epoch the later lands"),
        ("L3 tear", torn.sectors(data, SECTORS_PER_BLOCK) == w3[:4 * SECTOR] + base_f[4 * SECTOR:],
         "W3 torn 0x0F lands its first four sectors only"),
        ("L4 subset bits", by_bits.over == by_keep.over, "subset-bits 0b01 is keep {0}"),
        ("L5 walk stops", any("past the super's count of 2" in n for n in log.notes) and
         stop.entries == 3 and "zero header" in stop.stop and flagged.entries == 3 and
         "flags 0x10" in flagged.stop,
         "super 2 against 8 read; a zero header stops at %u entries; bad flags stop at %u"
         % (stop.entries, flagged.entries)),
    ]
