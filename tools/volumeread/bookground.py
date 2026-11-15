"""bookground — a volume's ground as its book is read off it: where the
volume stands, its uuid, its FRAME and what follows from it, and its sectors,
each read once and judged as a sector of the book.

Everything here is the book's, shared by the slots, the
Tally and the ribbon; where on the medium the volume stands is medium.py's.
"""

from . import bookread, ondisk
from .medium import StandOnDeed, Unreadable
from .ondisk import SECTOR, BLOCK, SECTORS_PER_BLOCK


class BookGround:
    """A volume on a medium: where it stands, its uuid, its FRAME and
    what follows from it, and its sectors, each read once and judged as a
    sector of the book (volume_book.h BookSectorCheck) — every one that holds
    counted into `top_seq`, for the next seq is past everything the mount saw
    hold."""

    def __init__(self, medium, forced_start=None):
        st = StandOnDeed(medium, forced_start)
        # The FRAME is the kernel's first question too (deed.c, at the same
        # point): a Deed that carries none says nothing of where anything is.
        # Whether the FRAME it carries holds is said below (frame_why).
        if ondisk.STAMP_FRAME not in st.deed["stamps"]:
            raise Unreadable("the deed at sector %u does not say where anything is — it carries no FRAME, "
                             "so its volume is no book this kernel reads" % st.start)
        self.medium, self.stand, self.deed = medium, st, st.deed
        self.base, self.uuid = st.start, st.deed["uuid"]
        self.frame_why, self.frame_note = ondisk.VolumeFrameCheck(st.deed)
        self.frame = st.deed["frame"]
        self.frame_bytes = len(st.deed["stamps"].get(ondisk.STAMP_FRAME, b""))
        geo = st.deed["geometry"] or {}
        self.block_bytes = geo.get("block_bytes", 0)
        self.grain = max(1, geo.get("grain_bytes", 0) // BLOCK)
        self.atom = max(BLOCK, geo.get("physical_bytes", 0))
        self.data_end = st.deed["tail_sector"] // SECTORS_PER_BLOCK
        self.data_block = self.frame["data_block"] if self.frame else self.data_end
        self.top_seq = 0
        self._sectors = {}

    def _Judge(self, vlba, raw):
        head = None
        # A head of zeros past its sum names no kind: no sector of the book,
        # and no CRC32C need be taken to say so.
        if raw is not None and raw[4:24] != bytes(20) and bookread.SectorCheck(raw, self.uuid, vlba):
            head = bookread.SECTOR_HEAD.Unpack(raw)
            self.top_seq = max(self.top_seq, head["seq"])
        self._sectors[vlba] = (raw, head)
        return raw, head

    def Run(self, vlba, count):
        """[(raw or None where the medium will not give it, its head when it
        holds as a book sector, else None)] of `count` sectors from `vlba`."""
        missing = [k for k in range(count) if vlba + k not in self._sectors]
        if missing:
            try:
                whole = self.medium.sectors(self.base + vlba, count)
                for k in missing:
                    self._Judge(vlba + k, whole[k * SECTOR:(k + 1) * SECTOR])
            except IOError:
                for k in missing:
                    try:
                        raw = self.medium.sectors(self.base + vlba + k, 1)
                    except IOError:
                        raw = None
                    self._Judge(vlba + k, raw)
        return [self._sectors[vlba + k] for k in range(count)]

    def Sector(self, vlba):
        return self.Run(vlba, 1)[0]

    def Unit(self, vlba, count, kind, seq=None):
        """(stream, None) when `count` sectors from `vlba` are one whole unit
        of `kind` (and of `seq`, when one is named), or (None, why) — the rule
        of volume_book.h BookUnitCheck and BookUnitStream, said once here over
        sectors judged once."""
        sectors = self.Run(vlba, count)
        first = None
        for k, (raw, head) in enumerate(sectors):
            if raw is None:
                return None, "sector %u will not read" % k
            if head is None:
                return None, "sector %u does not hold" % k
            first = first or head
            if head["kind"] != kind or head["at"] != k or head["of"] != count or head["seq"] != first["seq"]:
                return None, ("sector %u is part %u of %u of a unit of kind %u, seq %u"
                              % (k, head["at"], head["of"], head["kind"], head["seq"]))
        if seq is not None and first["seq"] != seq:
            return None, "it was sealed at seq %u where its pointer names %u" % (first["seq"], seq)
        return b"".join(raw[bookread.HEAD_BYTES:] for raw, _h in sectors), None

    def Block(self, block):
        """A block's 4096 bytes, or None where the medium will not give them."""
        try:
            return self.medium.sectors(self.base + block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK)
        except IOError:
            return None
