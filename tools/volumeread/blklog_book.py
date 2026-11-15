"""blklog_book — what a write of a machine on a volume touched
(blklog.Classifier, for a book): every block of the volume is an owner's or
free, and the record says which without a word from the machine.

A book writes three kinds of unit, and every sector of each carries its own
head (volume_book.h BookSectorHead: kind, seq, at of `of`, a CRC32C seeded by
the volume's uuid and the sector's place): a sector that holds there IS a
stitch's, a Tally node's or a Spine slot's, whoever names it yet. So the
book's own writes are named by their heads, and need no census at all — a
Tally's nodes are written before the slot that names them, a stitch before
the next that goes on from it.

What carries no head is a file's data, and fresh data is written BEFORE the
stitch that names it (§3.3: data, FLUSH, the stitch, FLUSH): the census as of
the write's own epoch does not know it yet. So the record's own stitches are
read first — every one whose every sector the record holds, parsed (stitch.py
Parsed) — and a block of data is the file's whose Born or Shaped names it in
the first stitch to land after it. Only what no stitch of the record names is
asked of the census as of the write's epoch (bookcensus.BookCensus): a file's
data, ground the ribbon let go, ground held for what rotted — or ground free,
which a book's census knows as surely as its owners (what none of them names
is free, and there is no third state): fresh data no stitch of the record
names at all, free at once (§3.4 L-8: a Stake let go before the S0 of the
cycle that would have named it). Its file was written again before any
stitch named it — each Ink of a churn between two cycles names its new bytes
on fresh ground of their own and lets the last go, and the cycle's stitch
carries only the record its file ends on — or the record ended before the
stitch that would have: the power went, or no cycle ran. The record cannot
tell such data from data a stitch should have named and did not; that is
the census's to see (a file whose runs do not cover its size, BASE_MISMATCH)
and the reading of what the file holds. What neither names — a census that
does not read, a book that does not mount, whose owners it cannot name, free
ground it does not count (a bookmark's), ground past the end its head Deed
names (a growth's, before the head) — is unowned, and on a record of this
kernel's a defect.

The front of the volume is the front, the Spine's places its slots; a slot
that does not hold is still the slot.
"""

from . import bookread, stitch, tally
from .bookread import HEAD_BYTES, SECTOR_HEAD, UNIT_SHEET, UNIT_SLOT, UNIT_STITCH

BOOK_KINDS = ("stitch", "sheet", "slot", "front", "fallow", "held", "boot", "free")
_UNIT_KIND = {UNIT_STITCH: "stitch", UNIT_SHEET: "sheet", UNIT_SLOT: "slot"}
_CENSUS_KIND = {"file": "file", "boot": "boot", "fallow": "fallow", "held": "held",
                "tally": "sheet", "ribbon": "stitch", "front": "front", "spine": "slot", "far_deed": "deed"}


class BookSectors:
    """The record's writes on a book, named. `stand` is the volume the base
    stands on (medium.StandOnDeed); `census_at(epoch)` the BookCensus of the
    base and the record to the end of that epoch, or a false value;
    `sectors_of(w)` how far the volume runs for write w — a growth the record
    made before it included (blklog.Classifier._Sectors)."""

    def __init__(self, log, stand, census_at, sectors_of):
        self.log, self.st, self.census_at, self.sectors_of = log, stand, census_at, sectors_of
        self.uuid, self.frame = stand.deed["uuid"], stand.deed["frame"]
        self.named = {}       # block -> [(landed, seq, fid)], landed the log seq of the stitch's last sector
        self.stitches = 0     # stitches of the record read whole
        self._Stitches()

    def _Head(self, raw, vlba):
        if raw[4:HEAD_BYTES] == bytes(HEAD_BYTES - 4) or not bookread.SectorCheck(raw, self.uuid, vlba):
            return None
        return SECTOR_HEAD.Unpack(raw)

    def _Stitches(self):
        """Every stitch the record holds every sector of, as it last lay: the
        blocks of fresh data its Born and Shaped records name."""
        sectors = {}          # (seq, at) -> (log seq, payload, of)
        for w in self.log.Writes():
            if w.discard:
                continue
            for i in range(w.sectors):
                vlba = w.lba + i - self.st.start
                if vlba < 0 or vlba >= self.sectors_of(w):
                    continue
                h = self._Head(bytes(self.log.SectorData(w, i)), vlba)
                if h and h["kind"] == UNIT_STITCH:
                    sectors[(h["seq"], h["at"])] = (w.seq, bytes(self.log.SectorData(w, i))[HEAD_BYTES:], h["of"])
        by_seq = {}
        for (seq, at), held in sectors.items():
            by_seq.setdefault(seq, {})[at] = held
        for seq, parts in by_seq.items():
            of = next(iter(parts.values()))[2]
            if sorted(parts) != list(range(of)):
                continue
            p = stitch.Parsed(b"".join(parts[k][1] for k in range(of)))
            if p.why is not None:
                continue
            self.stitches += 1
            landed = max(parts[k][0] for k in range(of))
            for kind, _ign, body, _at in p.records:
                self._Fresh(kind, body, seq, landed)

    def _Fresh(self, kind, body, seq, landed):
        if kind not in (stitch.BORN, stitch.SHAPED):
            return
        try:
            d = stitch.RecordDecode(kind, body, seq)
        except (bookread.Short, ValueError, IndexError):
            return
        runs = d["runs"] if kind == stitch.BORN else [x for _a, _m, rs in d["splices"] for x in rs]
        for x in runs:
            if x.hole or tally.EXT_SHARED in stitch.Extensions(x.ext):
                continue
            for b in range(x.start, x.start + x.count):
                self.named.setdefault(b, []).append((landed, seq, d["fid"]))

    def Sector(self, w, i, vb):
        """(kind, what, group) for sector i of write w, in volume block vb —
        neither the head Deed nor its far copy."""
        vlba = w.lba + i - self.st.start
        h = self._Head(bytes(self.log.SectorData(w, i)), vlba)
        if h:
            kind = _UNIT_KIND[h["kind"]]
            what = {"stitch": "stitch seq %u", "sheet": "Tally node seq %u",
                    "slot": "Spine slot of seq %u"}[kind] % h["seq"]
            if h["of"] > 1:
                what = "%s, %u sector(s)" % (what, h["of"])
            return kind, what, (kind, h["seq"])
        f = self.frame
        if f is None:
            return "unowned", "volume block %u of a volume whose FRAME is short" % vb, ("unowned", vb)
        k = vb - f["spine_block"]
        if f["spine_stride"] and k >= 0 and k % f["spine_stride"] == 0 and k // f["spine_stride"] < f["spine_slots"]:
            return "slot", "Spine slot %u, a sector that does not hold" % (k // f["spine_stride"]), ("slot", vb)
        if vb < f["data_block"]:
            return "front", "front matter block %u" % vb, ("front",)
        return self._Data(w, vb)

    def _Data(self, w, vb):
        later = [n for n in self.named.get(vb, ()) if n[0] > w.seq]
        if later:
            _landed, seq, fid = min(later)
            return "file", "fid %u data, named by stitch seq %u" % (fid, seq), ("file", fid, seq)
        census = self.census_at(w.epoch)
        if census:
            for c in census.claims:
                if c.kind in _CENSUS_KIND and any(s <= vb < s + n for s, n in c.runs):
                    kind = _CENSUS_KIND[c.kind]
                    return kind, "%s, as of its epoch" % c.label, (kind, c.label)
            if census.mount.refusal is None and census.counted and \
                    any(s <= vb < s + n for s, n in census.free_runs):
                return "free", "data run block %u, free ground as of its epoch: data no stitch of the record " \
                    "names" % vb, ("free",)
        return "unowned", "data run block %u: no stitch of the record names it, nor any owner as of its " \
            "epoch, nor free ground of a book that mounts and counts it" % vb, ("unowned", "data")
