"""selftest_cases — every case of volume-read's self-test on a format-v1 image:
a plant (selftest_plants.py), the codes it must raise and the facts about the
census and the mount that must hold; the Deeds a reader of v1 must refuse; the
cut logic on a log built in memory; and the run of them over the image
(Start, V1Cases), which selftest.py asks of a format-v1 image alone."""

import struct

from . import ondisk
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK, RunsNormal, RunsSubtract
from .medium import OverlayMedium, Unreadable
from .census import GroundCensus
from .selftest_book import Case, Judge
from . import blklog
from .selftest_book_medium import PlantGpt, PlantMbrEntry, SyntheticLog
from .selftest_plants import (LEDGER_AT, LastRecord, LedgerField, NoSeal, PlantAppend, PlantBadSector,
                              PlantBitmapAway, PlantBoot, PlantFellBack, PlantFrame, PlantGlean,
                              PlantHeadTorn, PlantLink, PlantManifest, PlantMissing,
                              PlantOlderClosed, PlantOrphan, PlantQuitclaimLanded, PlantRefusedRead,
                              PlantRegistryAppend, PlantRegistrySplit, PlantRetire, PlantRewritten,
                              PlantRotten, PlantRottenSettled, PlantSpare, PlantTornTail, PlantUseChain,
                              Quitclaim, Reconcile, RecordOf, Reseal, PlantNoDeed, Targets)


def FrameCases(t, medium):
    """(name, ok, said) for each Deed whose format a reader of v1 must not
    take for its own: each is unreadable to the census, and says the reason
    the kernel says at its format check (deed.c, DeedFormatRefused)."""
    out = []
    for name, plant, words in (
            ("F1 both formats", lambda o: PlantFrame(t, o, keep_layout=True),
             ("claims both formats",)),
            ("F2 FRAME with overlapping slots", lambda o: PlantFrame(t, o, spine_stride=0),
             ("format v2", "does not hold", "all one block")),
            ("F3 FRAME longer than 24", lambda o: PlantFrame(t, o, frame_bytes=32),
             ("format v2", "no book is written", "knows more")),
            ("F4 FRAME with a ring of four", lambda o: PlantFrame(t, o, spine_slots=4, data_block=256 * 5),
             ("format v2", "no book is written", "another maker"))):
        over = OverlayMedium(medium)
        plant(over)
        try:
            GroundCensus(over)
            out.append((name, False, "the census read it as a volume of format v1"))
        except Unreadable as e:
            out.append((name, all(w in str(e) for w in words), "unreadable: %s" % e))

    # A cut of a volume with no LAYOUT: the classifier places each write by
    # the FRAME — the Deed, a Spine slot, the front matter, the data run — and
    # one outside the ground stays outside, where reading v1's regions off a
    # Deed that has none fell over on the first write into the ground. A
    # sector that is no unit of the book is placed by where it lies; in the
    # data run, with no stitch naming it and no book to ask, it is no one's.
    over = OverlayMedium(medium)
    PlantFrame(t, over)
    grain = 256
    entries = [(t.VLba(0), 0, over.sectors(t.VLba(0), SECTORS_PER_BLOCK)),
               (t.VLba(2 * grain), 0, b"\x11" * BLOCK),
               (t.VLba(grain + 1), 0, b"\x22" * BLOCK),
               (t.VLba((1 + ondisk.FRAME_SPINE_SLOTS) * grain + 7), 0, b"\x33" * BLOCK),
               (1, 0, b"\x44" * SECTOR)]
    log = blklog.BlkLog(SyntheticLog(entries, len(entries)), over.SizeSectors())
    named = [[(x.kind, x.what) for x in blklog.Classifier(log, over).Classify(w)]
             for w in log.Writes()]
    want = [("deed", "head deed"), ("slot", "Spine slot 1"), ("front", "front matter"),
            ("unowned", "data run block"), ("outside", "boot code")]
    ok = len(named) == len(want) and all(
        len(n) == 1 and n[0][0] == kind and what in n[0][1] for n, (kind, what) in zip(named, want))
    out.append(("F5 a cut of a format-v2 volume", ok,
                "; ".join("%s %s" % n[0] if n else "nothing" for n in named)))
    return out


# ---------------------------------------------------------------------------
# The cases
# ---------------------------------------------------------------------------


def Findings(c, code):
    return [x for x in c.findings if x.code == code]


def Runs(c, code):
    return RunsNormal([r for x in Findings(c, code) for r in x.runs])


def Kinds(c):
    return {x.kind for x in c.claims}


# More than the 32 runs of each kind the kernel's mount names one by one
# (block_map_mount.c, MapTallySay); the rest it counts in one line.
ORPHAN_RUNS = 40
FSCK_RUNS_NAMED = 32


def ManyOrphans(t):
    """ORPHAN_RUNS single blocks with a free one between each: a roll call
    that must name more orphan runs than the kernel says one by one."""
    free = RunsSubtract([(3, t.f.D - 3)], t.f.used)
    s = next(s for s, n in free if n >= 2 * ORPHAN_RUNS)
    return [s + 2 * i for i in range(ORPHAN_RUNS)]


def K(c):
    return c.kernel.fsck or {}


def M(c):
    return c.f.mount


def Seal(c):
    return c.kernel.Seal()


def State(c, fid):
    return c.files.get(fid, {}).get("state")


def Stood(stood, copy, fell_back=False):
    return lambda c: (Seal(c)["stood"], Seal(c)["copy"], Seal(c)["fell_back"]) == (stood, copy, fell_back)


def Lost(n):
    return ("the kernel loses %u file(s) and lets no row go unsaid" % n,
            lambda c: K(c)["lost"] == n and K(c)["unsettled"] == 0)


def Lied(lied):
    return ("the mount %s the medium SETTLE_MEDIUM_LIED" % ("marks" if lied else "does not mark"),
            lambda c: Seal(c)["lied"] == lied)


class CaseFacts:
    """What every case of format v1 is measured from, read once off the
    pristine census: the targets, the copy the volume stands on and the one
    a tie gives, the chains, the rows, the records a case breaks."""

    def __init__(self, t):
        c0 = self.c0 = t.c0
        self.S, self.U, self.L, self.F = t.S, t.U, t.L, t.F
        self.Sfid, self.Ufid, self.maxfid = t.S["fid"], t.U["fid"], t.maxfid
        self.stand = c0.f.ledger["copy"]
        self.other = 1 - self.stand
        self.maxtag = max(c0.by_id)
        seqs = [c["seq"] for c in c0.f.ledgers]
        self.tie_rule = 0 if seqs[0] >= seqs[1] else 1
        self.pool0, self.registry0, self.table0 = (c0.f.chains[k] for k in ("pool", "registry", "table"))
        self.rows0 = len(c0.f.rows)
        self.tail_runs = RunsNormal([r for fid in t.tail_files
                                     for r in c0.files[fid]["record"].extents_at_mount])
        self.older = LastRecord(t, self.pool0[0])
        self.broken = LastRecord(t, t.Srec.block)
        self.broken_tail = (ondisk.PoolUsed(t.Block(OverlayMedium(c0.f.medium), self.broken.block)) -
                            (self.broken.at - ondisk.CHAIN_HEADER))


def Cases(t):
    x = CaseFacts(t)
    return (LedgerCases(t, x) + RowCases(t, x) + PoolCases(t, x) + SealCases(t, x) + ReadCases(t, x) +
            GroundCases(t))



def LedgerCases(t, x):
    """The ground every later case is measured from, the Ledger's copies and
    the Use Context's chain: C0-C8."""
    F, L, S, maxfid = x.F, x.L, x.S, x.maxfid
    maxtag, other, stand, tie_rule = x.maxtag, x.other, x.stand, x.tie_rule
    return [
        # --- the ground every later case is measured from ---------------------
        Case("C0 pristine", lambda o: None, checks=[
            ("the kernel mounts it", lambda c: c.kernel.refusal is None),
            ("the kernel stands on copy %u, the strictly newer or copy 0 on a tie" % tie_rule,
             lambda c: c.f.ledger["copy"] == tie_rule),
            ("the pool stands on the newest seal, borne out", Stood("seal", tie_rule)),
            ("its roll call finds nothing", lambda c: not K(c)["orphan_runs"] and not K(c)["missing_runs"]),
            ("the next file id is past every id shown, %u" % (maxfid + 1),
             lambda c: M(c).next_file_id == maxfid + 1),
            ("the registry, the table, the pool, the files, the integrity map and BOOT are heard",
             lambda c: {"registry", "table", "pool", "file", "integrity", "boot"} <= Kinds(c))]),
        Case("C1 orphan", lambda o: PlantOrphan(t, o, F), must={"ORPHAN"}, checks=[
            ("the orphan is %u+1" % F, lambda c: Runs(c, "ORPHAN") == [(F, 1)]),
            ("the kernel names %u+1" % F, lambda c: K(c)["orphan_runs"] == [(F, 1)])]),
        Case("C2 double", lambda o: PlantRewritten(t, o, t.Srec, block=t.Lrec.extents_at_mount[0][0]),
             must={"DOUBLE_OWNER", "ORPHAN"}, checks=[
                 ("the double names %s and %s" % (t.Label(S), t.Label(L)),
                  lambda c: any(t.Label(S) in x.text and t.Label(L) in x.text
                                for x in Findings(c, "DOUBLE_OWNER"))),
                 ("the orphans are %s's old blocks" % t.Label(S),
                  lambda c: Runs(c, "ORPHAN") == RunsNormal(t.Sruns[:1])),
                 ("the kernel corrects %s's row to its newest record" % t.Label(S),
                  lambda c: K(c)["corrected"] == 1)]),
        Case("C3 rot in a closed block", lambda o: t.RecordPatch(
                 o, t.Srec, t.Srec.len - 1,
                 bytes([t.Block(o, t.Srec.block)[t.Srec.at + t.Srec.len - 1] ^ 0x5A]), False),
             must={"RECORD_CRC", "FILE_LOST", "ORPHAN"}, checks=[
                 ("no seal is borne out, so the chain is read whole", Stood("none", tie_rule)),
                 Lost(1),
                 ("the walk passes 1 record over", lambda c: K(c)["unreadable"] == 1),
                 ("the orphans are %s's blocks" % t.Label(S), lambda c: Runs(c, "ORPHAN") == t.Sruns)]),
        Case("C4 next file id derived", lambda o: [t.Ledger(o, i, LedgerField("next_file_id", maxfid))
                                                   for i in (0, 1)], checks=[
            ("the Ledger says %u and the rows reach %u: the mount gives %u" % (maxfid, maxfid, maxfid + 1),
             lambda c: M(c).next_file_id == maxfid + 1)]),
        Case("C4b next file id kept", lambda o: [t.Ledger(o, i, LedgerField("next_file_id", maxfid + 10))
                                                for i in (0, 1)], checks=[
            ("the Ledger's %u is past every id shown and stays" % (maxfid + 10),
             lambda c: M(c).next_file_id == maxfid + 10)]),
        Case("C5 absent tag", lambda o: PlantRewritten(t, o, t.Srec, tag=maxtag + 1),
             must={"TAG_ABSENT"}, checks=[
                 ("the next tag id is past the tag a record wears, %u" % (maxtag + 2),
                  lambda c: M(c).next_tag_id == maxtag + 2)]),
        Case("C6 torn ledger", lambda o: o.Patch(t.VLba(t.lay["state_block"] + other) * SECTOR + 50,
                                                 bytes([o.sectors(t.VLba(t.lay["state_block"] + other), 1)[50] ^ 0xFF])),
             checks=[
                 ("the ledger copy is named torn", lambda c: bool(Findings(c, "LEDGER_TORN"))),
                 ("the kernel stands on copy %u" % stand, lambda c: c.f.ledger["copy"] == stand),
                 ("and on its seal alone", lambda c: M(c).seals[1] is None and Seal(c)["stood"] == "seal")]),
        Case("C6b no ledger", lambda o: [o.Patch(t.VLba(t.lay["state_block"] + i) * SECTOR + 50,
                                                 bytes([o.sectors(t.VLba(t.lay["state_block"] + i), 1)[50] ^ 0xFF]))
                                         for i in (0, 1)],
             must={"LEDGER_NONE"}, checks=[
                 ("the kernel refuses at the ledger", lambda c: c.kernel.step == "ledger")]),
        # --- the Use Context's chain ---------------------------------------------
        Case("C7 TUSE unnamed", lambda o: PlantUseChain(t, o, False), must={"TUSE_UNNAMED"}, checks=[
            ("it reads 2 links, 5000 bytes, crc ok",
             lambda c: any("2 links, 5000 bytes, crc ok" in x.text for x in Findings(c, "TUSE_UNNAMED")))]),
        Case("C7b TUSE broken", lambda o: PlantUseChain(t, o, True), must={"TUSE_BROKEN"}),
        Case("C7c TUSE named", lambda o: PlantUseChain(t, o, False, named=True), checks=[
            ("the chain copy 0 names is the use_chain's, %u+2" % F,
             lambda c: [x.runs for x in c.claims if x.kind == "use_chain"] == [[(F, 2)]]),
            ("and nothing is an orphan", lambda c: not K(c)["orphan_runs"])]),
        Case("C7d TUSE link never landed", lambda o: PlantUseChain(t, o, True, named=True),
             must={"TUSE_BROKEN"}, checks=[
                 ("the newest copy's chain is held only as far as its own links, %u+1" % F,
                  lambda c: [x.runs for x in c.claims if x.kind == "use_chain"] == [[(F, 1)]]),
                 ("the mount rolls the memory back and names the medium's lie",
                  lambda c: any("lied" in x.text for x in Findings(c, "TUSE_TORN"))),
                 ("the link of another pair is the orphan, %u+1" % (F + 1),
                  lambda c: K(c)["orphan_runs"] == [(F + 1, 1)])]),
        Case("C8 Spare", lambda o: PlantSpare(t, o), must={"SPARE_UNNAMED"}, checks=[
            ("it is a spare pool header", lambda c: any("spare pool" in x.text
                                                        for x in Findings(c, "SPARE_UNNAMED")))]),
    ]


def RowCases(t, x):
    """Files, rows and records, and chains linked to ground nothing
    stamped: C9-C19p."""
    F, S, Sfid, c0 = x.F, x.S, x.Sfid, x.c0
    maxtag, pool0, registry0, table0, tie_rule = x.maxtag, x.pool0, x.registry0, x.table0, x.tie_rule
    return [
        # --- files, rows and records ---------------------------------------------
        Case("C9 lost", lambda o: PlantRetire(t, o), must={"FILE_LOST", "ORPHAN"}, checks=[
            ("no seal is borne out", Stood("none", tie_rule)),
            Lost(1),
            ("the orphans are %s's blocks, named as a retired record's" % t.Label(S),
             lambda c: Runs(c, "ORPHAN") == t.Sruns and
             any("retired record" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C10 row derived", lambda o: t.Row(o, Sfid, (0, 0)), checks=[
            ("under the seal %s's row is set from the pool" % t.Label(S),
             lambda c: K(c)["derived"] == 1 and State(c, Sfid) == "live"),
            ("and the kernel says every file", lambda c: c.kernel.says[2] == c0.kernel.says[2])]),
        Case("C10b unnamed", lambda o: [NoSeal(t, o), t.Row(o, Sfid, (0, 0))],
             must={"UNNAMED_RECORD", "ORPHAN"}, checks=[
                 ("with no seal the record no row names is not taken up",
                  lambda c: K(c)["unnamed"] == 1 and State(c, Sfid) == "unnamed"),
                 ("UNNAMED_RECORD is a leak", lambda c: any(x.severity == "leak"
                                                            for x in Findings(c, "UNNAMED_RECORD"))),
                 ("the orphan is explained as its extents",
                  lambda c: any("unnamed record" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C11 BOOT", lambda o: PlantBoot(t, o), must={"BOOT_MISMATCH"}),
        Case("C12 registry chained", lambda o: [PlantRegistrySplit(t, o, F), Reseal(t, o, pool=False)], checks=[
            ("the registry chain is [%u, %u]" % (registry0[0], F),
             lambda c: c.f.chains["registry"] == [registry0[0], F]),
            ("every tag is adopted", lambda c: len(c.by_id) == len(c0.by_id)),
            ("the kernel's roll call reaches registry[1]", lambda c: not K(c)["orphan_runs"])]),
        Case("C12b registry torn", lambda o: t.PutBlock(o, registry0[-1], bytes(
                 b ^ 0x5A if i == ondisk.CHAIN_HEADER + 6 else b
                 for i, b in enumerate(t.Block(o, registry0[-1])))),
             must={"TAG_ABSENT"}, checks=[
                 ("the sealed block rolls back", lambda c: M(c).registry["rolled"] == registry0[-1] and
                  bool(Findings(c, "REGISTRY_ROLLED_BACK"))),
                 ("no tag is adopted", lambda c: not c.by_id),
                 ("and no tag id a record wears is given out again",
                  lambda c: M(c).next_tag_id == c0.f.mount.pool["top_tag"] + 1)]),
        Case("C12c registry past the seal", lambda o: PlantRegistryAppend(t, o, maxtag + 1), checks=[
            ("the record past the sealed bytes is not adopted",
             lambda c: maxtag + 1 not in c.by_id and len(c.by_id) == len(c0.by_id))]),
        Case("C13 missing", lambda o: PlantMissing(t, o), must={"MISSING"}, checks=[
            ("it is a note: the kernel keeps %u" % t.Lmid,
             lambda c: [x.severity for x in Findings(c, "MISSING")] == ["note"]),
            ("the kernel names %u+1 missing" % t.Lmid, lambda c: K(c)["missing_runs"] == [(t.Lmid, 1)])]),
        Case("C14 retired is not bad", lambda o: [t.Row(o, Sfid, (0, 0)), PlantRetire(t, o)] +
             [t.Bit(o, b, False) for s, n in t.Sruns for b in range(s, s + n)], checks=[
                 ("the retired record is read and passed over",
                  lambda c: any(r.fid == 0 and not r.unpack_ok for r in c.f.records))]),
        Case("C15 head torn", lambda o: PlantHeadTorn(t, o), must={"DEED_HEAD_BAD"}, checks=[
            ("it stands on the far copy", lambda c: c.f.stand.stood_on == "tail")]),
        Case("C17 many orphan runs", lambda o: [PlantOrphan(t, o, b) for b in ManyOrphans(t)],
             must={"ORPHAN"}, checks=[
                 ("the kernel names %u orphan runs of one block, past the %u it says one by one"
                  % (ORPHAN_RUNS, FSCK_RUNS_NAMED),
                  lambda c: K(c)["orphan_runs"] == [(b, 1) for b in ManyOrphans(t)]),
                 ("the census's orphans are the kernel's", lambda c: Runs(c, "ORPHAN") == K(c)["orphan_runs"])]),
        # --- chains linked to ground nothing stamped -------------------------------
        Case("C18 table linked to ground, sealed", lambda o: PlantLink(t, o, "table", F),
             must={"MISSING"}, checks=[
                 ("under the seal the table ends at %u, kept as its last link" % F,
                  lambda c: c.kernel.refusal is None and c.f.chains["table"] == table0 + [F]),
                 ("which the kernel takes again", lambda c: K(c)["missing_runs"] == [(F, 1)])]),
        LinkCase(t, "C18b", "table", "holds no file table"),
        Case("C19 registry linked to ground, sealed", lambda o: PlantLink(t, o, "registry", F), checks=[
            ("the walk ends at the block the seal names, %u" % registry0[-1],
             lambda c: c.kernel.refusal is None and c.f.chains["registry"] == registry0)]),
        LinkCase(t, "C19b", "registry", "holds 0x00000000"),
        Case("C19p pool linked to ground, sealed", lambda o: PlantLink(t, o, "pool", F), checks=[
            ("past the seal's open block the walk reads ids only, and stops at %u" % F,
             lambda c: c.kernel.refusal is None and c.f.chains["pool"] == pool0 and
             not M(c).pool["past_blocks"])]),
        LinkCase(t, "C19q", "pool", "holds no metadata pool"),
    ]


def PoolCases(t, x):
    """The pool's records, newest first: C20-C26."""
    F, S, Sfid = x.F, x.S, x.Sfid
    broken, broken_tail, maxfid, tie_rule = x.broken, x.broken_tail, x.maxfid, x.tie_rule
    return [
        # --- the pool's records, newest first --------------------------------------
        Case("C20 superseded", lambda o: PlantRewritten(t, o, t.Srec), checks=[
            ("%s is taken at the record appended, not the one its row names" % t.Label(S),
             lambda c: (c.f.newest[Sfid].block, c.f.newest[Sfid].at) == t.PoolTail),
            ("the kernel corrects its row to that record and loses nothing",
             lambda c: K(c)["corrected"] == 1 and K(c)["lost"] == 0 and State(c, Sfid) == "live"),
            ("its blocks are owned once", lambda c: not K(c)["orphan_runs"])]),
        Case("C21 quitclaim, row left", lambda o: [PlantAppend(t, o, Quitclaim(Sfid)),
                                                   Reseal(t, o, registry=False)],
             must={"ORPHAN"}, checks=[
                 ("%s is gone: its newest record is the Quitclaim" % t.Label(S),
                  lambda c: State(c, Sfid) == "quitclaim" and bool(Findings(c, "QUITCLAIM_PENDING"))),
                 ("its row goes with the delete, neither a cycle that did not settle nor a loss",
                  lambda c: K(c)["quitclaimed"] == 1 and K(c)["unsettled"] == 0 and K(c)["lost"] == 0),
                 ("the orphans are %s's blocks, named as a superseded record's" % t.Label(S),
                  lambda c: Runs(c, "ORPHAN") == t.Sruns and
                  any("superseded record" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C21b quitclaim, row left, no seal", lambda o: [PlantAppend(t, o, Quitclaim(Sfid)),
                                                             NoSeal(t, o)],
             must={"ORPHAN"}, checks=[
                 ("with no seal too the row goes with the delete, and nothing is lost",
                  lambda c: K(c)["quitclaimed"] == 1 and K(c)["lost"] == 0 and
                  State(c, Sfid) == "quitclaim" and not Findings(c, "FILE_LOST"))]),
        Case("C22 quitclaim landed", lambda o: PlantQuitclaimLanded(t, o), checks=[
            ("%s is no file at all" % t.Label(S), lambda c: Sfid not in c.files),
            ("and the kernel finds nothing to repair",
             lambda c: not K(c)["orphan_runs"] and not K(c)["missing_runs"])]),
        Case("C23 record past the seal", lambda o: [
                 PlantAppend(t, o, RecordOf(t, o, t.Srec, fid=maxfid + 5, block=F)), t.Bit(o, F, True)],
             must={"ORPHAN"}, checks=[
                 ("the seal is borne out and the record past it is not taken up",
                  lambda c: Seal(c)["stood"] == "seal" and (maxfid + 5) not in c.files),
                 ("its id is all the same: the next file id is %u" % (maxfid + 6),
                  lambda c: M(c).next_file_id == maxfid + 6),
                 ("its block is the orphan, named as a record past the seal's",
                  lambda c: Runs(c, "ORPHAN") == [(F, 1)] and
                  any("past the seal's span" in x.text for x in Findings(c, "ORPHAN")))]),
        Case("C24 rotten fid", lambda o: t.RecordPatch(
                 o, t.Srec, 2, struct.pack("<I", Sfid | 0x40000000), False),
             must={"RECORD_CRC", "FILE_LOST", "ORPHAN"}, checks=[
                 ("no seal is borne out", Stood("none", tie_rule)),
                 Lost(1),
                 ("a record that does not check out names no id: the next file id is %u" % (maxfid + 1),
                  lambda c: M(c).next_file_id == maxfid + 1)]),
        Case("C25 extent past the run", lambda o: PlantRewritten(t, o, t.Srec, block=t.f.D + 5),
             must={"EXTENT_OUTSIDE", "ORPHAN"}, checks=[
                 ("%s is taken up naming no blocks" % t.Label(S),
                  lambda c: State(c, Sfid) == "live" and not c.files[Sfid]["record"].extents_at_mount),
                 ("the walk counts 1 record whose extents it does not follow",
                  lambda c: K(c)["dropped"] == 1 and K(c)["dropped_first"] == Sfid),
                 ("the orphans are %s's old blocks" % t.Label(S),
                  lambda c: Runs(c, "ORPHAN") == t.Sruns)]),
        Case("C26 record length rotten", lambda o: t.RecordPatch(
                 o, broken, 0, struct.pack("<H", 0xFFF0), False),
             must={"RECORD_SHAPE", "FILE_LOST", "ORPHAN"}, checks=[
                 ("no seal is borne out", Stood("none", tie_rule)),
                 ("the walk says the %u byte(s) of block %u from fid %u's record on are not read"
                  % (broken_tail, broken.block, broken.fid),
                  lambda c: K(c)["skipped"] == [1, broken_tail] and
                  K(c)["skipped_first"] == [broken.block, broken.at]),
                 Lost(1)]),
    ]


def SealCases(t, x):
    """Which seal the pool stands on: S1-S8."""
    F, c0, maxfid = x.F, x.c0, x.maxfid
    older, other, tail_runs, tie_rule = x.older, x.other, x.tail_runs, x.tie_rule
    return [
        # --- which seal the pool stands on -----------------------------------------
        Case("S1 fell back", lambda o: PlantFellBack(t, o), must={"ORPHAN"}, checks=[
            ("the newest seal is not borne out and the volume stands on the other",
             lambda c: Stood("seal", 1 - tie_rule, True)(c) and bool(Findings(c, "SEAL_FELL_BACK"))),
            ("its verdicts are torn and borne", lambda c: Seal(c)["verdicts"] == ["torn", "borne"]),
            Lied(True),
            ("the new file's row is let go unsaid", lambda c: K(c)["unsettled"] == 1 and
             State(c, maxfid + 1) == "unsettled"),
            ("and its id is never given out again: the next file id is %u" % (maxfid + 2),
             lambda c: M(c).next_file_id == maxfid + 2),
            ("its block is the orphan", lambda c: K(c)["orphan_runs"] == [(F, 1)])]),
        Case("S2 torn tail", lambda o: PlantTornTail(t, o), must={"ORPHAN", "FILE_LOST"}, checks=[
            ("the newest seal's open block rolls back", lambda c: Stood("torn", tie_rule)(c) and
             bool(Findings(c, "SEAL_TAIL_TORN"))),
            Lied(True),
            ("the %u file(s) whose only record settled there are lost, said, and no row is let go "
             "as a cycle that did not settle" % len(t.tail_files),
             lambda c: K(c)["lost"] == len(t.tail_files) and K(c)["unsettled"] == 0 and
             sorted(f for f in t.tail_files if State(c, f) == "lost") == t.tail_files),
            ("the kernel says %u files" % (c0.kernel.says[2] - len(t.tail_files)),
             lambda c: c.kernel.says[2] == c0.kernel.says[2] - len(t.tail_files)),
            ("their blocks are the orphans", lambda c: K(c)["orphan_runs"] == tail_runs)]),
        Case("S3 no seal", lambda o: NoSeal(t, o), checks=[
            ("a kernel before the seal wrote both copies: none is stood on",
             lambda c: Stood("none", tie_rule)(c) and bool(Findings(c, "SEAL_ABSENT"))),
            ("and the volume reads whole", lambda c: c.kernel.says == c0.kernel.says and
             not K(c)["orphan_runs"])]),
        Case("S4 closed block rewritten in place", lambda o: t.RecordPatch(
                 o, t.Srec, t.Srec.len - 1, bytes([t.Block(o, t.Srec.block)[t.Srec.at + t.Srec.len - 1] ^ 1]),
                 True), checks=[
            ("no seal is borne out", lambda c: Stood("none", tie_rule)(c) and
             Seal(c)["verdicts"] == ["broken", "broken"] and bool(Findings(c, "SEAL_UNBORNE"))),
            ("and every file reads", lambda c: c.kernel.says == c0.kernel.says)]),
        Case("S5 newest unsealed, older sealed", lambda o: [
                 t.Ledger(o, 0, lambda raw: (struct.pack_into("<Q", raw, LEDGER_AT["seq"], 2),
                                             struct.pack_into("<I", raw, 8, ondisk.LEDGER_SEAL_AT)))],
             checks=[
                 ("copy 1's seal is not looked at: there is no seal", lambda c:
                  c.f.ledger["copy"] == 0 and Stood("none", 0)(c) and not Seal(c)["newest_sealed"])]),
        Case("S6 seal names a block the chain never reaches", lambda o: Reseal(
                 t, o, registry=False, edit=lambda mark, raw: mark.update(pool_to_block=F)), checks=[
            ("both seals are broken and none is stood on",
             lambda c: Stood("none", tie_rule)(c) and Seal(c)["verdicts"] == ["broken", "broken"]),
            Lied(False)]),
        Case("S7 the newest seal alone, its open block torn",
             lambda o: [NoSeal(t, o, copies=(other,)), PlantTornTail(t, o)],
             must={"RECORD_CRC", "FILE_LOST", "ORPHAN"}, checks=[
                 ("nothing is rolled back: the whole chain is read, with no seal",
                  lambda c: Stood("none", tie_rule)(c) and Seal(c)["alone"] and
                  bool(Findings(c, "SEAL_ALONE_TORN"))),
                 Lied(True),
                 ("only the file whose record the tear reached is lost", lambda c: K(c)["lost"] == 1 and
                  K(c)["unreadable"] == 1 and
                  [f for f in t.tail_files if State(c, f) == "live"] == t.tail_files[:-1])]),
        Case("S8 the other seal's open block closed by the newest",
             lambda o: PlantOlderClosed(t, o), must={"FILE_LOST", "ORPHAN"}, checks=[
                 ("the newest seal stands, its open block alone rolled back",
                  lambda c: Stood("torn", tie_rule)(c) and Seal(c)["verdicts"][1] == "borne"),
                 Lied(True),
                 ("the record the newest cycle laid in the block it closed is kept",
                  lambda c: State(c, older.fid) == "live"),
                 ("and the %u file(s) of the torn open block are lost" % len(t.tail_files),
                  lambda c: K(c)["lost"] == len(t.tail_files) and K(c)["unsettled"] == 0)]),
    ]


def ReadCases(t, x):
    """A sector the medium cannot read, Glean, a rotten newest record, the
    owners and what logcheck boots: S9-T2, GL1-GL2, R1-R3, O1, reconcile."""
    F, S, Sfid, U, Ufid = x.F, x.S, x.Sfid, x.U, x.Ufid
    pool0, registry0, rows0, table0, tie_rule = x.pool0, x.registry0, x.rows0, x.table0, x.tie_rule
    return [
        # --- a sector the medium cannot read, judged by where it lies --------------
        Case("S9 bad sector in the open block", lambda o: PlantBadSector(t, o, t.PoolTail[0]),
             must={"ORPHAN", "FILE_LOST"}, checks=[
                 ("a page torn as it was written: the newest seal's open block rolls back, and "
                  "the volume mounts", lambda c: Stood("torn", tie_rule)(c) and c.kernel.refusal is None),
                 Lied(True),
                 ("the %u file(s) whose only record settled there are lost, said" % len(t.tail_files),
                  lambda c: K(c)["lost"] == len(t.tail_files) and K(c)["unsettled"] == 0)]),
        Case("S10 bad sector in a closed block", lambda o: PlantBadSector(t, o, pool0[0]),
             must={"CHAIN_REFUSED"}, may={"BOOT_MISMATCH", "FILE_LOST", "ORPHAN"}, checks=[
                 ("rot: no seal is borne out", lambda c: Seal(c)["verdicts"] == ["broken", "broken"]),
                 ("and the walk of a kernel before the seal meets it again: the mount is refused at "
                  "the pool, naming block %u" % pool0[0],
                  lambda c: c.kernel.step == "pool" and
                  c.f.refused.get("pool", "").startswith("block %u " % pool0[0]) and
                  "holds a sector the medium cannot read" in c.f.refused["pool"])]),
        Case("S11 read of the open block refused", lambda o: PlantRefusedRead(t, o, t.PoolTail[0]),
             must={"CHAIN_REFUSED"}, may={"FILE_LOST", "ORPHAN"}, checks=[
                 ("a refusal is no verdict on any seal: the mount is refused at the pool, nothing "
                  "rolled back", lambda c: c.kernel.step == "pool" and
                  c.f.refused.get("pool", "").startswith(
                      "the medium refused the read of block %u " % t.PoolTail[0]) and
                  Seal(c)["stood"] == "none" and "torn" not in Seal(c)["verdicts"])]),
        Case("T1 bad sector in a table block, sealed", lambda o: PlantBadSector(t, o, table0[-1]),
             checks=[
                 ("under the seal the table ends at block %u, kept as its last link, and the volume "
                  "mounts" % table0[-1],
                  lambda c: c.kernel.refusal is None and c.f.chains["table"] == table0)]),
        Case("T2 read of a table block refused, sealed", lambda o: PlantRefusedRead(t, o, table0[-1]),
             must={"CHAIN_REFUSED"}, may={"ORPHAN"}, checks=[
                 ("a refusal is no verdict under the seal either: the mount is refused at the table",
                  lambda c: c.kernel.step == "table" and
                  c.f.refused.get("table", "").startswith(
                      "the medium refused the read of block %u " % table0[-1]))]),
        # --- Glean: the chain's first block moves ----------------------------------
        Case("GL1 Glean head", lambda o: PlantGlean(t, o), checks=[
            ("the chain begins at the seal's first block, %u" % F,
             lambda c: c.f.chains["pool"] == [F + i for i in range(len(pool0))]),
            ("what the head names before it is the pool's",
             lambda c: M(c).prefix == pool0[1:] and
             any(x.kind == "pool" and x.label == "pool head" for x in c.claims)),
            ("every row follows its record and nothing is an orphan",
             lambda c: K(c)["corrected"] == 0 and not K(c)["orphan_runs"] and not K(c)["missing_runs"])]),
        Case("GL2 Glean, newest lied", lambda o: PlantGlean(t, o, lie=True), must={"ORPHAN"}, checks=[
            ("the other copy's seal, its chain from the head, is judged by a walk of its own and stood on",
             lambda c: Stood("seal", 1 - tie_rule, True)(c) and c.f.chains["pool"] == pool0),
            ("the new blocks are past its open block", lambda c: M(c).pool["past_blocks"] ==
             [F + i for i in range(len(pool0))] and K(c)["orphan_runs"] == [(F, len(pool0))]),
            ("every row moved with Glean is corrected back", lambda c: K(c)["corrected"] == rows0)]),
        # --- a rotten newest record: today's rule (meta_pool_mount.c WalkPut) -----
        Case("R1 rotten newest, older stands", lambda o: PlantRotten(t, o, False),
             must={"RECORD_CRC", "ORPHAN"}, checks=[
                 ("%s stands on its older record" % t.Label(S),
                  lambda c: State(c, Sfid) == "live" and
                  (c.files[Sfid]["record"].block, c.files[Sfid]["record"].at) == (t.Srec.block, t.Srec.at)),
                 ("the walk passes 1 record over", lambda c: K(c)["unreadable"] == 1),
                 ("the rotten record's block is the orphan", lambda c: K(c)["orphan_runs"] == [(F, 1)])]),
        Case("R2 rotten newest, older's block taken", lambda o: PlantRotten(t, o, True),
             must={"RECORD_CRC", "DOUBLE_OWNER", "ORPHAN"}, checks=[
                 ("%s stands on its older record, whose block %s holds" % (t.Label(S), t.Label(U)),
                  lambda c: State(c, Sfid) == "live" and any(
                      t.Label(S) in x.text and "fid %u " % Ufid in x.text for x in Findings(c, "DOUBLE_OWNER"))),
                 ("the orphans are %s's old block and the rotten record's" % t.Label(U),
                  lambda c: K(c)["orphan_runs"] == RunsNormal(t.Uruns + [(F, 1)]))]),
        Case("R3 rotten only record, sealed", lambda o: PlantRottenSettled(t, o),
             must={"RECORD_CRC", "FILE_LOST", "ORPHAN"}, checks=[
                 ("the seal is borne out over the rotten record, and stood on",
                  Stood("seal", tie_rule)),
                 ("the walk passes 1 record over", lambda c: K(c)["unreadable"] == 1),
                 Lost(1),
                 ("%s, an id the stood Ledger had issued, is the file lost — said, not let go as "
                  "a cycle that did not settle" % t.Label(S),
                  lambda c: State(c, Sfid) == "lost" and
                  any("fid %u " % Sfid in x.text and "had settled it" in x.text
                      for x in Findings(c, "FILE_LOST"))),
                 ("its blocks are the orphans", lambda c: K(c)["orphan_runs"] == t.Sruns)]),
        # --- the owners ------------------------------------------------------------
        Case("O1 manifest", lambda o: PlantManifest(t, o), checks=[
            ("the manifest the Ledger names is owned", lambda c: [x.runs for x in c.claims
                                                                  if x.kind == "manifest"] == [[(F, 1)]]),
            ("and nothing is an orphan", lambda c: not K(c)["orphan_runs"])]),
        # --- what logcheck boots ---------------------------------------------------
        Case("reconcile", lambda o: Reconcile(t, o), must={"ORPHAN", "FILE_LOST", "MISSING"}, checks=[
            ("the census's orphans are the kernel's", lambda c: Runs(c, "ORPHAN") == K(c)["orphan_runs"]),
            ("the kernel's orphans are %u+2 and %s's blocks" % (F, t.Label(S)),
             lambda c: K(c)["orphan_runs"] == RunsNormal([(F, 2)] + t.Sruns)),
            ("the kernel names %u+1 missing" % t.Lmid, lambda c: K(c)["missing_runs"] == [(t.Lmid, 1)]),
            ("the registry chain reaches %u under its seal" % (F + 3),
             lambda c: c.f.chains["registry"] == [registry0[0], F + 3]),
            Lost(1)]),
    ]


def GroundCases(t):
    return [
        Case("G1 MBR type", lambda o: PlantMbrEntry(o, kind=0x83), must={"GROUND_REFUSED"}, checks=[
            ("the kernel refuses at the ground", lambda c: c.kernel.step == "ground"),
            ("the refusal names the types it found", lambda c: "0:0x83" in c.kernel.refusal),
            ("check exits 1", lambda c: c.ExitCode() == 1)]),
        Case("G2 MBR past the end", lambda o: PlantMbrEntry(o, extra=1), must={"GROUND_REFUSED"}, checks=[
            ("the kernel does not use an entry past the medium",
             lambda c: c.kernel.step == "ground" and "not using it" in c.kernel.refusal)]),
        Case("G3 GPT", lambda o: PlantGpt(o, t.base), checks=[
            ("the kernel stands on GPT entry 0", lambda c: c.f.stand.kernels and
             c.f.stand.how == "GPT entry 0 (BoxOS type)" and c.kernel.refusal is None)]),
        Case("G4 GPT primary array", lambda o: PlantGpt(o, t.base, primary="array"), checks=[
            ("the kernel stands on the far copy's entry",
             lambda c: c.f.stand.kernels and "from the copy at the far end" in c.f.stand.how and
             c.kernel.refusal is None)]),
        Case("G5 GPT primary gone", lambda o: PlantGpt(o, t.base, primary="gone"), checks=[
            ("the kernel stands on the far copy's entry",
             lambda c: c.f.stand.kernels and "from the copy at the far end" in c.f.stand.how and
             c.kernel.refusal is None)]),
        Case("G6 GPT both arrays", lambda o: PlantGpt(o, t.base, primary="array", backup="array"),
             must={"GROUND_REFUSED"}, checks=[
                 ("the kernel reads neither copy", lambda c: c.kernel.step == "ground" and
                  "neither copy of its GPT" in c.kernel.refusal)]),
        Case("G7 GPT ends first", lambda o: PlantGpt(o, t.base, ends=t.base - 1), must={"GROUND_REFUSED"}, checks=[
            ("the kernel skips an entry that ends before it begins",
             lambda c: c.kernel.step == "ground" and "ends before it begins" in c.kernel.refusal)]),
        Case("G8 GPT of 1024", lambda o: PlantGpt(o, t.base, count=1024), checks=[
            ("the kernel reads a table of 1024 entries", lambda c: c.f.stand.kernels and
             c.kernel.refusal is None)]),
        Case("G9 bitmap away", lambda o: PlantBitmapAway(t, o), must={"BITMAP_UNREADABLE", "REGION_OVERLAP"},
             checks=[("the kernel refuses at the bitmap", lambda c: c.kernel.step == "bitmap")]),
    ]


def LinkCase(t, label, name, said):
    """A chain whose last block links to free ground (PlantLink) on a volume
    with no seal: the mount refuses at that chain, and says which block it
    found unstamped."""
    before = t.c0.f.chains[name]
    return Case("%s %s linked to ground, no seal" % (label, name),
                lambda o: [NoSeal(t, o), PlantLink(t, o, name, t.F)],
                must={"CHAIN_REFUSED"}, checks=[
                    ("the kernel refuses at the %s" % name, lambda c: c.kernel.step == name),
                    ("the refusal names data block %u, never stamped" % t.F,
                     lambda c: c.f.refused.get(name, "").startswith("block %u " % t.F) and
                     said in c.f.refused[name]),
                    ("the chain stops at its last stamped block, %u" % before[-1],
                     lambda c: c.f.chains[name][:len(before)] == before)])


# ---------------------------------------------------------------------------
# L1-L5: the cut logic, on a log built in memory
# ---------------------------------------------------------------------------


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
# Running them over a format-v1 image
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


def V1Cases(t, medium, count):
    """Every case of format v1 over the image; the owner kinds heard."""
    heard = set()
    for case in Cases(t):
        over = OverlayMedium(medium)
        case.plant(over)
        census = GroundCensus(over)
        ok, codes, wrong = Judge(case, census)
        heard |= Kinds(census)
        count.Say(ok, "%s — flagged %s — %s" % (case.name, ", ".join(codes) or "nothing",
                                                 "as expected" if ok else "; ".join(wrong)))
    over = OverlayMedium(medium)
    PlantNoDeed(t, over)
    try:
        GroundCensus(over)
        ok, said = False, "the census read a volume with no deed at either end"
    except Unreadable as e:
        ok, said = True, "unreadable: %s" % e
    count.Say(ok, "C16 no deed — %s — %s" % (said, "as expected" if ok else "expected unreadable"))
    for name, ok, said in FrameCases(t, medium) + LogCases(t, medium):
        count.Say(ok, "%s — %s — %s" % (name, said, "as expected" if ok else "NOT so"))
    return heard
