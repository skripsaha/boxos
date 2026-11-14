"""selftest_plants — the aims and the plants of volume-read's self-test.

Targets chooses what the cases aim at off C0's census; each Plant* lays one
kind of damage, or one state a kernel cycle leaves, over an OverlayMedium —
never the image file. selftest_cases.py holds what each plant must make the
census and the mount say."""

import struct

from . import ondisk
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK, U16, U32
from .medium import OverlayMedium, Unreadable
from .ondisk import RunsNormal, RunsSubtract


class Targets:
    """What the cases aim at, chosen off C0's census.

    S — the live file with the fewest blocks (at least one), lowest fid first,
        that has exactly one record in the pool, carries a tag and a name, is
        not the BOOT file, and whose record lies in a closed block of the pool:
        a file whose loss nothing else stands in for, and whose record changed
        in place is a closed block the seal no longer bears out.
    U — the next file of one block that meets the same terms except where its
        record lies: the file a block S let go is taken by. Its case writes U
        again over S's old block and reseals, so U's own record may stand in
        the open tail, where an image's last records fall as it grows.
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
        aimed = [e for e in live if e["record"].Blocks() >= 1 and counts.get(e["fid"]) == 1 and
                 e["record"].tags and e["record"].name and
                 ondisk.RECORD_HEADER <= e["record"].len and
                 "fid %u '%s'" % (e["fid"], e["record"].name) != c0.boot_file]
        ok = [e for e in aimed if e["record"].block != tail_block]
        if not ok or len(live) < 2:
            raise Unreadable("the image has no live file a case can aim at")
        self.S = min(ok, key=lambda e: (e["record"].Blocks(), e["fid"]))
        ones = [e for e in aimed if e is not self.S and e["record"].Blocks() == 1]
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


def PlantBitmapAway(t, over):
    """The head deed's layout moves the block bitmap past the medium's end,
    and the deed is sealed again: every other region stays where it is."""
    raw = bytearray(over.sectors(t.base, ondisk.DEED_SECTORS))
    at = ondisk.DeedStampAt(raw, ondisk.STAMP_LAYOUT)
    off = dict((f, o) for f, o, _s in ondisk.VOLUME_LAYOUT.Offsets())["block_bitmap_block"]
    struct.pack_into("<I", raw, at + off, t.lay["total_blocks"] + 64)
    over.Put(t.base, ondisk.DeedSeal(raw))


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
