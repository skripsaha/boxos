"""selftest — every census rule planted into an image and caught, the cut logic
proved on a synthetic log, and the format held against the C headers.

A rule nobody has seen fire is a rule that may not work. Each case here lays
one kind of damage — or one state a kernel cycle leaves — over the image in
memory (an OverlayMedium — the image file is never written), runs the census on
it, and holds the findings, and the mount's decisions, against what that state
must produce. The image has to be consistent to begin with (C0), because every
case is measured as a difference from it; and it is sealed, as mkfs leaves every
volume, so a plant that changes the pool or the registry either seals again what
it laid down (Reseal — what a Settle cycle's last write leaves) or leaves the
seal as it was and is read the way the mount reads a seal the medium does not
bear out. NoSeal writes both copies of the Ledger the way a kernel before the
seal writes them.

`--emit CASE --out IMG` writes one case's overlay out as a new image, for a
harness to boot: logcheck's `census` scenario boots some of them and holds the
kernel's own lines against this package's prediction.
"""

import importlib.util
import os
import re
import struct

from . import ondisk
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK, U16, U32
from .medium import OverlayMedium, Unreadable
from .census import GroundCensus
from .ondisk import RunsNormal, RunsSubtract
from .ground_owners import GROUND_OWNERS
from . import blklog


class Targets:
    """What the cases aim at, chosen off C0's census.

    S — the live file with the fewest blocks (at least one), lowest fid first,
        that has exactly one record in the pool, carries a tag and a name, is
        not the BOOT file, and whose record lies in a closed block of the pool:
        a file whose loss nothing else stands in for, and whose record changed
        in place is a closed block the seal no longer bears out.
    U — the next such file of one block: the file a block S let go is taken by.
    L — the live file with the most blocks, lowest fid first.
    F — the lowest data block starting four free ones (`reconcile` sits a
        registry block at F+3)."""

    def __init__(self, c0):
        f = c0.f
        self.c0, self.f = c0, f
        self.base, self.lay = f.base, f.layout
        live = [e for e in c0.files.values() if e["state"] == "live"]
        tail_block, tail, _raw = f.pool[-1]
        counts = {}
        for r in f.records:
            counts[r.fid] = counts.get(r.fid, 0) + 1
        ok = [e for e in live if e["record"].Blocks() >= 1 and counts.get(e["fid"]) == 1 and
              e["record"].tags and e["record"].name and e["record"].block != tail_block and
              ondisk.RECORD_HEADER <= e["record"].len and
              "fid %u '%s'" % (e["fid"], e["record"].name) != c0.boot_file]
        if not ok or len(live) < 2:
            raise Unreadable("the image has no live file a case can aim at")
        self.S = min(ok, key=lambda e: (e["record"].Blocks(), e["fid"]))
        ones = [e for e in ok if e is not self.S and e["record"].Blocks() == 1]
        if not ones:
            raise Unreadable("the image has no second one-block file a case can aim at")
        self.U = min(ones, key=lambda e: e["fid"])
        self.L = min((e for e in live if e is not self.S),
                     key=lambda e: (-e["record"].Blocks(), e["fid"]))
        free = RunsSubtract([(3, f.D - 3)], f.used)
        self.F = next(s for s, n in free if n >= 4)
        self.Srec, self.Lrec, self.Urec = self.S["record"], self.L["record"], self.U["record"]
        self.Sruns = RunsNormal(self.Srec.extents_at_mount)
        self.Uruns = RunsNormal(self.Urec.extents_at_mount)
        blocks = [b for s, n in RunsNormal(self.Lrec.extents_at_mount) for b in range(s, s + n)]
        self.Lmid = blocks[len(blocks) // 2]
        self.PoolTail = (tail_block, ondisk.CHAIN_HEADER + tail["used_bytes"])
        self.maxfid = max(e["fid"] for e in live)
        self.tail_files = sorted(e["fid"] for e in live if e["record"].block == tail_block)

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
        b = self.f.chains["table"][link]
        raw = self.Block(over, b)
        struct.pack_into("<II", raw, ondisk.CHAIN_HEADER + i * 8, *place)
        if i >= U32(raw, 8):
            struct.pack_into("<I", raw, 8, i + 1)
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

    def Chain(self, over, first):
        """[(block, raw)] of a chain followed by next_block from `first`."""
        out, b = [], first
        while True:
            raw = self.Block(over, b)
            out.append((b, raw))
            b = U32(raw, 4)
            if b == 0 or len(out) >= self.f.D:
                return out


LEDGER_AT = dict((f, o) for f, o, _s in ondisk.VOLUME_LEDGER.Offsets())


def LedgerField(name, value):
    """An edit for Targets.Ledger: one u32 field of the record set."""
    return lambda raw: struct.pack_into("<I", raw, LEDGER_AT[name], value)


# ---------------------------------------------------------------------------
# The plants
# ---------------------------------------------------------------------------


def Reseal(t, over, pool=True, registry=True, copies=(0, 1), first=None, edit=None):
    """What a Settle cycle's last write leaves over what the medium now holds:
    each copy's seal made again (settle kept, so it is still the copy's own) —
    the pool from the seal's first block (`first`, when Glean moved it) to the
    chain's end, its closed blocks folded and its open block summed; the
    registry's last block and its records. `edit(mark, raw)` changes the seal
    and the record after, for a newer cycle or a seal that lies."""
    def Seal(raw):
        mark = ondisk.SETTLE_MARK.Unpack(raw, ondisk.LEDGER_SEAL_AT)
        if first is not None:
            mark["chain_first"] = first
        if pool:
            chain = t.Chain(over, mark["chain_first"])
            fold = 0
            for b, r in chain[:-1]:
                fold ^= ondisk.PoolBlockMix(b, r)
            tb, tr = chain[-1]
            to = ondisk.PoolUsed(tr)
            mark.update(pool_to_block=tb, pool_to_bytes=to, closed_crc32=fold,
                        tail_crc32=ondisk.Crc32(bytes(tr[ondisk.CHAIN_HEADER:ondisk.CHAIN_HEADER + to])))
        if registry:
            rb, rr = t.Chain(over, t.f.mount.head["tag_registry_block"])[-1]
            n = U16(rr, 10)
            mark.update(registry_block=rb, registry_bytes=n,
                        registry_crc32=ondisk.Crc32(bytes(rr[ondisk.CHAIN_HEADER:ondisk.CHAIN_HEADER + n])))
        if edit:
            edit(mark, raw)
        raw[ondisk.LEDGER_SEAL_AT:ondisk.LEDGER_SEAL_END] = ondisk.SETTLE_MARK.struct.pack(
            *(mark[k] for k in ondisk.SETTLE_MARK.names))
    for c in copies:
        t.Ledger(over, c, Seal)


def NewerCycle(more=0):
    """A Reseal edit: the copy written by the next cycle (seq and settle 2),
    its seal naming `more` bytes of the open block past what the medium holds."""
    def Edit(mark, raw):
        mark["settle"] = 2
        mark["pool_to_bytes"] += more
        struct.pack_into("<Q", raw, LEDGER_AT["seq"], 2)
    return Edit


def NoSeal(t, over, copies=(0, 1)):
    """The copies as a kernel before the seal writes them: the record ends
    where the seal would begin (volume_ledger.h LedgerHas)."""
    for c in copies:
        t.Ledger(over, c, LedgerField("bytes", ondisk.LEDGER_SEAL_AT))


def PlantOrphan(t, over, b):
    t.Bit(over, b, True)


def PlantRegistrySplit(t, over, at):
    """The registry's last ten records moved to data block `at`, linked from
    the head: a registry chain whose continuation is NOT data block 0."""
    head = t.f.chains["registry"][0]
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


def PlantRegistryAppend(t, over, tag_id):
    """A registry record (id `tag_id`, key "zz") laid after the last block's
    records, its counts moved with it, and the seal left as it was: a tag a
    cycle that did not settle added."""
    b = t.f.chains["registry"][-1]
    raw = t.Block(over, b)
    hdr = ondisk.REGISTRY_BLOCK.Unpack(raw)
    rec = ondisk.REGISTRY_RECORD.struct.pack(tag_id, 0, 2, 0) + b"zz"
    at = ondisk.CHAIN_HEADER + hdr["used_bytes"]
    raw[at:at + len(rec)] = rec
    struct.pack_into("<HH", raw, 8, hdr["entry_count"] + 1, hdr["used_bytes"] + len(rec))
    t.PutBlock(over, b, raw)


def PlantLink(t, over, name, at):
    """A chain's last block naming data block `at`, ground nothing ever
    stamped — what a chain grown link first leaves when the power goes before
    the new block is written."""
    last = t.f.chains[name][-1]
    raw = t.Block(over, last)
    struct.pack_into("<I", raw, 4, at)
    t.PutBlock(over, last, raw)


def PlantRetire(t, over):
    """What an earlier kernel's meta_pool_delete left: the file_id zeroed and
    the CRC left alone — a closed block rewritten in place."""
    t.RecordPatch(over, t.Srec, 2, b"\0\0\0\0", False)


def PlantAppend(t, over, record):
    """`record` appended to the pool as every Ink writes a record, older ones
    left where they are: after the last record of the chain's last block, that
    block's used bytes, record count and kept count moved with it. Where it
    landed, (block, at)."""
    block, _at = t.PoolTail
    raw = t.Block(over, block)
    head = ondisk.META_POOL_BLOCK.Unpack(raw)
    at = ondisk.CHAIN_HEADER + head["used_bytes"]
    if at + len(record) > ondisk.CHAIN_HEADER + ondisk.MPOOL_DATA:
        raise Unreadable("the pool's last block has no room for a %u-byte record" % len(record))
    raw[at:at + len(record)] = record
    kept = head["kept"] + (1 if struct.unpack_from("<I", record, 2)[0] else 0)
    struct.pack_into("<HHH", raw, 8, head["used_bytes"] + len(record), head["record_count"] + 1, kept)
    t.PutBlock(over, block, raw)
    return block, at


def Resummed(record):
    rec = bytearray(record)
    rec[ondisk.RECORD_CRC_AT:ondisk.RECORD_CRC_AT + 2] = b"\0\0"
    struct.pack_into("<H", rec, ondisk.RECORD_CRC_AT, ondisk.Crc16(bytes(rec)))
    return bytes(rec)


def Quitclaim(fid):
    """A Quitclaim of `fid` (Quill design §2): a record of the file without
    ACTIVE, nothing else in it, summed."""
    return Resummed(ondisk.META_RECORD.struct.pack(ondisk.RECORD_HEADER, fid, 0, 0, 0, 0, 0, 0, 0, 0))


def RecordOf(t, over, rec, fid=None, block=None, tag=None, rot=False):
    """`rec`'s bytes again — as file `fid`, its first extent at `block`, its
    first tag `tag` — summed, or with its sum broken when `rot`."""
    out = bytearray(t.Block(over, rec.block)[rec.at:rec.at + rec.len])
    if fid is not None:
        struct.pack_into("<I", out, 2, fid)
    if tag is not None:
        struct.pack_into("<H", out, ondisk.RECORD_HEADER, tag)
    if block is not None:
        struct.pack_into("<I", out, ondisk.RECORD_HEADER + 2 * rec.tag_count, block)
    out = bytearray(Resummed(out))
    if rot:
        out[-1] ^= 0x5A
    return bytes(out)


def PlantRewritten(t, over, rec, **kw):
    """`rec`'s file written again by a cycle that settled: the record appended
    (RecordOf with `kw`) and the pool sealed over it."""
    place = PlantAppend(t, over, RecordOf(t, over, rec, **kw))
    Reseal(t, over, registry=False)
    return place


def PlantQuitclaimLanded(t, over):
    """S deleted by a cycle that settled: its Quitclaim appended, its row gone,
    its blocks free, the pool sealed over it."""
    PlantAppend(t, over, Quitclaim(t.S["fid"]))
    t.Row(over, t.S["fid"], (0, 0))
    for s, n in t.Sruns:
        for b in range(s, s + n):
            t.Bit(over, b, False)
    Reseal(t, over, registry=False)


def PlantMissing(t, over):
    t.Bit(over, t.Lmid, False)


def PlantFellBack(t, over):
    """A cycle the medium lied about: a new file (fid maxfid+1, its block F
    taken, its row written) appended at the open block, and copy 0 written as
    the next cycle with a seal naming 42 bytes more of the open block than the
    medium holds; copy 1's seal, the cycle before, is borne out."""
    block, at = PlantAppend(t, over, RecordOf(t, over, t.Srec, fid=t.maxfid + 1, block=t.F))
    t.Row(over, t.maxfid + 1, (block, at))
    t.Bit(over, t.F, True)
    Reseal(t, over, copies=(0,), edit=NewerCycle(ondisk.RECORD_HEADER))


def PlantTornTail(t, over):
    """The open block torn as it was written again: a byte of its settled
    records is not what either seal summed."""
    block, _at = t.PoolTail
    raw = t.Block(over, block)
    raw[ondisk.CHAIN_HEADER + ondisk.PoolUsed(raw) - 1] ^= 0x5A
    t.PutBlock(over, block, raw)


def PlantBadSector(t, over, block):
    """The medium answers that a sector of data block `block` cannot be read
    (UNC, MEDIUM ERROR) — what a page torn as it was written reads as."""
    over.Bad(t.DLba(block), 1)


def PlantRefusedRead(t, over, block):
    """The medium refuses the read of data block `block`: a command dropped,
    no answer."""
    over.Refuse(t.DLba(block), 1)


def PlantOlderClosed(t, over):
    """The owner's B3: the other copy's seal made the cycle before, whose open
    block was the chain's first block — closed since by the newest cycle,
    which laid its last record there — and the newest cycle's open block torn
    (PlantTornTail). The fid of that last record, which only the newest seal
    holds."""
    first = t.f.chains["pool"][0]
    raw = t.Block(over, first)
    k = LastRecord(t, first).at - ondisk.CHAIN_HEADER

    def Before(mark, _raw):
        mark.update(pool_to_block=first, pool_to_bytes=k, closed_crc32=0,
                    tail_crc32=ondisk.Crc32(bytes(raw[ondisk.CHAIN_HEADER:ondisk.CHAIN_HEADER + k])))
    other = 1 - t.c0.f.ledger["copy"]
    Reseal(t, over, pool=False, registry=False, copies=(other,), edit=Before)
    PlantTornTail(t, over)


def LastRecord(t, block):
    """The last record of a closed block of the pool, as the image holds it —
    one a case may lose or break: never the BOOT file's."""
    raw = t.Block(OverlayMedium(t.f.medium), block)
    walk, _stop = ondisk.PoolRecords(raw, ondisk.PoolUsed(raw))
    if len(t.f.chains["pool"]) < 2 or len(walk) < 2:
        raise Unreadable("the pool has no closed block of two records for a case to aim at")
    rec = ondisk.ParseRecord(raw, walk[-1][0], block)
    if "fid %u '%s'" % (rec.fid, rec.name) == t.c0.boot_file:
        raise Unreadable("the last record of pool block %u is the BOOT file's" % block)
    return rec


def PlantGlean(t, over, lie=False):
    """What Glean leaves: the records of the pool's blocks laid again in new
    blocks from F on, linked after the old open block, every row following its
    record, and the seal's first block moved to F — the Deed's head still
    naming the old blocks, which now lie before the chain's first. With `lie`,
    only copy 0 is sealed so, as the next cycle, naming 42 bytes more of its
    open block than the medium holds: copy 1's seal, whose chain begins at the
    head, is judged by a walk of its own."""
    old = t.Chain(over, t.f.mount.head["metadata_pool_block"])
    moved = {}
    for i, (b, raw) in enumerate(old):
        blk = bytearray(raw)
        struct.pack_into("<I", blk, 4, t.F + i + 1 if i + 1 < len(old) else 0)
        t.PutBlock(over, t.F + i, blk)
        t.Bit(over, t.F + i, True)
        moved[b] = t.F + i
    tail_b, tail_raw = old[-1]
    raw = bytearray(tail_raw)
    struct.pack_into("<I", raw, 4, t.F)
    t.PutBlock(over, tail_b, raw)
    for fid, (mb, mo) in t.f.rows.items():
        t.Row(over, fid, (moved.get(mb, mb), mo))
    Reseal(t, over, registry=False, copies=(0,) if lie else (0, 1), first=t.F,
           edit=NewerCycle(ondisk.RECORD_HEADER) if lie else None)
    return [t.F + i for i in range(len(old))], [b for b, _r in old]


def PlantUseChain(t, over, broken, named=False):
    """A Use Context chain of two links at F and F+1. `broken`: unnamed, the
    second link's index is wrong; named, the second link carries another pair,
    a write that never landed. `named`: copy 0 names the chain."""
    text = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    crc = ondisk.Crc32(text)
    parts = [text[:ondisk.TUSE_TEXT], text[ondisk.TUSE_TEXT:]]
    for i, part in enumerate(parts):
        link = bytearray(BLOCK)
        index = 5 if broken and not named and i == 1 else i
        seq = 2 if broken and named and i == 1 else 1
        nxt = t.F + 1 if i == 0 else 0
        link[:ondisk.TUSE_LINK.size] = ondisk.TUSE_LINK.struct.pack(
            ondisk.TUSE_MAGIC, nxt, seq, 1, index, len(part), 2, len(text), crc)
        link[ondisk.TUSE_LINK.size:ondisk.TUSE_LINK.size + len(part)] = part
        t.PutBlock(over, t.F + i, link)
        t.Bit(over, t.F + i, True)
    if named:
        t.Ledger(over, 0, lambda raw: raw.__setitem__(
            slice(ondisk.LEDGER_USE_CHAIN_AT, ondisk.LEDGER_USE_CHAIN_END),
            ondisk.USE_CHAIN.struct.pack(t.F, 1, 1, len(text), crc)))


def PlantSpare(t, over):
    spare = bytearray(BLOCK)
    struct.pack_into("<I", spare, 0, ondisk.MPOOL_MAGIC)
    t.PutBlock(over, t.F, spare)
    t.Bit(over, t.F, True)


def PlantManifest(t, over):
    """A snapshot manifest at F with no snapshots, named by both copies."""
    raw = bytearray(BLOCK)
    struct.pack_into("<II", raw, 0, ondisk.COW_MANIFEST_MAGIC, 0)
    t.PutBlock(over, t.F, raw)
    t.Bit(over, t.F, True)
    for c in (0, 1):
        t.Ledger(over, c, LedgerField("cow_manifest_block", t.F))


def PlantRotten(t, over, taken):
    """S's newest record rotten inside the sealed span: S written again to
    block F and that record's sum broken. `taken`: first U written again onto
    S's old block — the block S let go, taken — so the older record S stands
    on names U's block."""
    if taken:
        PlantAppend(t, over, RecordOf(t, over, t.Urec, block=t.Sruns[0][0]))
    PlantAppend(t, over, RecordOf(t, over, t.Srec, block=t.F, rot=True))
    t.Bit(over, t.F, True)
    Reseal(t, over, registry=False)


def PlantRottenSettled(t, over):
    """S's only record rotten inside the sealed span, and the seal made again
    over the rotten bytes: the span is what the seal says, and holds no record
    that reads of a file the stood Ledger had issued — a file that settled,
    lost, as a block let go with its record still a file's leaves it."""
    last = t.Srec.at + t.Srec.len - 1
    t.RecordPatch(over, t.Srec, t.Srec.len - 1, bytes([t.Block(over, t.Srec.block)[last] ^ 1]), False)
    Reseal(t, over, registry=False)


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


def PlantFrame(t, over, frame_bytes=ondisk.VOLUME_FRAME.size, keep_layout=False, **edit):
    """The head Deed made again with a FRAME — the one a maker of format v2
    lays down, with `edit`'s fields changed, `frame_bytes` long — in place of
    its LAYOUT, or beside it when `keep_layout`: a Deed claiming both formats."""
    raw = bytearray(over.sectors(t.base, ondisk.DEED_SECTORS))
    d = ondisk.ReadDeed(bytes(raw))
    grain = 256
    frame = dict(data_block=grain * (1 + ondisk.FRAME_SPINE_SLOTS), spine_block=grain,
                 spine_slots=ondisk.FRAME_SPINE_SLOTS, spine_stride=grain)
    frame.update(edit)
    payload = ondisk.VOLUME_FRAME.struct.pack(*(frame[k] for k in ondisk.VOLUME_FRAME.names))
    stamps = b""
    for kind, body in list(d["stamps"].items()) + [(ondisk.STAMP_FRAME, payload.ljust(frame_bytes, b"\0"))]:
        if kind == ondisk.STAMP_LAYOUT and not keep_layout:
            continue
        stamps += struct.pack("<HH", kind, len(body)) + body
        stamps = stamps.ljust((len(stamps) + 3) & ~3, b"\0")
    pb = d["prologue_bytes"]
    raw[pb:] = stamps.ljust(len(raw) - pb, b"\0")
    struct.pack_into("<H", raw, 10, len(stamps))
    over.Put(t.base, ondisk.DeedSeal(raw))


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
             ("format v2", "no book is written", "knows more"))):
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
    # Deed that has none fell over on the first write into the ground.
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
    want = [("deed", "head deed"), ("unowned", "Spine slot 1"), ("unowned", "front matter"),
            ("unowned", "data run block"), ("outside", "boot code")]
    ok = len(named) == len(want) and all(
        len(n) == 1 and n[0][0] == kind and what in n[0][1] for n, (kind, what) in zip(named, want))
    out.append(("F4 a cut of a format-v2 volume", ok,
                "; ".join("%s %s" % n[0] if n else "nothing" for n in named)))
    return out


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


def Kinds(c):
    return {x.kind for x in c.claims}


class Case:
    """A plant, the codes it must raise, the other damage or leak codes it may
    raise, and the facts about the census and the mount that must hold."""

    def __init__(self, name, plant, must=(), may=(), checks=()):
        self.name, self.plant = name, plant
        self.must, self.may, self.checks = set(must), set(may), list(checks)


# More than the 32 runs of each kind the kernel's mount names one by one
# (block_map_mount.c, MapTallySay); the rest it counts in one line.
ORPHAN_RUNS = 40
FSCK_RUNS_NAMED = 32


def Cases(t):
    c0, S, U, L, F = t.c0, t.S, t.U, t.L, t.F
    Sfid, Ufid, maxfid = S["fid"], U["fid"], t.maxfid
    stand = c0.f.ledger["copy"]
    other = 1 - stand
    maxtag = max(c0.by_id)
    seqs = [c["seq"] for c in c0.f.ledgers]
    tie_rule = 0 if seqs[0] >= seqs[1] else 1
    head = c0.f.mount.head
    pool0, registry0, table0 = (c0.f.chains[k] for k in ("pool", "registry", "table"))
    rows0 = len(c0.f.rows)
    tail_runs = RunsNormal([r for fid in t.tail_files
                            for r in c0.files[fid]["record"].extents_at_mount])

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

    older = LastRecord(t, pool0[0])
    broken = LastRecord(t, t.Srec.block)
    broken_tail = (ondisk.PoolUsed(t.Block(OverlayMedium(c0.f.medium), broken.block)) -
                   (broken.at - ondisk.CHAIN_HEADER))

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
    ] + GroundCases(t)


def GroundCases(t):
    return [
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


def Reconcile(t, over):
    """C1 at F and F+1, C12 at F+3 (its registry sealed), C13 and C9 at once:
    what logcheck boots. The retired record leaves no seal borne out, so the
    mount reads the chain whole and the table says which files there are."""
    PlantOrphan(t, over, t.F)
    PlantOrphan(t, over, t.F + 1)
    PlantRegistrySplit(t, over, t.F + 3)
    Reseal(t, over, pool=False)
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
        except (KeyError, IndexError, TypeError, AttributeError):
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


def Defines(text, known=None):
    """Every #define in `text` whose value can be read: a number, a char, a
    shift, a product, or the name of one defined above it here or in `known`
    — the headers read before it, so an alias resolves to its owner."""
    out = {}
    for name, value in _DEFINE.findall(text):
        v = _Value(value, dict(known or {}, **out))
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
    """The format's numbers held against the headers.

    ‼ THE KERNEL'S HEADERS ARE READ LAST, so theirs is the word ondisk.py is
    held to. The UEFI loader keeps copies of some of the same numbers
    (tagfs_boot.h), and when it was read last its copy was what got checked —
    a loader that agreed with ondisk.py hid a kernel that did not. Now the
    loader is read first, and each number it shares with the kernel is held to
    the kernel's on its own line."""
    inc, fs = os.path.join(src, "include"), os.path.join(src, "kernel", "tagfs")
    paths = {"boot": os.path.join(src, "boot", "uefi", "tagfs_boot.h"),
             "magic": os.path.join(inc, "boxos_magic.h"), "deed": os.path.join(inc, "volume_deed.h"),
             "ledger": os.path.join(inc, "volume_ledger.h"), "pen": os.path.join(inc, "deed_pen.h"),
             "tagfs": os.path.join(fs, "tagfs.h"), "consts": os.path.join(fs, "tagfs_constants.h"),
             "cow": os.path.join(fs, "cow", "cow.h"), "book": os.path.join(fs, "disk_book", "disk_book.h"),
             "pool": os.path.join(fs, "metadata_pool", "meta_pool_internal.h"),
             "plot": os.path.join(fs, "plot", "plot_width.h"),
             "ground": os.path.join(src, "kernel", "core", "boardroom", "ground.c"),
             "groundh": os.path.join(src, "kernel", "core", "boardroom", "ground.h")}
    if not all(os.path.exists(p) for p in paths.values()):
        return None, "self-test needs the source tree beside the tool (src/ next to tools/)"
    text = {k: open(p, encoding="utf-8", errors="replace").read() for k, p in paths.items()}
    loader, kernel = Defines(text["boot"]), {}
    for k in text:
        if k != "boot":
            kernel.update(Defines(text[k], kernel))
    d = dict(loader, **kernel)
    wrong, n = [], 0

    def Eq(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            wrong.append("%s: the header says %r and ondisk.py %r" % (name, want, have))

    for name in sorted(set(loader) & set(kernel)):
        n += 1
        if loader[name] != kernel[name]:
            wrong.append("%s: tagfs_boot.h says %r and the kernel's headers %r"
                         % (name, loader[name], kernel[name]))

    magic = lambda p: bytes(d["%s_MAGIC_%u" % (p, i)] for i in range(8))
    Eq("VOLUME_DEED_MAGIC", ondisk.DEED_MAGIC, magic("VOLUME_DEED"))
    Eq("VOLUME_LEDGER_MAGIC", ondisk.LEDGER_MAGIC, magic("VOLUME_LEDGER"))
    for py, c in (("SECTOR", "VOLUME_DEED_SECTOR_BYTES"), ("DEED_ROLE_HEAD", "VOLUME_DEED_ROLE_HEAD"),
                  ("DEED_ROLE_TAIL", "VOLUME_DEED_ROLE_TAIL"), ("STAMP_GEOMETRY", "VOLUME_STAMP_GEOMETRY"),
                  ("STAMP_LAYOUT", "VOLUME_STAMP_LAYOUT"), ("STAMP_BORN", "VOLUME_STAMP_BORN"),
                  ("STAMP_BOOT", "VOLUME_STAMP_BOOT"), ("STAMP_PACKED", "VOLUME_STAMP_PACKED"),
                  ("STAMP_FRAME", "VOLUME_STAMP_FRAME"), ("FRAME_SPINE_SLOTS", "VOLUME_FRAME_SPINE_SLOTS"),
                  ("SECTORS_PER_BLOCK", "VOLUME_BLOCK_SECTORS"), ("BLOCK", "TAGFS_BLOCK_SIZE"),
                  ("DEED_SECTORS", "DEED_PEN_SECTORS"), ("LEDGER_COPIES", "VOLUME_LEDGER_COPIES"),
                  ("REGISTRY_MAGIC", "TAGFS_REGISTRY_MAGIC"), ("FILETBL_MAGIC", "TAGFS_FILETBL_MAGIC"),
                  ("MPOOL_MAGIC", "TAGFS_MPOOL_MAGIC"), ("BLOCK", "VOLUME_BLOCK_BYTES"),
                  ("REGISTRY_DATA", "TAGFS_REGISTRY_DATA_SIZE"), ("MPOOL_DATA", "TAGFS_MPOOL_DATA_SIZE"),
                  ("FTABLE_PER_BLOCK", "TAGFS_FTABLE_PER_BLOCK"), ("MAX_TAG_ID", "TAGFS_MAX_TAG_ID"),
                  ("FILE_ACTIVE", "TAGFS_FILE_ACTIVE"), ("FILE_TRASHED", "TAGFS_FILE_TRASHED"),
                  ("FILE_HIDDEN", "TAGFS_FILE_HIDDEN"),
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
                  ("GROUND_MAX_PER_MEDIUM", "GROUND_MAX_PER_MEDIUM"),
                  ("TUSE_MAGIC", "USE_CHAIN_MAGIC"), ("TUSE_TEXT", "USE_CHAIN_TEXT")):
        Eq(c, getattr(ondisk, py), d.get(c))
    guid = re.search(r"g_boxos_type_guid\[16\]\s*=\s*\{(.*?)\}", text["ground"], re.S)
    Eq("g_boxos_type_guid", ondisk.BOXOS_TYPE_GUID,
       bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", guid.group(1))) if guid else None)
    rec = dict((f, o) for f, o, _s in ondisk.META_RECORD.Offsets())
    for c, field in (("META_RECORD_LEN_OFF", "record_len"), ("META_RECORD_FILE_ID_OFF", "file_id"),
                     ("META_RECORD_FLAGS_OFF", "flags"), ("META_RECORD_SIZE_OFF", "size"),
                     ("META_RECORD_TAGCOUNT_OFF", "tag_count"), ("META_RECORD_EXTCOUNT_OFF", "extent_count"),
                     ("META_RECORD_NAMELEN_OFF", "name_len"), ("META_RECORD_CRC_OFF", "crc16")):
        Eq(c, rec[field], d.get(c))
    Eq("META_RECORD_VARDATA_OFF", ondisk.META_RECORD.size, d.get("META_RECORD_VARDATA_OFF"))

    structs = {}
    for k in ("deed", "ledger", "tagfs", "plot", "cow", "book"):
        structs.update(Structs(text[k], d))
    for cname, fields in (("VolumeDeed", ondisk.VOLUME_DEED), ("VolumeGeometry", ondisk.VOLUME_GEOMETRY),
                          ("VolumeLayout", ondisk.VOLUME_LAYOUT), ("VolumeBoot", ondisk.VOLUME_BOOT),
                          ("VolumeBorn", ondisk.VOLUME_BORN), ("VolumePacked", ondisk.VOLUME_PACKED),
                          ("VolumeFrame", ondisk.VOLUME_FRAME),
                          ("VolumeLedger", ondisk.VOLUME_LEDGER), ("TagRegistryBlock", ondisk.REGISTRY_BLOCK),
                          ("FileTableBlock", ondisk.FILE_TABLE_BLOCK), ("FileTableEntry", ondisk.FILE_TABLE_ENTRY),
                          ("MetaPoolBlock", ondisk.META_POOL_BLOCK), ("FileExtent", ondisk.FILE_EXTENT),
                          ("CowSnapshotDisk", ondisk.COW_SNAPSHOT_DISK),
                          ("DiskBookSuperblock", ondisk.DISKBOOK_SUPERBLOCK),
                          ("DiskBookEntry", ondisk.DISKBOOK_ENTRY),
                          ("UseChainLink", ondisk.TUSE_LINK)):
        c_fields = {f: (o, s) for f, o, s in structs.get(cname, [])}
        for field, off, size in fields.Offsets():
            Eq("%s.%s" % (cname, field), (off, size), c_fields.get(field))
    ledger = {f: o for f, o, _s in structs.get("VolumeLedger", [])}
    Eq("VOLUME_LEDGER_REQUIRED_BYTES", ondisk.LEDGER_REQUIRED_BYTES, ledger.get("use_context_offset"))
    for field, off, _size in ondisk.USE_CHAIN.Offsets():
        Eq("VolumeLedger.use_chain_%s" % field, ondisk.LEDGER_USE_CHAIN_AT + off,
           ledger.get("use_chain_%s" % field))
    return wrong, n


# ---------------------------------------------------------------------------
# X2: the owners of the data run, held against the header and heard
# ---------------------------------------------------------------------------


def OwnersCheck(src, heard):
    """What is wrong with ground_owners.py: not what tools/gen_ground_owners.py
    writes out of src/include/ground_owner.h today, or an owner the header
    names that no case's census heard claim a block."""
    gen = os.path.join(os.path.dirname(src), "tools", "gen_ground_owners.py")
    spec = importlib.util.spec_from_file_location("gen_ground_owners", gen)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    wrong = []
    with open(module.OUT) as fh:
        if fh.read() != module.Text(module.Owners()):
            wrong.append("%s is stale against %s — run tools/gen_ground_owners.py"
                         % (module.OUT, module.HEADER))
    unheard = [name for name, _alone in GROUND_OWNERS if name not in heard]
    if unheard:
        wrong.append("no case's census heard %s claim a block" % ", ".join(unheard))
    return wrong


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
    heard = set()
    for case in Cases(t):
        over = OverlayMedium(medium)
        case.plant(over)
        census = GroundCensus(over)
        ok, codes, wrong = Judge(case, census)
        heard |= Kinds(census)
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
    for name, ok, said in FrameCases(t, medium):
        total += 1
        passed += ok
        print("self-test: %s — %s — %s" % (name, said, "as expected" if ok else "NOT so"))
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
    wrong = OwnersCheck(src, heard)
    total += 1
    passed += not wrong
    print("self-test: X2 ground owners — the %u owners ground_owners.py lists are the header's, and "
          "each is heard in a case — %s" % (len(GROUND_OWNERS), "as expected" if not wrong
                                           else "; ".join(wrong)))
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
