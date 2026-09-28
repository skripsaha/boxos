"""selftest — every census rule planted into an image and caught, the cut logic
proved on a synthetic log, and the format held against the C headers.

A rule nobody has seen fire is a rule that may not work. Each case here lays
one kind of damage over the image in memory (an OverlayMedium — the image
file is never written), runs the census on it, and holds the findings, and
the KernelView's roll call, against what that damage must produce. The image
has to be consistent to begin with (C0), because every case is measured as a
difference from it.

`--emit CASE --out IMG` writes one case's overlay out as a new image, for a
harness to boot: logcheck's `census` scenario boots `reconcile` and `C17`
and holds the kernel's own fsck lines against this package's prediction.
"""

import os
import re
import struct

from . import ondisk
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK
from .medium import OverlayMedium, Unreadable
from .census import GroundCensus
from .ondisk import RunsNormal, RunsSubtract
from . import blklog


class Targets:
    """What the cases aim at, chosen off C0's census.

    S — the live file with the fewest blocks (at least one), lowest fid first,
        that has exactly one record in the pool, carries a tag and a name, and
        is not the BOOT file: a file whose loss or retirement nothing else
        stands in for.
    L — the live file with the most blocks, lowest fid first.
    F — the lowest data block starting four free ones (C12 in `reconcile` sits
        at F+3)."""

    def __init__(self, c0):
        f = c0.f
        self.c0, self.f = c0, f
        self.base, self.lay = f.base, f.layout
        live = [e for e in c0.files.values() if e["state"] == "live"]
        counts = {}
        for r in f.records:
            counts[r.fid] = counts.get(r.fid, 0) + 1
        ok = [e for e in live if e["record"].Blocks() >= 1 and counts.get(e["fid"]) == 1 and
              e["record"].tags and e["record"].name and
              ondisk.RECORD_HEADER <= e["record"].len and
              "fid %u '%s'" % (e["fid"], e["record"].name) != c0.boot_file]
        if not ok or len(live) < 2:
            raise Unreadable("the image has no live file a case can aim at")
        self.S = min(ok, key=lambda e: (e["record"].Blocks(), e["fid"]))
        self.L = min((e for e in live if e is not self.S),
                     key=lambda e: (-e["record"].Blocks(), e["fid"]))
        free = RunsSubtract([(3, f.D - 3)], f.used)
        self.F = next(s for s, n in free if n >= 4)
        self.Srec, self.Lrec = self.S["record"], self.L["record"]
        self.Sruns = RunsNormal(self.Srec.extents_at_mount)
        blocks = [b for s, n in RunsNormal(self.Lrec.extents_at_mount) for b in range(s, s + n)]
        self.Lmid = blocks[len(blocks) // 2]

    def Label(self, e):
        return "fid %u '%s'" % (e["fid"], e["record"].name)

    # --- where things are on the medium ------------------------------------

    def VLba(self, vblock):
        return self.base + vblock * SECTORS_PER_BLOCK

    def DLba(self, b):
        return self.VLba(self.lay["data_block"] + b)

    def Block(self, over, b):
        return bytearray(over.sectors(self.DLba(b), SECTORS_PER_BLOCK))

    def PutBlock(self, over, b, raw):
        over.Put(self.DLba(b), bytes(raw))

    def Bit(self, over, b, on):
        at = self.VLba(self.lay["block_bitmap_block"]) * SECTOR + b // 8
        cur = over.sectors(at // SECTOR, 1)[at % SECTOR]
        cur = cur | (1 << b % 8) if on else cur & ~(1 << b % 8)
        over.Patch(at, bytes([cur]))

    def RecordPatch(self, over, rec, offset, data, fix_crc):
        """Change bytes of a record in place, and re-sum it if asked."""
        raw = self.Block(over, rec.block)
        raw[rec.at + offset:rec.at + offset + len(data)] = data
        if fix_crc:
            body = bytearray(raw[rec.at:rec.at + rec.len])
            body[ondisk.RECORD_CRC_AT:ondisk.RECORD_CRC_AT + 2] = b"\0\0"
            struct.pack_into("<H", raw, rec.at + ondisk.RECORD_CRC_AT, ondisk.Crc16(body))
        self.PutBlock(over, rec.block, raw)

    def Row(self, over, fid, place):
        link, i = divmod(fid, ondisk.FTABLE_PER_BLOCK)
        b = self.f.chains["table"][0][link]
        raw = self.Block(over, b)
        struct.pack_into("<II", raw, ondisk.CHAIN_HEADER + i * 8, *place)
        self.PutBlock(over, b, raw)

    def Ledger(self, over, copy, edit):
        """Rewrite a ledger copy through `edit(bytearray)` and re-sum it."""
        lba = self.VLba(self.lay["state_block"] + copy)
        raw = bytearray(over.sectors(lba, 1))
        edit(raw)
        n = struct.unpack_from("<I", raw, 8)[0]
        body = bytearray(raw[:n])
        body[ondisk.LEDGER_CRC_AT:ondisk.LEDGER_CRC_AT + 4] = b"\0\0\0\0"
        struct.pack_into("<I", raw, ondisk.LEDGER_CRC_AT, ondisk.Crc32(bytes(body)))
        over.Put(lba, bytes(raw))


# ---------------------------------------------------------------------------
# The plants
# ---------------------------------------------------------------------------


def PlantOrphan(t, over, b):
    t.Bit(over, b, True)


def PlantDouble(t, over):
    t.RecordPatch(over, t.Srec, ondisk.RECORD_HEADER + 2 * t.Srec.tag_count,
                  struct.pack("<I", t.Lrec.extents_at_mount[0][0]), True)


def PlantRegistrySplit(t, over, at):
    """The registry's last ten records moved to data block `at`, linked from
    the head: a registry chain whose continuation is NOT data block 0."""
    head = t.f.chains["registry"][0][0]
    raw = t.Block(over, head)
    blk = ondisk.ReadRegistryBlock(raw)
    recs = blk["records"]
    if len(recs) < 11:
        raise Unreadable("the registry has too few records to split")
    cut = recs[-10]["at"]
    moved = bytes(raw[cut:ondisk.CHAIN_HEADER + blk["used_bytes"]])
    struct.pack_into("<IIHH", raw, 0, ondisk.REGISTRY_MAGIC, at, len(recs) - 10,
                     cut - ondisk.CHAIN_HEADER)
    raw[cut:] = bytes(BLOCK - cut)
    t.PutBlock(over, head, raw)
    cont = bytearray(BLOCK)
    struct.pack_into("<IIHH", cont, 0, ondisk.REGISTRY_MAGIC, 0, 10, len(moved))
    cont[ondisk.CHAIN_HEADER:ondisk.CHAIN_HEADER + len(moved)] = moved
    t.PutBlock(over, at, cont)
    t.Bit(over, at, True)


def PlantLink(t, over, name, at):
    """What a chain grown link first leaves when the power goes before the new
    block is written (MP-D5, tag_registry.c and file_table.c flush): the
    chain's last block naming data block `at`, ground nothing ever stamped."""
    last = t.f.chains[name][0][-1]
    raw = t.Block(over, last)
    struct.pack_into("<I", raw, 4, at)
    t.PutBlock(over, last, raw)


def PlantRetire(t, over):
    """What meta_pool_delete leaves (meta_pool.c:1138-1240): the file_id zeroed
    and the CRC left alone."""
    t.RecordPatch(over, t.Srec, 2, b"\0\0\0\0", False)


def PlantMissing(t, over):
    t.Bit(over, t.Lmid, False)


def PlantUseChain(t, over, broken):
    text = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    crc = ondisk.Crc32(text)
    parts = [text[:ondisk.TUSE_TEXT], text[ondisk.TUSE_TEXT:]]
    for i, part in enumerate(parts):
        link = bytearray(BLOCK)
        index = 5 if broken and i == 1 else i
        nxt = t.F + 1 if i == 0 else 0
        link[:ondisk.TUSE_LINK.size] = ondisk.TUSE_LINK.struct.pack(
            ondisk.TUSE_MAGIC, nxt, 1, 1, index, len(part), 2, len(text), crc)
        link[ondisk.TUSE_LINK.size:ondisk.TUSE_LINK.size + len(part)] = part
        t.PutBlock(over, t.F + i, link)
        t.Bit(over, t.F + i, True)


def PlantSpare(t, over):
    spare = bytearray(BLOCK)
    struct.pack_into("<I", spare, 0, ondisk.MPOOL_MAGIC)
    t.PutBlock(over, t.F, spare)
    t.Bit(over, t.F, True)


def PlantBoot(t, over):
    lba = t.base
    raw = bytearray(over.sectors(lba, ondisk.DEED_SECTORS))
    at = ondisk.DeedStampAt(raw, ondisk.STAMP_BOOT)
    kb = struct.unpack_from("<I", raw, at)[0]
    struct.pack_into("<I", raw, at, kb + 1)
    over.Put(lba, ondisk.DeedSeal(raw))


def PlantHeadTorn(t, over):
    raw = bytearray(over.sectors(t.base, 1))
    pb = struct.unpack_from("<H", raw, 8)[0]
    raw[pb + 8] ^= 0xFF
    over.Put(t.base, bytes(raw))


def PlantNoDeed(t, over):
    over.Put(t.base, bytes(ondisk.DEED_SECTORS * SECTOR))
    over.Put(t.base + t.f.deed["tail_sector"], bytes(ondisk.DEED_SECTORS * SECTOR))


def PlantMbrEntry(t, over, kind=None, extra=0):
    """MBR entry 0, the volume's ground: its type byte, or its sector count
    `extra` sectors longer."""
    raw = bytearray(over.sectors(0, 1))
    if kind is not None:
        raw[446 + 4] = kind
    count = struct.unpack_from("<I", raw, 446 + 12)[0]
    struct.pack_into("<I", raw, 446 + 12, count + extra)
    over.Put(0, bytes(raw))


def PlantGpt(t, over, count=128, primary="good", backup="good", ends=None):
    """The volume's ground said again as a GPT (UEFI 2.10 §5.3): a protective
    MBR, the primary header at LBA 1 with its entries from LBA 2, and the far
    copy's header in the medium's last sector — the unused tail of the far
    deed's block — with its entries at sector 1024, before the ESP. One BoxOS
    entry, from the volume's first sector to `ends` (the medium's last by
    default). `primary` and `backup` are "good", "array" (the entry array
    written with one byte off its sum) or "gone" (no header at all)."""
    size = t.f.medium.SizeSectors()
    entry = bytearray(128)
    entry[0:16] = ondisk.BOXOS_TYPE_GUID
    entry[16:32] = bytes(range(1, 17))
    struct.pack_into("<QQ", entry, 32, t.base, size - 1 if ends is None else ends)
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


def PlantBitmapAway(t, over):
    """The head deed's layout moves the block bitmap past the medium's end,
    and the deed is sealed again: every other region stays where it is."""
    raw = bytearray(over.sectors(t.base, ondisk.DEED_SECTORS))
    at = ondisk.DeedStampAt(raw, ondisk.STAMP_LAYOUT)
    off = dict((f, o) for f, o, _s in ondisk.VOLUME_LAYOUT.Offsets())["block_bitmap_block"]
    struct.pack_into("<I", raw, at + off, t.lay["total_blocks"] + 64)
    over.Put(t.base, ondisk.DeedSeal(raw))


def ManyOrphans(t):
    """ORPHAN_RUNS single blocks with a free one between each: a roll call
    that must name more orphan runs than the kernel says one by one."""
    free = RunsSubtract([(3, t.f.D - 3)], t.f.used)
    s = next(s for s, n in free if n >= 2 * ORPHAN_RUNS)
    return [s + 2 * i for i in range(ORPHAN_RUNS)]


# ---------------------------------------------------------------------------
# The cases
# ---------------------------------------------------------------------------


def Findings(c, code):
    return [x for x in c.findings if x.code == code]


def Runs(c, code):
    return RunsNormal([r for x in Findings(c, code) for r in x.runs])


class Case:
    """A plant, the codes it must raise, the other damage or leak codes it may
    raise, and the facts about the census and the kernel that must hold."""

    def __init__(self, name, plant, must=(), may=(), checks=()):
        self.name, self.plant = name, plant
        self.must, self.may, self.checks = set(must), set(may), list(checks)


# More than the 32 runs of each kind the kernel's mount names one by one
# (tagfs.c FsckSayRuns); the rest it counts in one line.
ORPHAN_RUNS = 40
FSCK_RUNS_NAMED = 32


def Cases(t):
    c0, S, L, F = t.c0, t.S, t.L, t.F
    maxfid = max(e["fid"] for e in c0.files.values() if e["state"] == "live")
    Mruns = RunsNormal(c0.files[maxfid]["record"].extents_at_mount)
    stand = c0.f.ledger["copy"]
    other = 1 - stand
    maxtag = max(c0.by_id)
    seqs = [c["seq"] for c in c0.f.ledgers]
    tie_rule = 0 if seqs[0] >= seqs[1] else 1

    def K(c):
        return c.kernel.fsck or {}

    def Next(value):
        return lambda raw: struct.pack_into("<I", raw, 48, value)

    return [
        Case("C0 pristine", lambda o: None, checks=[
            ("the kernel mounts it", lambda c: c.kernel.refusal is None),
            ("the kernel stands on copy %u, the strictly newer or copy 0 on a tie" % tie_rule,
             lambda c: c.f.ledger["copy"] == tie_rule),
            ("its roll call finds nothing", lambda c: not K(c)["orphan_runs"] and not K(c)["missing_runs"])]),
        Case("C1 orphan", lambda o: PlantOrphan(t, o, F), must={"ORPHAN"}, checks=[
            ("the orphan is %u+1" % F, lambda c: Runs(c, "ORPHAN") == [(F, 1)]),
            ("the kernel names %u+1" % F, lambda c: K(c)["orphan_runs"] == [(F, 1)])]),
        Case("C2 double", lambda o: PlantDouble(t, o), must={"DOUBLE_OWNER", "ORPHAN"}, checks=[
            ("the double names %s and %s" % (t.Label(S), t.Label(L)),
             lambda c: any(t.Label(S) in x.text and t.Label(L) in x.text
                           for x in Findings(c, "DOUBLE_OWNER"))),
            ("the orphans are %s's old blocks" % t.Label(S),
             lambda c: Runs(c, "ORPHAN") == RunsNormal(t.Sruns[:1]))]),
        Case("C3 bad CRC", lambda o: t.RecordPatch(o, t.Srec, t.Srec.len - 1,
                                                   bytes([t.Block(o, t.Srec.block)[t.Srec.at + t.Srec.len - 1] ^ 0x5A]),
                                                   False),
             must={"RECORD_CRC", "FILE_LOST", "ORPHAN"}, checks=[
                 ("the kernel loses 1 file", lambda c: K(c)["lost"] == 1),
                 ("the orphans are %s's blocks" % t.Label(S), lambda c: Runs(c, "ORPHAN") == t.Sruns)]),
        Case("C4 fid past next", lambda o: [t.Ledger(o, i, Next(maxfid)) for i in (0, 1)],
             must={"FID_PAST_NEXT"}, checks=[
                 ("KERNEL_BLIND names fid %u's blocks" % maxfid,
                  lambda c: Runs(c, "KERNEL_BLIND") == Mruns),
                 ("the kernel's orphans are fid %u's blocks" % maxfid,
                  lambda c: K(c)["orphan_runs"] == Mruns)]),
        Case("C5 absent tag", lambda o: t.RecordPatch(o, t.Srec, ondisk.RECORD_HEADER,
                                                      struct.pack("<H", maxtag + 1), True),
             must={"TAG_ABSENT"}),
        Case("C6 torn ledger", lambda o: o.Patch(t.VLba(t.lay["state_block"] + other) * SECTOR + 50,
                                                 bytes([o.sectors(t.VLba(t.lay["state_block"] + other), 1)[50] ^ 0xFF])),
             must={"LEDGER_TORN"}, checks=[
                 ("the kernel stands on copy %u" % stand, lambda c: c.f.ledger["copy"] == stand)]),
        Case("C6b no ledger", lambda o: [o.Patch(t.VLba(t.lay["state_block"] + i) * SECTOR + 50,
                                                 bytes([o.sectors(t.VLba(t.lay["state_block"] + i), 1)[50] ^ 0xFF]))
                                         for i in (0, 1)],
             must={"LEDGER_NONE"}, checks=[
                 ("the kernel refuses at the ledger", lambda c: c.kernel.step == "ledger")]),
        Case("C7 TUSE", lambda o: PlantUseChain(t, o, False), must={"TUSE_UNNAMED"}, checks=[
            ("it reads 2 links, 5000 bytes, crc ok",
             lambda c: any("2 links, 5000 bytes, crc ok" in x.text for x in Findings(c, "TUSE_UNNAMED")))]),
        Case("C7b broken TUSE", lambda o: PlantUseChain(t, o, True), must={"TUSE_BROKEN"}),
        Case("C8 Spare", lambda o: PlantSpare(t, o), must={"SPARE_UNNAMED"}, checks=[
            ("it is a spare pool header", lambda c: any("spare pool" in x.text
                                                        for x in Findings(c, "SPARE_UNNAMED")))]),
        Case("C9 lost", lambda o: PlantRetire(t, o), must={"FILE_LOST", "ORPHAN"}, checks=[
            ("the kernel loses 1 file", lambda c: K(c)["lost"] == 1),
            ("the orphans are %s's blocks, named as a retired record's" % t.Label(S),
             lambda c: Runs(c, "ORPHAN") == t.Sruns and
             any("retired record" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C10 unnamed", lambda o: t.Row(o, S["fid"], (0, 0)), must={"UNNAMED_RECORD", "ORPHAN"},
             checks=[("UNNAMED_RECORD is a leak", lambda c: any(x.severity == "leak"
                                                               for x in Findings(c, "UNNAMED_RECORD"))),
                     ("the orphan is explained as its extents",
                      lambda c: any("unnamed record" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C11 BOOT", lambda o: PlantBoot(t, o), must={"BOOT_MISMATCH"}),
        Case("C12 registry chained", lambda o: PlantRegistrySplit(t, o, F), checks=[
            ("the registry chain is [%u, %u]" % (c0.f.chains["registry"][0][0], F),
             lambda c: c.f.chains["registry"][0] == [c0.f.chains["registry"][0][0], F]),
            ("the kernel's roll call reaches registry[1]", lambda c: not K(c)["orphan_runs"]),
            ("nothing is KERNEL_BLIND", lambda c: not Findings(c, "KERNEL_BLIND"))]),
        Case("C13 missing", lambda o: PlantMissing(t, o), must={"MISSING"}, checks=[
            ("it is a note: the kernel keeps %u" % t.Lmid,
             lambda c: [x.severity for x in Findings(c, "MISSING")] == ["note"]),
            ("the kernel names %u+1 missing" % t.Lmid, lambda c: K(c)["missing_runs"] == [(t.Lmid, 1)])]),
        Case("C14 retired is not bad", lambda o: [t.Row(o, S["fid"], (0, 0)), PlantRetire(t, o)] +
             [t.Bit(o, b, False) for s, n in t.Sruns for b in range(s, s + n)]),
        Case("C15 head torn", lambda o: PlantHeadTorn(t, o), must={"DEED_HEAD_BAD"}, checks=[
            ("it stands on the far copy", lambda c: c.f.stand.stood_on == "tail")]),
        Case("C17 many orphan runs", lambda o: [PlantOrphan(t, o, b) for b in ManyOrphans(t)],
             must={"ORPHAN"}, checks=[
                 ("the kernel names %u orphan runs of one block, past the %u it says one by one"
                  % (ORPHAN_RUNS, FSCK_RUNS_NAMED),
                  lambda c: K(c)["orphan_runs"] == [(b, 1) for b in ManyOrphans(t)]),
                 ("the census's orphans are the kernel's", lambda c: Runs(c, "ORPHAN") == K(c)["orphan_runs"])]),
        LinkCase(t, "C18", "table"),
        LinkCase(t, "C19", "registry"),
        Case("G1 MBR type", lambda o: PlantMbrEntry(t, o, kind=0x83), must={"GROUND_REFUSED"}, checks=[
            ("the kernel refuses at the ground", lambda c: c.kernel.step == "ground"),
            ("the refusal names the types it found", lambda c: "0:0x83" in c.kernel.refusal),
            ("check exits 1", lambda c: c.ExitCode() == 1)]),
        Case("G2 MBR past the end", lambda o: PlantMbrEntry(t, o, extra=1), must={"GROUND_REFUSED"}, checks=[
            ("the kernel does not use an entry past the medium",
             lambda c: c.kernel.step == "ground" and "not using it" in c.kernel.refusal)]),
        Case("G3 GPT", lambda o: PlantGpt(t, o), checks=[
            ("the kernel stands on GPT entry 0", lambda c: c.f.stand.kernels and
             c.f.stand.how == "GPT entry 0 (BoxOS type)" and c.kernel.refusal is None)]),
        Case("G4 GPT primary array", lambda o: PlantGpt(t, o, primary="array"), checks=[
            ("the kernel stands on the far copy's entry",
             lambda c: c.f.stand.kernels and "from the copy at the far end" in c.f.stand.how and
             c.kernel.refusal is None)]),
        Case("G5 GPT primary gone", lambda o: PlantGpt(t, o, primary="gone"), checks=[
            ("the kernel stands on the far copy's entry",
             lambda c: c.f.stand.kernels and "from the copy at the far end" in c.f.stand.how and
             c.kernel.refusal is None)]),
        Case("G6 GPT both arrays", lambda o: PlantGpt(t, o, primary="array", backup="array"),
             must={"GROUND_REFUSED"}, checks=[
                 ("the kernel reads neither copy", lambda c: c.kernel.step == "ground" and
                  "neither copy of its GPT" in c.kernel.refusal)]),
        Case("G7 GPT ends first", lambda o: PlantGpt(t, o, ends=t.base - 1), must={"GROUND_REFUSED"}, checks=[
            ("the kernel skips an entry that ends before it begins",
             lambda c: c.kernel.step == "ground" and "ends before it begins" in c.kernel.refusal)]),
        Case("G8 GPT of 1024", lambda o: PlantGpt(t, o, count=1024), checks=[
            ("the kernel reads a table of 1024 entries", lambda c: c.f.stand.kernels and
             c.kernel.refusal is None)]),
        Case("G9 bitmap away", lambda o: PlantBitmapAway(t, o), must={"BITMAP_UNREADABLE", "REGION_OVERLAP"},
             checks=[("the kernel refuses at the bitmap", lambda c: c.kernel.step == "bitmap")]),
        Case("reconcile", lambda o: Reconcile(t, o), must={"ORPHAN", "FILE_LOST", "MISSING"}, checks=[
            ("the census's orphans are the kernel's", lambda c: Runs(c, "ORPHAN") == K(c)["orphan_runs"]),
            ("the kernel's orphans are %u+2 and %s's blocks" % (F, t.Label(S)),
             lambda c: K(c)["orphan_runs"] == RunsNormal([(F, 2)] + t.Sruns)),
            ("the kernel names %u+1 missing" % t.Lmid, lambda c: K(c)["missing_runs"] == [(t.Lmid, 1)]),
            ("the kernel loses 1 file", lambda c: K(c)["lost"] == 1)]),
    ]


def LinkCase(t, label, name):
    """A chain whose last block links to free ground (PlantLink): the mount
    refuses at that chain, and says which block it found unstamped."""
    before = t.c0.f.chains[name][0]
    return Case("%s %s linked to ground" % (label, name), lambda o: PlantLink(t, o, name, t.F),
                must={"CHAIN_REFUSED"}, checks=[
                    ("the kernel refuses at the %s" % name, lambda c: c.kernel.step == name),
                    ("the refusal names data block %u, never stamped" % t.F,
                     lambda c: c.f.refused.get(name, "").startswith("block %u holds 0x" % t.F)),
                    ("the chain stops at its last stamped block, %u" % before[-1],
                     lambda c: c.f.chains[name][0] == before)])


def Reconcile(t, over):
    """C1 at F and F+1, C12 at F+3, C13 and C9 at once: what logcheck boots."""
    PlantOrphan(t, over, t.F)
    PlantOrphan(t, over, t.F + 1)
    PlantRegistrySplit(t, over, t.F + 3)
    PlantMissing(t, over)
    PlantRetire(t, over)


def Judge(case, census):
    """(ok, flagged codes, what went wrong)."""
    codes = {x.code for x in census.findings}
    bad = {x.code for x in census.findings if x.severity in ("damage", "leak")}
    wrong = []
    if not case.must <= codes:
        wrong.append("missing %s" % ", ".join(sorted(case.must - codes)))
    extra = bad - case.must - case.may
    if extra:
        wrong.append("unexpected %s" % ", ".join(sorted(extra)))
    for what, check in case.checks:
        try:
            held = check(census)
        except (KeyError, IndexError, TypeError):
            held = False
        if not held:
            wrong.append("not so: " + what)
    return not wrong, sorted(codes), wrong


# ---------------------------------------------------------------------------
# L1-L5: the cut logic, on a log built in memory
# ---------------------------------------------------------------------------


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


def LogCases(t, medium):
    led_lba = t.VLba(t.lay["state_block"] + 1)
    bm_sector = t.F // (SECTOR * 8)
    bm_lba = t.VLba(t.lay["block_bitmap_block"]) + bm_sector

    def LedgerAt(seq):
        raw = bytearray(medium.sectors(led_lba, 1))
        struct.pack_into("<Q", raw, 16, seq)
        body = bytearray(raw[:struct.unpack_from("<I", raw, 8)[0]])
        body[12:16] = b"\0\0\0\0"
        struct.pack_into("<I", raw, 12, ondisk.Crc32(bytes(body)))
        return bytes(raw)

    bits = bytearray(medium.sectors(bm_lba, 1))
    bits[(t.F % (SECTOR * 8)) // 8] |= 1 << (t.F % 8)
    w3 = bytes((0xA0 + s) for s in range(SECTORS_PER_BLOCK) for _ in range(SECTOR))
    w4 = b"\x5c" * BLOCK
    flush = (0, blklog.LOG_FLUSH, b"")
    entries = [(led_lba, 0, LedgerAt(100)), (bm_lba, 0, bytes(bits)), flush,
               (led_lba, 0, LedgerAt(101)), (t.DLba(t.F), 0, w3), flush,
               (t.DLba(t.F + 1), 0, w4)]
    size = medium.SizeSectors()
    log = blklog.BlkLog(SyntheticLog(entries, 2), size)
    kinds = [[x.kind for x in blklog.Classifier(log, medium).Classify(w)] for w in log.Writes()]
    ep1 = log.Epoch(1)
    keep1 = log.Cut(medium, 1, {1}, {})
    keep_all = log.Cut(medium, 1, {0, 1}, {})
    # Two writes to one sector inside ONE epoch: kept together, the later wins.
    twice = blklog.BlkLog(SyntheticLog([(led_lba, 0, LedgerAt(200)), (led_lba, 0, LedgerAt(201))], 2),
                          size).Cut(medium, 0, {0, 1}, {})
    torn = log.Cut(medium, 1, set(), {1: 0x0F})
    base_f = medium.sectors(t.DLba(t.F), SECTORS_PER_BLOCK)
    by_bits = log.Cut(medium, 1, blklog.ParseSubsetBits("0b01", ep1), {})
    by_keep = log.Cut(medium, 1, {0}, {})
    stop = blklog.BlkLog(SyntheticLog(entries[:3] + [(0, 0, b"")] + entries[3:], 2), size)
    zero_at = 3
    raw = bytearray(SyntheticLog(entries, 7))
    bad_flags = bytearray(raw)
    struct.pack_into("<Q", bad_flags, (1 + 2 + 2 + 1) * SECTOR + 16, 0x10)
    flagged = blklog.BlkLog(bytes(bad_flags), size)
    return [
        ("L1 count and kinds", [e["writes"] for e in log.EpochFacts()] == [2, 2, 1] and
         len(log.flush_seqs) == 2 and kinds == [["ledger"], ["bitmap"], ["ledger"], ["unowned"], ["unowned"]],
         "epochs %s, kinds %s" % ([e["writes"] for e in log.EpochFacts()], kinds)),
        ("L2 keep and order", keep1.sectors(led_lba, 1) == LedgerAt(100) and
         keep1.sectors(t.DLba(t.F), 8) == w3 and keep_all.sectors(led_lba, 1) == LedgerAt(101) and
         twice.sectors(led_lba, 1) == LedgerAt(201),
         "keep {1} leaves W0's ledger and applies W3; keep all leaves W2's; of two kept writes "
         "to one sector in one epoch the later lands"),
        ("L3 tear", torn.sectors(t.DLba(t.F), 8) == w3[:4 * SECTOR] + base_f[4 * SECTOR:],
         "W3 torn 0x0F lands its first four sectors only"),
        ("L4 subset bits", by_bits.over == by_keep.over, "subset-bits 0b01 is keep {0}"),
        ("L5 walk stops", any("past the super's count of 2" in n for n in log.notes) and
         stop.entries == zero_at and "zero header" in stop.stop and flagged.entries == 3 and
         "flags 0x10" in flagged.stop,
         "super 2 against 7 read; a zero header stops at %u entries; bad flags stop at %u"
         % (stop.entries, flagged.entries)),
    ]


# ---------------------------------------------------------------------------
# X1: the constants and the struct layouts, held against the headers
# ---------------------------------------------------------------------------

_DEFINE = re.compile(r"^\s*#define\s+(\w+)\s+(.+?)\s*(?://.*|/\*.*)?$", re.M)
_TYPES = {"uint8_t": 1, "char": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8}


def _Value(text, defines):
    text = text.strip()
    m = re.fullmatch(r"'(\\0|.)'", text)
    if m:
        return 0 if m.group(1) == "\\0" else ord(m.group(1))
    m = re.fullmatch(r"\(\s*1U?\s*<<\s*(\d+)\s*\)", text)
    if m:
        return 1 << int(m.group(1))
    m = re.fullmatch(r"\(\s*(\d+)[uU]?\s*\*\s*(\d+)[uU]?\s*\)", text)
    if m:
        return int(m.group(1)) * int(m.group(2))
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", text)
    if m:
        return int(m.group(1), 0)
    return defines.get(text)


def Defines(text):
    out = {}
    for name, value in _DEFINE.findall(text):
        v = _Value(value, out)
        if v is not None:
            out[name] = v
    return out


def Structs(text, defines):
    """{name: [(field, offset, size)]} for every packed typedef struct."""
    out, sizes = {}, dict(_TYPES)
    for body, name in re.findall(r"typedef struct (?:__attribute__\(\(packed\)\)|__packed)\s*\{(.*?)\}\s*(\w+);",
                                 text, re.S):
        fields, at = [], 0
        for typ, star, field, count in re.findall(
                r"^\s*(?:struct\s+)?(\w+)\s*(\*?)\s*(\w+)\s*(?:\[(\w+)\])?\s*;", body, re.M):
            size = 8 if star else sizes.get(typ)
            if size is None:
                break
            n = 1 if not count else (int(count, 0) if count[0].isdigit() else defines.get(count))
            if n is None:
                break
            fields.append((field, at, size * n))
            at += size * n
        out[name] = fields
        sizes[name] = at
    return out


def ConstantsCheck(src):
    inc, fs = os.path.join(src, "include"), os.path.join(src, "kernel", "tagfs")
    paths = {"magic": os.path.join(inc, "boxos_magic.h"), "deed": os.path.join(inc, "volume_deed.h"),
             "ledger": os.path.join(inc, "volume_ledger.h"), "pen": os.path.join(inc, "deed_pen.h"),
             "tagfs": os.path.join(fs, "tagfs.h"), "consts": os.path.join(fs, "tagfs_constants.h"),
             "cow": os.path.join(fs, "cow", "cow.h"), "book": os.path.join(fs, "disk_book", "disk_book.h"),
             "pool": os.path.join(fs, "metadata_pool", "meta_pool.c"),
             "boot": os.path.join(src, "boot", "uefi", "tagfs_boot.h"),
             "ground": os.path.join(src, "kernel", "core", "boardroom", "ground.c"),
             "groundh": os.path.join(src, "kernel", "core", "boardroom", "ground.h")}
    if not all(os.path.exists(p) for p in paths.values()):
        return None, "self-test needs the source tree beside the tool (src/ next to tools/)"
    text = {k: open(p, encoding="utf-8", errors="replace").read() for k, p in paths.items()}
    d = {}
    for k in text:
        d.update(Defines(text[k]))
    wrong, n = [], 0

    def Eq(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            wrong.append("%s: the header says %r and ondisk.py %r" % (name, want, have))

    magic = lambda p: bytes(d["%s_MAGIC_%u" % (p, i)] for i in range(8))
    Eq("VOLUME_DEED_MAGIC", ondisk.DEED_MAGIC, magic("VOLUME_DEED"))
    Eq("VOLUME_LEDGER_MAGIC", ondisk.LEDGER_MAGIC, magic("VOLUME_LEDGER"))
    for py, c in (("SECTOR", "VOLUME_DEED_SECTOR_BYTES"), ("DEED_ROLE_HEAD", "VOLUME_DEED_ROLE_HEAD"),
                  ("DEED_ROLE_TAIL", "VOLUME_DEED_ROLE_TAIL"), ("STAMP_GEOMETRY", "VOLUME_STAMP_GEOMETRY"),
                  ("STAMP_LAYOUT", "VOLUME_STAMP_LAYOUT"), ("STAMP_BORN", "VOLUME_STAMP_BORN"),
                  ("STAMP_BOOT", "VOLUME_STAMP_BOOT"), ("STAMP_PACKED", "VOLUME_STAMP_PACKED"),
                  ("DEED_SECTORS", "DEED_PEN_SECTORS"), ("LEDGER_COPIES", "VOLUME_LEDGER_COPIES"),
                  ("REGISTRY_MAGIC", "TAGFS_REGISTRY_MAGIC"), ("FILETBL_MAGIC", "TAGFS_FILETBL_MAGIC"),
                  ("MPOOL_MAGIC", "TAGFS_MPOOL_MAGIC"), ("BLOCK", "TAGFS_BLOCK_SIZE"),
                  ("REGISTRY_DATA", "TAGFS_REGISTRY_DATA_SIZE"), ("MPOOL_DATA", "TAGFS_MPOOL_DATA_SIZE"),
                  ("FTABLE_PER_BLOCK", "TAGFS_FTABLE_PER_BLOCK"), ("MAX_TAG_ID", "TAGFS_MAX_TAG_ID"),
                  ("FILE_ACTIVE", "TAGFS_FILE_ACTIVE"), ("FILE_TRASHED", "TAGFS_FILE_TRASHED"),
                  ("FILE_HIDDEN", "TAGFS_FILE_HIDDEN"), ("FILE_COW", "TAGFS_FILE_COW"),
                  ("TAG_FLAG_HAS_VALUE", "TAGFS_TAG_FLAG_HAS_VALUE"), ("TAG_FLAG_SYSTEM", "TAGFS_TAG_FLAG_SYSTEM"),
                  ("COW_MANIFEST_MAGIC", "COW_MANIFEST_MAGIC"), ("COW_MANIFEST_MAX", "COW_MANIFEST_MAX"),
                  ("DISKBOOK_MAGIC", "DISK_BOOK_MAGIC"), ("DISKBOOK_SB_MAGIC", "DISK_BOOK_SB_MAGIC"),
                  ("DISKBOOK_VERSION", "DISK_BOOK_VERSION"), ("DISKBOOK_ENTRY_BYTES", "DISK_BOOK_ENTRY_SIZE"),
                  ("DISKBOOK_SECTORS_PER_ENTRY", "DISK_BOOK_SECTORS_PER_ENTRY"),
                  ("DISKBOOK_CAPACITY", "DISK_BOOK_CAPACITY"), ("DISKBOOK_TYPE_REDIRECT", "DISK_BOOK_TYPE_REDIRECT"),
                  ("RECORD_HEADER", "RECORD_HEADER_SIZE"), ("MPOOL_BLOCK_HEADER", "MPOOL_BLOCK_HEADER"),
                  ("RECORD_CRC_AT", "RECORD_CRC_OFFSET"), ("MBR_TYPE_BOXOS", "GROUND_MBR_TYPE"),
                  ("MBR_TYPE_PROTECTIVE", "GROUND_MBR_TYPE_PROTECTIVE"),
                  ("GPT_ARRAY_MAX_BYTES", "GPT_ARRAY_MAX_BYTES"), ("GPT_BLOCK_MAX_BYTES", "GPT_BLOCK_MAX_BYTES"),
                  ("GROUND_MAX_PER_MEDIUM", "GROUND_MAX_PER_MEDIUM")):
        Eq(c, getattr(ondisk, py), d.get(c))
    guid = re.search(r"g_boxos_type_guid\[16\]\s*=\s*\{(.*?)\}", text["ground"], re.S)
    Eq("g_boxos_type_guid", ondisk.BOXOS_TYPE_GUID,
       bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", guid.group(1))) if guid else None)
    rec = dict((f, o) for f, o, _s in ondisk.META_RECORD.Offsets())
    for c, field in (("META_RECORD_LEN_OFF", "record_len"), ("META_RECORD_FILE_ID_OFF", "file_id"),
                     ("META_RECORD_FLAGS_OFF", "flags"), ("META_RECORD_SIZE_OFF", "size"),
                     ("META_RECORD_TAGCOUNT_OFF", "tag_count"), ("META_RECORD_EXTCOUNT_OFF", "extent_count"),
                     ("META_RECORD_NAMELEN_OFF", "name_len")):
        Eq(c, rec[field], d.get(c))
    Eq("META_RECORD_VARDATA_OFF", ondisk.META_RECORD.size, d.get("META_RECORD_VARDATA_OFF"))

    structs = {}
    for k in ("deed", "ledger", "tagfs", "cow", "book"):
        structs.update(Structs(text[k], d))
    for cname, fields in (("VolumeDeed", ondisk.VOLUME_DEED), ("VolumeGeometry", ondisk.VOLUME_GEOMETRY),
                          ("VolumeLayout", ondisk.VOLUME_LAYOUT), ("VolumeBoot", ondisk.VOLUME_BOOT),
                          ("VolumeBorn", ondisk.VOLUME_BORN), ("VolumePacked", ondisk.VOLUME_PACKED),
                          ("VolumeLedger", ondisk.VOLUME_LEDGER), ("TagRegistryBlock", ondisk.REGISTRY_BLOCK),
                          ("FileTableBlock", ondisk.FILE_TABLE_BLOCK), ("FileTableEntry", ondisk.FILE_TABLE_ENTRY),
                          ("MetaPoolBlock", ondisk.META_POOL_BLOCK), ("FileExtent", ondisk.FILE_EXTENT),
                          ("CowSnapshotDisk", ondisk.COW_SNAPSHOT_DISK),
                          ("DiskBookSuperblock", ondisk.DISKBOOK_SUPERBLOCK),
                          ("DiskBookEntry", ondisk.DISKBOOK_ENTRY)):
        c_fields = {f: (o, s) for f, o, s in structs.get(cname, [])}
        for field, off, size in fields.Offsets():
            Eq("%s.%s" % (cname, field), (off, size), c_fields.get(field))
    ledger = {f: o for f, o, _s in structs.get("VolumeLedger", [])}
    Eq("VOLUME_LEDGER_REQUIRED_BYTES", ondisk.LEDGER_REQUIRED_BYTES, ledger.get("use_context_offset"))
    return wrong, n


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------


def Start(medium):
    try:
        c0 = GroundCensus(medium)
    except (Unreadable, IOError) as e:
        print("self-test: the image handed to self-test is unreadable — %s" % e)
        return None
    if c0.Verdict() != "consistent":
        print("self-test: the image handed to self-test is not consistent to begin with:")
        for line in c0.HumanLines():
            print("self-test:   " + line)
        return None
    return Targets(c0)


def Run(medium, src):
    t = Start(medium)
    if t is None:
        return 2
    print("self-test: aiming at S = %s (%u block(s)), L = %s (%u), F = data block %u"
          % (t.Label(t.S), t.Srec.Blocks(), t.Label(t.L), t.Lrec.Blocks(), t.F))
    passed = total = 0
    for case in Cases(t):
        over = OverlayMedium(medium)
        case.plant(over)
        ok, codes, wrong = Judge(case, GroundCensus(over))
        total += 1
        passed += ok
        print("self-test: %s — flagged %s — %s" % (case.name, ", ".join(codes) or "nothing",
                                                   "as expected" if ok else "; ".join(wrong)))
    over = OverlayMedium(medium)
    PlantNoDeed(t, over)
    try:
        GroundCensus(over)
        ok, said = False, "the census read a volume with no deed at either end"
    except Unreadable as e:
        ok, said = True, "unreadable: %s" % e
    total += 1
    passed += ok
    print("self-test: C16 no deed — %s — %s" % (said, "as expected" if ok else "expected unreadable"))
    for name, ok, said in LogCases(t, medium):
        total += 1
        passed += ok
        print("self-test: %s — %s — %s" % (name, said, "as expected" if ok else "NOT so"))
    wrong, n = ConstantsCheck(src)
    if wrong is None:
        print("self-test: X1 constants — %s" % n)
        return 2
    total += 1
    passed += not wrong
    print("self-test: X1 constants — %u constants and struct fields held against the headers — %s"
          % (n, "as expected" if not wrong else "; ".join(wrong)))
    print("self-test: %u of %u cases answered as expected" % (passed, total))
    return 0 if passed == total else 1


def Emit(medium, device, name, out):
    t = Start(medium)
    if t is None:
        return 2
    wanted = [c for c in Cases(t) if c.name == name or c.name.split()[0] == name]
    if not wanted:
        print("self-test: no case %r; the cases are %s" % (name, ", ".join(
            c.name.split()[0] for c in Cases(t))))
        return 2
    over = OverlayMedium(medium)
    wanted[0].plant(over)
    try:
        over.WriteOut(out)
    except ValueError as e:
        print("self-test: %s" % e)
        return 2
    print("self-test: %s written to %s — %u sector(s) differ from %s"
          % (wanted[0].name, out, len(over.over), device))
    return 0
