"""selftest_book_medium — the medium under a book: the plants that say its
ground again in a partition table, and a blklogwrites log built in memory of
what a machine wrote to it.
"""

import struct

from . import blklog, ondisk
from .ondisk import SECTOR


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
