"""blklog — a QEMU blklogwrites log: every write a machine made, and every
state its medium could have been left in.

QEMU's blklogwrites driver (block/blklogwrites.c, read at v10.1.2 — the
version this desk runs) writes each guest write to the image AND appends it to
a log, and appends a FLUSH entry once a flush has reached the image. The log
is therefore the machine's whole write history, ordered, with the durability
points marked in it.

Reading it:
  - the log is walked from log sector 1 and stops at the first header that is
    not an entry QEMU writes — the super block's count is rewritten only on a
    flush or every interval-th entry, and a killed QEMU leaves it lagging, or
    all zero (blklogwrites.c:402-460);
  - a FLUSH closes an EPOCH: every write the guest saw complete before it
    issued the flush precedes the FLUSH entry (blklogwrites.c:373-384, 567-573);
  - the tail after the last FLUSH is the last epoch.

A crash state is `base + every write of the epochs before K + a subset of epoch
K [+ torn writes of epoch K]`, applied in log order, built as an overlay over
the base without copying it. Writes in flight across a flush may have landed
on either side of it and are counted before it, so this enumerates a SUBSET of
the reachable crash states: it can miss one, it never invents one.
"""

import bisect
import mmap
import os
import struct

from . import ondisk
from .ondisk import SECTOR, SECTORS_PER_BLOCK, BLOCK
from .medium import OverlayMedium, StandOnDeed, Unreadable, GroundCandidates
from .census import GroundCensus

LOG_FLUSH, LOG_FUA, LOG_DISCARD, LOG_MARK = 1, 2, 4, 8   # blklogwrites.c:25-28
LOG_FLAG_MASK = 0xF                                     # blklogwrites.c:29-32
LOG_MAGIC = 0x6a736677736872                            # blklogwrites.c:35 WRITE_LOG_MAGIC
LOG_VERSION = 1                                         # blklogwrites.c:34 WRITE_LOG_VERSION
LOG_SUPER = struct.Struct("<QQQI")                      # blklogwrites.c:38-43 log_write_super
LOG_ENTRY = struct.Struct("<QQQQ")                      # blklogwrites.c:45-50 log_write_entry

KINDS = ("deed", "ledger", "diskbook", "bitmap", "registry", "table", "pool",
         "integrity", "manifest", "tuse", "spare", "file", "unowned", "outside")


class LogUnreadable(Exception):
    """The log cannot be read against this base, or an argument cannot be
    honoured."""


class LogWrite:
    """One write (or discard) in the log. `seq` is its 1-based entry number,
    flushes counted; `index` its place within its epoch; `lba`/`sectors` in
    512-byte medium sectors; `data_at` the byte offset of its data in the log."""

    __slots__ = ("seq", "epoch", "index", "log_sector", "lba", "sectors", "discard", "data_at")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw[k])


class Touch:
    __slots__ = ("first_lba", "sectors", "kind", "what")

    def __init__(self, first_lba, sectors, kind, what):
        self.first_lba, self.sectors, self.kind, self.what = first_lba, sectors, kind, what


def _Valid(size):
    """blk_log_writes_sector_size_valid (blklogwrites.c:117-123)."""
    return size & (size - 1) == 0 and size >= LOG_ENTRY.size and size < (1 << 24)


class BlkLog:
    """A log, walked. `source` is a path (mapped read-only) or bytes."""

    def __init__(self, source, medium_sectors):
        if isinstance(source, (bytes, bytearray)):
            self.data = memoryview(bytes(source))
        else:
            with open(source, "rb") as fh:
                size = os.fstat(fh.fileno()).st_size
                self.data = memoryview(mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
                                       if size else b"")
        data, self.notes = self.data, []
        head = LOG_SUPER.unpack_from(data, 0) if len(data) >= LOG_SUPER.size else None
        if head is not None and head[0] == LOG_MAGIC:
            if head[1] != LOG_VERSION:
                raise LogUnreadable("the log's super block says version %u; this reads version %u"
                                    % (head[1], LOG_VERSION))
            if not _Valid(head[3]):
                raise LogUnreadable("the log's super block states a sector size of %u, which "
                                    "blklogwrites never writes" % head[3])
            self.sector_bytes, self.super_entries = head[3], head[2]
        else:
            self.sector_bytes, self.super_entries = SECTOR, None
            self.notes.append("no super block: QEMU wrote no flush before it stopped "
                              "(blklogwrites.c:402-404), so %u-byte log sectors are assumed" % SECTOR)
        if self.sector_bytes % SECTOR:
            raise LogUnreadable("a log sector of %u bytes cannot be laid onto 512-byte medium "
                                "sectors" % self.sector_bytes)
        self._Walk(medium_sectors)

    def _Walk(self, medium_sectors):
        data, ss, scale = self.data, self.sector_bytes, self.sector_bytes // SECTOR
        self.entries, self.flush_seqs, self.epochs = 0, [], [[]]
        pos, stop = 1, "end of the log"
        while True:
            off = pos * ss
            if off + LOG_ENTRY.size > len(data):
                break
            sector, nr, flags, _dlen = LOG_ENTRY.unpack_from(data, off)
            if (sector, nr, flags, _dlen) == (0, 0, 0, 0):
                stop = "a zero header at log sector %u" % pos
                break
            if flags & ~LOG_FLAG_MASK or flags & (LOG_FUA | LOG_MARK):
                stop = "flags 0x%x at log sector %u, which QEMU never writes" % (flags, pos)
                break
            if flags & LOG_FLUSH:
                if sector or nr:
                    stop = "a flush carrying an extent at log sector %u" % pos
                    break
                self.entries += 1
                self.flush_seqs.append(self.entries)
                self.epochs.append([])
                pos += 1
                continue
            discard = bool(flags & LOG_DISCARD)
            if nr == 0:
                stop = "a write of no sectors at log sector %u" % pos
                break
            data_at = (pos + 1) * ss
            if not discard and data_at + nr * ss > len(data):
                stop = "a write whose data runs past the end of the log at log sector %u" % pos
                break
            lba, sectors = sector * scale, nr * scale
            if medium_sectors is not None and lba + sectors > medium_sectors:
                raise LogUnreadable("entry %u writes sectors %u+%u and the base has %u — this "
                                    "log is not of this base" % (self.entries + 1, lba, sectors,
                                                                 medium_sectors))
            self.entries += 1
            ep = self.epochs[-1]
            ep.append(LogWrite(seq=self.entries, epoch=len(self.epochs) - 1, index=len(ep),
                               log_sector=pos, lba=lba, sectors=sectors, discard=discard,
                               data_at=None if discard else data_at))
            pos += 1 if discard else 1 + nr
        self.stop = stop
        if self.super_entries is not None and self.super_entries < self.entries:
            self.notes.append("%u entries past the super's count of %u (it is rewritten only on a "
                              "flush or every interval-th entry)"
                              % (self.entries - self.super_entries, self.super_entries))
        elif self.super_entries is not None and self.super_entries > self.entries:
            self.notes.append("the super counts %u and %u are readable — QEMU stopped with entries "
                              "in flight; the readable prefix is the log"
                              % (self.super_entries, self.entries))

    def Writes(self):
        return [w for ep in self.epochs for w in ep]

    def SectorData(self, w, i):
        if w.discard:
            return bytes(SECTOR)
        return self.data[w.data_at + i * SECTOR:w.data_at + (i + 1) * SECTOR]

    def Apply(self, over, w, mask=None):
        """Lay one write over the overlay; with `mask`, only the sectors whose
        bit is set (bit i = sector i). A discard reads back as zeros on this
        host (P3 F-E), so it is applied as zeros."""
        for i in range(w.sectors):
            if mask is None or mask >> i & 1:
                over.over[w.lba + i] = self.SectorData(w, i)

    def Epoch(self, k):
        if not 0 <= k < len(self.epochs):
            raise LogUnreadable("there is no epoch %u; this log has %u (0..%u)"
                                % (k, len(self.epochs), len(self.epochs) - 1))
        return self.epochs[k]

    def Cut(self, base, k, keep, tears):
        """base + epochs before K + the kept writes and torn writes of K, in log
        order, so the later of two overlapping kept writes wins."""
        over = OverlayMedium(base)
        for ep in self.epochs[:k]:
            for w in ep:
                self.Apply(over, w)
        for w in self.Epoch(k):
            if w.index in keep:
                self.Apply(over, w)
            elif w.index in tears:
                self.Apply(over, w, tears[w.index])
        return over

    def EpochFacts(self):
        """Per epoch: how many writes, the entry of the flush that closed it (or
        None), and whether that flush may be QEMU's own — a flush with no write
        after it in the log may be the one QEMU appends when it quits."""
        out, last_write = [], max((w.seq for w in self.Writes()), default=0)
        for k, ep in enumerate(self.epochs):
            fs = self.flush_seqs[k] if k < len(self.flush_seqs) else None
            out.append({"epoch": k, "writes": len(ep), "flush_seq": fs,
                        "maybe_qemu_exit_flush": fs is not None and fs > last_write})
        return out


# ---------------------------------------------------------------------------
# Keep sets
# ---------------------------------------------------------------------------


def ParseKeep(text, epoch, classify):
    """`all | none | prefix:N | ITEM[,ITEM...]`, ITEM = I | I-J | kind:NAME —
    indices WITHIN the epoch. `classify(w)` gives a write's touches, for kind
    terms."""
    n = len(epoch)
    if text == "all":
        return set(range(n))
    if text == "none":
        return set()
    if text.startswith("prefix:"):
        return set(range(min(n, _Int(text[7:], "prefix"))))
    keep = set()
    for item in text.split(","):
        if item.startswith("kind:"):
            kind = item[5:]
            if kind not in KINDS:
                raise LogUnreadable("no kind %r; the kinds are %s" % (kind, " ".join(KINDS)))
            keep |= {w.index for w in epoch if any(t.kind == kind for t in classify(w))}
        elif "-" in item:
            a, b = item.split("-", 1)
            keep |= set(range(_Index(a, n), _Index(b, n) + 1))
        else:
            keep.add(_Index(item, n))
    return keep


def _Int(text, what):
    try:
        return int(text, 0)
    except ValueError:
        raise LogUnreadable("%s wants a number, not %r" % (what, text))


def _Index(text, n):
    i = _Int(text, "a write index")
    if not 0 <= i < n:
        raise LogUnreadable("write %u is not in this epoch, which has %u (0..%u)" % (i, n, n - 1))
    return i


def ParseSubsetBits(text, epoch):
    mask = _Int(text, "--subset-bits")
    if mask >> len(epoch):
        raise LogUnreadable("--subset-bits 0x%x names writes past this epoch's %u" % (mask, len(epoch)))
    return {i for i in range(len(epoch)) if mask >> i & 1}


def ParseTears(items, epoch, keep):
    tears = {}
    for item in items:
        if ":" not in item:
            raise LogUnreadable("--tear wants W:MASK, not %r" % item)
        w, m = item.split(":", 1)
        i, mask = _Index(w, len(epoch)), _Int(m, "a tear mask")
        if i in keep:
            raise LogUnreadable("write %u is both kept and torn" % i)
        if mask >> epoch[i].sectors:
            raise LogUnreadable("tear mask 0x%x has bits past write %u's %u sectors"
                                % (mask, i, epoch[i].sectors))
        tears[i] = mask
    return tears


# ---------------------------------------------------------------------------
# What a write touched
# ---------------------------------------------------------------------------


class Classifier:
    """Names what each write touched, by region of the medium, by what it
    wrote, and by who owns the block in the image as of the end of its epoch."""

    def __init__(self, log, base):
        self.log, self.base = log, base
        self.over = OverlayMedium(base)
        self.done_epoch = -1
        self.census = None
        try:
            self.stand = StandOnDeed(base, None)
        except Unreadable:
            self.stand = None
        self.parts = [(s, n, how) for s, n, how, _k in GroundCandidates(base) if n and s]
        try:
            self.has_mbr = base.sectors(0, 1)[510:512] == b"\x55\xaa"
        except IOError:
            self.has_mbr = False
        self.cache = {}

    def _Owners(self, epoch):
        """The census of base + epochs 0..epoch, built incrementally."""
        while self.done_epoch < epoch:
            self.done_epoch += 1
            for w in self.log.epochs[self.done_epoch]:
                self.log.Apply(self.over, w)
            self.census = None
        if self.census is None:
            try:
                self.census = GroundCensus(self.over)
            except (Unreadable, IOError):
                self.census = False
            self.stand = self.census.f.stand if self.census else self.stand
        return self.census

    def Classify(self, w):
        if w.seq in self.cache:
            return self.cache[w.seq]
        census = self._Owners(w.epoch) if self.stand is not None else None
        touches, cur = [], None
        for i in range(w.sectors):
            kind, what, group = self._Sector(w, i, census)
            if cur is not None and cur[3] == group:
                cur[1] += 1
            else:
                cur = [w.lba + i, 1, kind, group, what]
                touches.append(cur)
        out = [Touch(t[0], t[1], t[2], self._Finish(t, w)) for t in touches]
        self.cache[w.seq] = out
        return out

    def _Finish(self, t, w):
        first, count, kind, group, what = t
        if kind in ("file", "integrity", "unowned") and group[0] == "data":
            b0 = (first - self.stand.start) // SECTORS_PER_BLOCK - self.lay["data_block"]
            b1 = (first + count - 1 - self.stand.start) // SECTORS_PER_BLOCK - self.lay["data_block"]
            return "%s, data block%s %u%s" % (what, "s" if b1 > b0 else "", b0,
                                             "..%u" % b1 if b1 > b0 else "")
        return what

    def _Outside(self, lba):
        if lba == 0 and self.has_mbr:
            return "MBR sector 0"
        for s, n, how in self.parts:
            if s <= lba < s + n:
                return how
        first = min((s for s, _n, _h in self.parts), default=None)
        if first is not None and lba < first:
            return "boot code sectors 1..%u" % (first - 1)
        return "outside every partition"

    def _Sector(self, w, i, census):
        """(kind, what, group) for sector i of write w: consecutive sectors of
        one group are one touch."""
        lba = w.lba + i
        st = self.stand
        if st is None or not st.start <= lba < st.start + st.deed["sectors"]:
            what = self._Outside(lba)
            return "outside", what, ("outside", what)
        self.lay = lay = (census.f.layout if census else st.deed["layout"])
        v = lba - st.start
        vb = v // SECTORS_PER_BLOCK
        if vb == 0 or v // SECTORS_PER_BLOCK == st.deed["tail_sector"] // SECTORS_PER_BLOCK:
            what = "head deed" if vb == 0 else "far copy of the deed"
            return "deed", what, ("deed", vb)
        if lay["state_block"] <= vb < lay["state_block"] + lay["state_blocks"]:
            c = vb - lay["state_block"]
            if v % SECTORS_PER_BLOCK:
                return "ledger", "ledger copy %u, unused sector %u" % (c, v % SECTORS_PER_BLOCK), ("ledger", vb, 1)
            d = ondisk.ReadLedgerCopy(bytes(self.log.SectorData(w, i)), c)
            what = ("ledger copy %u (seq %u, next_file_id %u, free %u, integrity map %u+%u)"
                    % (c, d["seq"], d["next_file_id"], d["free_blocks"], d["integrity_map_block"],
                       d["integrity_map_blocks"]) if d["ok"] else "ledger copy %u (%s)" % (c, d["why"]))
            return "ledger", what, ("ledger", vb, 0)
        db = lay["disk_book_block"]
        if db <= vb < db + ondisk.DISKBOOK_BLOCKS:
            if vb == db and v % SECTORS_PER_BLOCK == 0:
                h = ondisk.ReadDiskBookHead(bytes(self.log.SectorData(w, i)), (db + 2) * SECTORS_PER_BLOCK)
                return "diskbook", "DiskBook head (count %u, generation %u)" % (h["count"], h["generation"]), ("db", 0)
            if vb == db + 1:
                return "diskbook", "DiskBook head's copy", ("db", 1)
            rec = (v - (db + 2) * SECTORS_PER_BLOCK) // ondisk.DISKBOOK_SECTORS_PER_ENTRY
            return "diskbook", "DiskBook record %u" % rec, ("db", 2, rec)
        bm = lay["block_bitmap_block"]
        if bm <= vb < bm + lay["block_bitmap_blocks"]:
            k = v - bm * SECTORS_PER_BLOCK
            D = lay["data_blocks"]
            if k * SECTOR * 8 >= D:
                return "bitmap", "bitmap past the data run", ("bm", "past")
            return "bitmap", "bitmap sector %u (data blocks %u..%u)" % (
                k, k * SECTOR * 8, min(D, (k + 1) * SECTOR * 8) - 1), ("bm", k)
        b = vb - lay["data_block"]
        if 0 <= b < lay["data_blocks"]:
            kind, what = self._DataBlock(w, i, b, census)
            return kind, what, ("data", kind, what) if kind in ("file", "integrity", "unowned") \
                else ("data", kind, what, b)
        return "unowned", "volume block %u, in no region of the deed" % vb, ("gap", vb)

    def _DataBlock(self, w, i, b, census):
        v_first = (self.lay["data_block"] + b) * SECTORS_PER_BLOCK + self.stand.start
        head = None
        if w.lba <= v_first < w.lba + w.sectors:
            head = bytes(self.log.SectorData(w, v_first - w.lba))
        chains = {}
        if census:
            for c in census.claims:
                if c.runs and any(s <= b < s + n for s, n in c.runs):
                    chains.setdefault(c.kind, c.label)
        if head is not None:
            magic, nxt = struct.unpack_from("<II", head, 0)
            names = {ondisk.MPOOL_MAGIC: "pool", ondisk.FILETBL_MAGIC: "table",
                     ondisk.REGISTRY_MAGIC: "registry"}
            if magic in names:
                whole = head + bytes(BLOCK - SECTOR)
                if w.lba <= v_first and v_first + SECTORS_PER_BLOCK <= w.lba + w.sectors:
                    whole = b"".join(bytes(self.log.SectorData(w, v_first - w.lba + j))
                                     for j in range(SECTORS_PER_BLOCK))
                spare = ondisk.SpareShape(whole)
                if spare and names[magic] not in chains:
                    return "spare", "a spare %s header at data block %u" % (spare, b)
                if magic == ondisk.MPOOL_MAGIC:
                    hdr = ondisk.META_POOL_BLOCK.Unpack(head)
                    return "pool", "pool block %u (%u records, next %u)" % (b, hdr["record_count"], nxt)
                return names[magic], "%s block %u (next %u)" % (names[magic], b, nxt)
            if magic == ondisk.TUSE_MAGIC:
                return "tuse", "TUSE link at data block %u" % b
            if magic == ondisk.COW_MANIFEST_MAGIC:
                return "manifest", "snapshot manifest at data block %u" % b
        # The BOOT run is an alias of the kernel file and is named by it; a
        # snapshot's frozen block is file data that a snapshot still holds.
        for kind, name in (("file", "file"), ("frozen", "file"), ("integrity", "integrity"),
                           ("manifest", "manifest"), ("manifest_backup", "manifest")):
            if kind in chains:
                return name, chains[kind]
        for kind in ("registry", "table", "pool"):
            if kind in chains:
                return kind, "%s at data block %u" % (chains[kind], b)
        return "unowned", "no owner"


# ---------------------------------------------------------------------------
# Against
# ---------------------------------------------------------------------------


def Against(log, base, final_path):
    """base + every entry, sector for sector, against FINAL. Returns None when
    they are equal, or the sentence naming the first difference."""
    over = OverlayMedium(base)
    for w in log.Writes():
        log.Apply(over, w)
    keys = sorted(over.over)
    size = base.SizeSectors()
    with open(final_path, "rb") as fin:
        final_size = os.fstat(fin.fileno()).st_size // SECTOR
        if size is not None and final_size != size:
            return "the base runs %u sectors and %s runs %u" % (size, final_path, final_size)
        chunk, lba = 2048, 0
        while size is None or lba < size:
            n = chunk if size is None else min(chunk, size - lba)
            try:
                have = bytearray(base.sectors(lba, n))
            except IOError:
                break
            lo, hi = bisect.bisect_left(keys, lba), bisect.bisect_left(keys, lba + n)
            for k in keys[lo:hi]:
                have[(k - lba) * SECTOR:(k - lba + 1) * SECTOR] = over.over[k]
            want = fin.read(n * SECTOR)
            if bytes(have) != want:
                for s in range(n):
                    if have[s * SECTOR:(s + 1) * SECTOR] != want[s * SECTOR:(s + 1) * SECTOR]:
                        src = ("written by the log" if (lba + s) in over.over else
                               "untouched by the log")
                        return ("sector %u differs: base + log gives one thing and %s another "
                                "(the sector is %s)" % (lba + s, final_path, src))
            lba += n
    return None
