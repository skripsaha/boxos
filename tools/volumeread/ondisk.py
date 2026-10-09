"""ondisk — the TagFS format as the medium carries it, said once on the host.

Every number here is copied out of a C header or a C routine, and every one
names where it came from: a header by file and line, a routine by its name —
the lines of tagfs.c move with every step of the plan, its routines do not.
The `self-test` X1 case holds the constants and the struct layouts against the
headers themselves, so a drift there is FOUND, by a red line; a routine a
comment names is only found by reading it.

The parsers take bytes and give back plain dicts (or a Record). None of them
reads the medium: what to read, and in which order, is the caller's business —
the mount's model (mount.py) reads it the way the mounting kernel does, the cut
reads it out of a write log.

Blocks of the data run are answered as runs, [(start, count)] sorted: the
bitmap's bit order is format, and a 64 GB stick is two megabytes of bitmap
that nobody should hold as a set of Python integers.
"""

import re
import struct
import zlib

# ---------------------------------------------------------------------------
# Units
# ---------------------------------------------------------------------------

SECTOR = 512                            # volume_deed.h:112 VOLUME_DEED_SECTOR_BYTES
BLOCK = 4096                            # volume_deed.h:130 VOLUME_BLOCK_BYTES
SECTORS_PER_BLOCK = 8                   # volume_deed.h:131 VOLUME_BLOCK_SECTORS

# ---------------------------------------------------------------------------
# A packed C struct: its format and the names of its fields, in order
# ---------------------------------------------------------------------------


class Fields:
    """One packed C struct. The names are the C field names, so the X1 check
    can hold each field's offset and size against the header by name."""

    __slots__ = ("fmt", "struct", "names", "size")

    def __init__(self, fmt, names):
        self.fmt = fmt
        self.struct = struct.Struct(fmt)
        self.names = tuple(names.split())
        self.size = self.struct.size

    def Unpack(self, buf, at=0):
        return dict(zip(self.names, self.struct.unpack_from(buf, at)))

    def Offsets(self):
        """[(name, offset, size)] of every named field; `x` pads are unnamed."""
        out, at, names = [], 0, iter(self.names)
        num = ""
        for ch in self.fmt[1:]:
            if ch.isdigit():
                num += ch
                continue
            count = int(num) if num else 1
            num = ""
            if ch == "x":
                at += count
            elif ch == "s":
                out.append((next(names), at, count))
                at += count
            else:
                size = struct.calcsize("<" + ch)
                for _ in range(count):
                    out.append((next(names), at, size))
                    at += size
        return out


# ---------------------------------------------------------------------------
# The Deed
# ---------------------------------------------------------------------------

DEED_MAGIC = b"BOXDEED\0"               # volume_deed.h:95-102
DEED_SECTORS = 8                        # deed_pen.h:62 DEED_PEN_SECTORS
DEED_ROLE_HEAD, DEED_ROLE_TAIL = 1, 2   # volume_deed.h:106-107
DEED_CRC_AT = 12                        # volume_deed.h:146 VolumeDeed.crc32
STAMP_GEOMETRY, STAMP_LAYOUT, STAMP_BORN, STAMP_BOOT, STAMP_PACKED = 1, 2, 3, 4, 5  # volume_deed.h:168-172
STAMP_FRAME = 6                         # volume_deed.h:173 VOLUME_STAMP_FRAME
STAMP_HEADER = 4                        # volume_deed.h:161-164 VolumeStamp

VOLUME_DEED = Fields("<8sHHI16sQQI",    # volume_deed.h:142-151
                     "magic prologue_bytes stamp_bytes crc32 uuid sectors tail_sector role")
VOLUME_GEOMETRY = Fields("<IIII",       # volume_deed.h:196-201
                         "logical_bytes physical_bytes grain_bytes block_bytes")
VOLUME_LAYOUT = Fields("<QIIIIIIIIIIIIII",   # volume_deed.h:222-238 — format v1 only
                       "total_blocks state_block state_blocks tag_registry_block "
                       "tag_registry_blocks file_table_block file_table_blocks "
                       "metadata_pool_block metadata_pool_blocks block_bitmap_block "
                       "block_bitmap_blocks disk_book_block disk_book_blocks "
                       "data_block data_blocks")
VOLUME_BOOT = Fields("<IIII",           # volume_deed.h:254-259
                     "kernel_block kernel_blocks kernel_bytes reserved")
VOLUME_BORN = Fields("<Q16s",           # volume_deed.h:263-266
                     "created_unix maker")
VOLUME_PACKED = Fields("<QQQII",        # volume_deed.h:291-297
                       "packed_unix shipment tree file_count verdict")
VOLUME_FRAME = Fields("<QQII",          # volume_deed.h:329-334 — format v2
                      "data_block spine_block spine_slots spine_stride")
FRAME_SPINE_SLOTS = 3                   # volume_deed.h:338 VOLUME_FRAME_SPINE_SLOTS

# ---------------------------------------------------------------------------
# The Ledger
# ---------------------------------------------------------------------------

LEDGER_MAGIC = b"BOXLEDGR"              # volume_ledger.h:70-77
LEDGER_COPIES = 2                       # volume_ledger.h:83 VOLUME_LEDGER_COPIES
LEDGER_REQUIRED_BYTES = 76              # volume_ledger.h:131 offsetof(use_context_offset)
LEDGER_CRC_AT = 12                      # volume_ledger.h:88 VolumeLedger.crc32
VOLUME_LEDGER = Fields("<8sIIQQQQIIIIIIIHH",   # volume_ledger.h:85-126
                       "magic bytes crc32 seq written_unix free_blocks total_files "
                       "next_file_id next_tag_id total_tags cow_manifest_block "
                       "cow_manifest_backup_block integrity_map_block "
                       "integrity_map_blocks use_context_offset use_context_bytes")
# The seal of the last settled cycle, in the growth field right after the
# record's fields (volume_ledger.h SettleMark, LedgerHas).
SETTLE_MARK = Fields("<QIIIIIIIIIII",
                     "settle chain_first pool_from_block pool_from_bytes pool_to_block "
                     "pool_to_bytes registry_block registry_bytes registry_crc32 "
                     "closed_crc32 tail_crc32 flags")
LEDGER_SEAL_AT = VOLUME_LEDGER.size                  # offsetof(VolumeLedger, mark)
LEDGER_SEAL_END = LEDGER_SEAL_AT + SETTLE_MARK.size  # volume_ledger.h VOLUME_LEDGER_SEAL_END
# The name of the Use Context's chain, in the growth field right after the seal
# (volume_ledger.h use_chain_*, VOLUME_LEDGER_USE_CHAIN_END). first 0 names none.
USE_CHAIN = Fields("<IIQQI", "first attempt seq bytes crc32")
LEDGER_USE_CHAIN_AT = LEDGER_SEAL_END
LEDGER_USE_CHAIN_END = LEDGER_USE_CHAIN_AT + USE_CHAIN.size
SETTLE_CLEAN = 1 << 0                                # volume_ledger.h
SETTLE_MEDIUM_LIED = 1 << 1

# ---------------------------------------------------------------------------
# The three chains and the metadata record
# ---------------------------------------------------------------------------

REGISTRY_MAGIC = 0x54524547             # boxos_magic.h:12 TAGFS_REGISTRY_MAGIC "TREG"
FILETBL_MAGIC = 0x54465442              # boxos_magic.h:13 TAGFS_FILETBL_MAGIC  "TFTB"
MPOOL_MAGIC = 0x544D504C                # boxos_magic.h:14 TAGFS_MPOOL_MAGIC    "TMPL"
REGISTRY_DATA = 4080                    # tagfs_constants.h:57 TAGFS_REGISTRY_DATA_SIZE
MPOOL_DATA = 4080                       # plot/plot_width.h TAGFS_MPOOL_DATA_SIZE
FTABLE_PER_BLOCK = 510                  # tagfs_constants.h:61 TAGFS_FTABLE_PER_BLOCK
CHAIN_HEADER = 16                       # tagfs.h:48, 70, 81 — every chain header is 16 bytes

REGISTRY_BLOCK = Fields("<IIHH4x",      # tagfs.h:52-59 TagRegistryBlock header
                        "magic next_block entry_count used_bytes")
REGISTRY_RECORD = Fields("<HBBH",       # tagfs.h:49-51; tag_registry.c RegistryPack
                         "tag_id flags key_len value_len")
FILE_TABLE_BLOCK = Fields("<IIII",      # tagfs.h:71-77 FileTableBlock header
                          "magic next_block entry_count reserved")
FILE_TABLE_ENTRY = Fields("<II",        # tagfs.h:63-66 FileTableEntry
                          "meta_block meta_offset")
META_POOL_BLOCK = Fields("<IIHHH2x",    # tagfs.h:100-108 MetaPoolBlock header
                         "magic next_block used_bytes record_count kept")
FILE_EXTENT = Fields("<IH",             # plot/plot_width.h FileExtent
                     "start_block block_count")

RECORD_HEADER = 42                      # plot/plot_width.h RECORD_HEADER_SIZE
MPOOL_BLOCK_HEADER = 16                 # meta_pool.c:12
RECORD_CRC_AT = 40                      # plot/plot_width.h RECORD_CRC_OFFSET
META_RECORD = Fields("<HIIQQQHHHH",     # tagfs_boot.h:53-67; plot/plot_pack.c PackV1
                     "record_len file_id flags size created_time modified_time "
                     "tag_count extent_count name_len crc16")

FILE_ACTIVE, FILE_TRASHED, FILE_HIDDEN = 1, 2, 4   # tagfs_constants.h:44-46; bit 3 is spent (:47)
MAX_TAG_ID = 0x7FFE                     # tagfs_constants.h:34 TAGFS_MAX_TAG_ID
TAG_FLAG_HAS_VALUE, TAG_FLAG_SYSTEM = 1, 2   # tagfs_constants.h:40-41
EXTENT_END_LIMIT = 0xFFFFFFFF           # plot/plot_pack.c PlotExtentsInRun: start + count past 2^32-1 is an overflow

# ---------------------------------------------------------------------------
# DiskBook
# ---------------------------------------------------------------------------

DISKBOOK_MAGIC = 0x44424F4F             # disk_book.h:25 DISK_BOOK_MAGIC    "DBOO"
DISKBOOK_SB_MAGIC = 0x44425342          # disk_book.h:26 DISK_BOOK_SB_MAGIC "DBSB"
DISKBOOK_VERSION = 2                    # disk_book.h:27
DISKBOOK_ENTRY_BYTES = 1024             # disk_book.h:28
DISKBOOK_SECTORS_PER_ENTRY = 2          # disk_book.h:29
DISKBOOK_CAPACITY = 512                 # disk_book.h:30
DISKBOOK_TYPE_REDIRECT = 1              # disk_book.h:33
DISKBOOK_HEAD_CRC_AT = 32               # disk_book.h:43 DiskBookSuperblock.crc32
DISKBOOK_ENTRY_CRC_AT = 32              # disk_book.h:62 DiskBookEntry.record_crc32
# Head, its copy, then the records (create_tagfs.c compute_layout, disk_book.c DiskBookPartAt).
DISKBOOK_BLOCKS = 2 + DISKBOOK_CAPACITY * DISKBOOK_SECTORS_PER_ENTRY // SECTORS_PER_BLOCK
DISKBOOK_SUPERBLOCK = Fields("<IIQIIIII16s",   # disk_book.h:35-46
                             "magic version start_sector capacity count "
                             "generation flags crc32 uuid")
DISKBOOK_ENTRY = Fields("<IIHHIIIQI",   # disk_book.h:53-64
                        "magic sequence type flags snapshot_id old_block "
                        "new_block tag_bits record_crc32")

# ---------------------------------------------------------------------------
# Copy-on-write manifest, integrity map, and the Quill TUSE link
# ---------------------------------------------------------------------------

COW_MANIFEST_MAGIC = 0x434F564D         # cow.h:13 "COVM"
COW_MANIFEST_MAX = 60                   # cow.h:14
COW_MANIFEST_HEADER = 8                 # cow.h:48-51 magic + count
COW_SNAPSHOT_DISK = Fields("<IIQI32sB3xIQ",    # cow.h:34-44 CowSnapshotDisk
                           "snapshot_id parent_file_id created_time "
                           "disk_book_checkpoint name flags file_count total_size")

INTEGRITY_PER_BLOCK = 512               # integrity.c:14 ENTRIES_PER_BLOCK

# The Use Context's chain (volume_ledger.h UseChainLink; the kernel's reader is
# tagfs/use_memory/use_chain.c UseChainRead, its writer UseChainWrite).
TUSE_MAGIC = 0x54555345                 # volume_ledger.h USE_CHAIN_MAGIC, "TUSE"
TUSE_LINK = Fields("<IIQIIIIQI",
                   "magic next seq attempt index used n_links total_bytes text_crc32")
TUSE_TEXT = BLOCK - 44                  # volume_ledger.h USE_CHAIN_TEXT: 4052 bytes after the header

# ---------------------------------------------------------------------------
# Partition tables — the ground the kernel will stand on
# ---------------------------------------------------------------------------

MBR_TYPE_BOXOS = 0x7F                   # ground.c:17 GROUND_MBR_TYPE
MBR_TYPE_PROTECTIVE = 0xEE              # ground.c:27 GROUND_MBR_TYPE_PROTECTIVE
GPT_ARRAY_MAX_BYTES = 128 * 1024        # ground.c:83 GPT_ARRAY_MAX_BYTES
GPT_BLOCK_MAX_BYTES = 4096              # ground.c:122 GPT_BLOCK_MAX_BYTES
GROUND_MAX_PER_MEDIUM = 8               # ground.h:42 GROUND_MAX_PER_MEDIUM
BOXOS_TYPE_GUID = bytes([               # ground.c:19-22, tagboot.c:451-454 — as on the medium
    0x9a, 0xe4, 0x8a, 0xcf, 0x6a, 0xd2, 0x59, 0x49,
    0x9a, 0x6f, 0x71, 0xc9, 0x32, 0xe0, 0xc9, 0xeb,
])

# ---------------------------------------------------------------------------
# Checksums
# ---------------------------------------------------------------------------


def Crc32(data, crc=0):
    """CRC-32 (ISO 3309): KCrc32 (crypto.c:12-26) and DeedPenSum
    (deed_pen.h:105-115) are both exactly zlib's. `crc` continues a sum."""
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def _Crc16Table():
    table = []
    for byte in range(256):
        crc = byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
        table.append(crc & 0xFFFF)
    return tuple(table)


_CRC16 = _Crc16Table()


def Crc16(data):
    """CRC-16/CCITT-FALSE, the sum over a metadata record: KCrc16
    (crypto.c:33-47) — polynomial 0x1021, init 0xFFFF, no reflection. Table
    driven with identical results: a pool of a hundred thousand records summed
    bit by bit in Python takes tens of seconds."""
    crc = 0xFFFF
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC16[(crc >> 8) ^ b]
    return crc


def U32(buf, at):
    """A little-endian u32 at `at`, reading zeros past the end of `buf` — the
    kernel reads what lies beyond as stack, which is nothing a host can know,
    and the walks that do so never act on it (see MirrorWalk)."""
    return struct.unpack("<I", bytes(buf[at:at + 4]).ljust(4, b"\0"))[0]


def U16(buf, at):
    return struct.unpack("<H", bytes(buf[at:at + 2]).ljust(2, b"\0"))[0]


# ---------------------------------------------------------------------------
# Deed
# ---------------------------------------------------------------------------


def ReadDeed(raw):
    """A Deed from the eight sectors it occupies, every check the kernel makes
    that does not need to know where it was read from (deed.c:34-95), and its
    stamps walked exactly as DeedStamp walks them (deed.c:239-275).

    `ok` is magic + bounds + checksum; the role, the ground and the tail are
    judged by the caller (medium.StandOnDeed), because they are questions about
    where the copy sits, not about the copy."""
    d = {"ok": False, "why": None, "crc_ok": False, "stamps": {},
         "walk_error": None, "layout": None, "geometry": None,
         "boot": None, "born": None, "packed": None, "frame": None}
    if len(raw) < VOLUME_DEED.size or raw[:8] != DEED_MAGIC:
        d["why"] = "carries no deed"
        return d
    d.update(VOLUME_DEED.Unpack(raw))
    pb, sb = d["prologue_bytes"], d["stamp_bytes"]
    if pb < VOLUME_DEED.size or pb > len(raw) or sb > len(raw) - pb:
        d["why"] = ("states a prologue of %u and %u bytes of stamps, which do "
                    "not fit the %u it lives in" % (pb, sb, len(raw)))
        return d
    probe = bytearray(raw[:pb + sb])
    probe[DEED_CRC_AT:DEED_CRC_AT + 4] = b"\0\0\0\0"
    have = Crc32(bytes(probe))
    d["crc_ok"] = have == d["crc32"]
    if not d["crc_ok"]:
        d["why"] = ("does not match its own checksum (0x%08x against 0x%08x)"
                    % (have, d["crc32"]))
        return d

    left, at = sb, pb
    while left >= STAMP_HEADER:
        kind, nbytes = struct.unpack_from("<HH", raw, at)
        if nbytes > left - STAMP_HEADER:
            d["walk_error"] = ("a stamp of kind %u claims %u bytes with %u left"
                               % (kind, nbytes, left - STAMP_HEADER))
            break
        # DeedStamp answers the FIRST stamp of a kind.
        d["stamps"].setdefault(kind, bytes(raw[at + STAMP_HEADER:at + STAMP_HEADER + nbytes]))
        step = (STAMP_HEADER + nbytes + 3) & ~3
        if step > left:
            break
        at += step
        left -= step

    for key, kind, fields in (("layout", STAMP_LAYOUT, VOLUME_LAYOUT),
                              ("geometry", STAMP_GEOMETRY, VOLUME_GEOMETRY),
                              ("boot", STAMP_BOOT, VOLUME_BOOT),
                              ("born", STAMP_BORN, VOLUME_BORN),
                              ("packed", STAMP_PACKED, VOLUME_PACKED),
                              ("frame", STAMP_FRAME, VOLUME_FRAME)):
        payload = d["stamps"].get(kind)
        # Each is used only when the stamp is at least as long as the struct
        # this build knows (tagfs.c volume_take_ground; deed.c:296-329).
        if payload is not None and len(payload) >= fields.size:
            d[key] = fields.Unpack(payload)
    d["ok"] = True
    return d


def DeedFormatOf(d):
    """DeedFormatOf (deed.c): which format a Deed's book is in, by the stamps
    it carries — "v1" (LAYOUT), "v2" (FRAME), "both", or "none"."""
    layout, frame = STAMP_LAYOUT in d["stamps"], STAMP_FRAME in d["stamps"]
    return "both" if layout and frame else "v2" if frame else "v1" if layout else "none"


def VolumeFrameCheck(d):
    """VolumeFrameCheck (deed.c), the one rule in the kernel's words: (why, note)
    for the FRAME a Deed carries — `why` None when it holds, `note` what a
    FRAME that holds still has to say (a longer one, a ring of fewer slots)."""
    payload = d["stamps"].get(STAMP_FRAME)
    if payload is None or len(payload) < VOLUME_FRAME.size:
        return "it is shorter than a FRAME", None
    f = VOLUME_FRAME.Unpack(payload)
    if f["spine_slots"] < 2:
        return "its Spine ring has fewer than two slots, which is no ring", None
    if f["spine_stride"] == 0:
        return "its Spine slots are all one block — a ring with no stride", None
    if f["spine_block"] == 0:
        return "its Spine begins on block 0, which is the Deed's own", None
    last = f["spine_block"] + (f["spine_slots"] - 1) * f["spine_stride"]
    if last > 0xFFFFFFFFFFFFFFFF:
        return ("its Spine ring runs past the last block number there is and comes "
                "round onto the blocks before it"), None
    if last >= f["data_block"]:
        return "its Spine ring runs into the data run", None
    if f["data_block"] >= d["tail_sector"] // SECTORS_PER_BLOCK:
        return ("its data run starts at or past the volume's last block, where the far "
                "copy of the Deed sits"), None
    longer = len(payload) > VOLUME_FRAME.size
    fewer = f["spine_slots"] < FRAME_SPINE_SLOTS
    if longer and fewer:
        return None, ("it is longer than the FRAME this kernel knows and its ring keeps "
                      "fewer slots than this format lays down — written by a kernel that "
                      "knows more, or by another maker")
    if longer:
        return None, "it is longer than the FRAME this kernel knows — written by a kernel that knows more"
    if fewer:
        return None, "its ring keeps fewer slots than this format lays down — a volume made by another maker"
    return None, None


def DeedSeal(raw):
    """DeedPenSeal (deed_pen.h:314-325): the crc over the prologue with its own
    field zeroed plus exactly stamp_bytes of stamps. Used by the self-test to
    plant a Deed the kernel will believe."""
    raw = bytearray(raw)
    pb, sb = struct.unpack_from("<HH", raw, 8)
    raw[DEED_CRC_AT:DEED_CRC_AT + 4] = b"\0\0\0\0"
    struct.pack_into("<I", raw, DEED_CRC_AT, Crc32(bytes(raw[:pb + sb])))
    return bytes(raw)


def DeedStampAt(raw, kind):
    """Byte offset of the first stamp of `kind`'s payload inside a Deed, or None
    — for planting a changed stamp in place (the self-test's C11)."""
    pb, sb = struct.unpack_from("<HH", raw, 8)
    left, at = sb, pb
    while left >= STAMP_HEADER:
        k, nbytes = struct.unpack_from("<HH", raw, at)
        if nbytes > left - STAMP_HEADER:
            return None
        if k == kind:
            return at + STAMP_HEADER
        step = (STAMP_HEADER + nbytes + 3) & ~3
        if step > left:
            return None
        at += step
        left -= step
    return None


# ---------------------------------------------------------------------------
# Ledger
# ---------------------------------------------------------------------------


def ReadLedgerCopy(sector, copy):
    """One Ledger copy, read as ledger_read_copy (ledger.c) reads it:
    magic, a length inside [76, 512], the crc over exactly that length, zeros
    past a short record, the name of the Use Context's chain where the record
    reaches it (LedgerHas), and otherwise the text a kernel before the chain
    kept, only if it lies inside the record. `why` carries the kernel's own
    words for a refusal."""
    rec = bytearray(sector[:SECTOR].ljust(SECTOR, b"\0"))
    d = VOLUME_LEDGER.Unpack(rec)
    d.update(copy=copy, ok=False, why=None, use_context=None, use_misplaced=False,
             use_chain=None)
    if d["magic"] != LEDGER_MAGIC:
        d["why"] = "carries no ledger"
        return d
    n = d["bytes"]
    if n < LEDGER_REQUIRED_BYTES or n > SECTOR:
        d["why"] = "states a length of %u, which cannot be one" % n
        return d
    probe = bytearray(rec[:n])
    probe[LEDGER_CRC_AT:LEDGER_CRC_AT + 4] = b"\0\0\0\0"
    have = Crc32(bytes(probe))
    if have != d["crc32"]:
        d["why"] = ("does not match its own checksum (0x%08x against 0x%08x)"
                    % (have, d["crc32"]))
        return d
    if n < SECTOR:
        rec[n:SECTOR] = bytes(SECTOR - n)
        d.update(VOLUME_LEDGER.Unpack(rec))
        d.update(copy=copy, why=None, use_context=None, use_misplaced=False, use_chain=None)
    d["mark"] = SETTLE_MARK.Unpack(rec, LEDGER_SEAL_AT)
    d["sealed"] = (LedgerHas(d, LEDGER_SEAL_END) and d["mark"]["settle"] != 0 and
                   d["mark"]["settle"] == d["seq"])
    if LedgerHas(d, LEDGER_USE_CHAIN_END):
        d["use_chain"] = USE_CHAIN.Unpack(rec, LEDGER_USE_CHAIN_AT)
    elif d["use_context_bytes"]:
        off, ln = d["use_context_offset"], d["use_context_bytes"]
        if off < LEDGER_REQUIRED_BYTES or off + ln > n:
            d["use_misplaced"] = True
        else:
            d["use_context"] = bytes(rec[off:off + ln]).decode("utf-8", "replace")
    d["ok"] = True
    return d


def LedgerHas(d, end):
    """The one rule for the Ledger's growth field (volume_ledger.h LedgerHas):
    the record reaches past `end`, and no Use Context lies over it."""
    return d["bytes"] >= end and (d["use_context_bytes"] == 0 or d["use_context_offset"] >= end)


def LedgerChoice(copies):
    """Which copy the kernel stands on: the valid copy with the strictly
    greater seq, a tie going to copy 0 (ledger.c ledger_load). None when
    neither is valid — the kernel refuses."""
    newest = None
    for c in copies:
        if not c["ok"]:
            continue
        if newest is None or c["seq"] > newest["seq"]:
            newest = c
    return newest


# ---------------------------------------------------------------------------
# DiskBook
# ---------------------------------------------------------------------------


def ReadDiskBookHead(sector, records_sector):
    """The DiskBook head (disk_book.h DiskBookSuperblock). `valid` is magic + crc
    over the whole 512 bytes with its crc field zeroed (disk_book.c
    DiskBookHeadWhole); `acceptable` is a copy DiskBookReplay takes, and a book
    with no such copy has nothing to replay (disk_book.c DiskBookHeadTaken)."""
    sb = bytes(sector[:SECTOR])
    d = DISKBOOK_SUPERBLOCK.Unpack(sb)
    probe = bytearray(sb)
    probe[DISKBOOK_HEAD_CRC_AT:DISKBOOK_HEAD_CRC_AT + 4] = b"\0\0\0\0"
    d["valid"] = d["magic"] == DISKBOOK_SB_MAGIC and Crc32(bytes(probe)) == d["crc32"]
    d["acceptable"] = (d["valid"] and d["version"] == DISKBOOK_VERSION and
                       d["capacity"] == DISKBOOK_CAPACITY and
                       d["count"] <= DISKBOOK_CAPACITY and
                       d["start_sector"] == records_sector)
    return d


def ReadDiskBookEntry(raw):
    """One redirect record (disk_book.h DiskBookEntry); `valid` is magic + crc
    over the 1024 bytes with its crc field zeroed (disk_book.c DiskBookRecordWhole)."""
    raw = bytes(raw[:DISKBOOK_ENTRY_BYTES])
    d = DISKBOOK_ENTRY.Unpack(raw)
    probe = bytearray(raw)
    probe[DISKBOOK_ENTRY_CRC_AT:DISKBOOK_ENTRY_CRC_AT + 4] = b"\0\0\0\0"
    d["valid"] = d["magic"] == DISKBOOK_MAGIC and Crc32(bytes(probe)) == d["record_crc32"]
    return d


# ---------------------------------------------------------------------------
# CoW manifest
# ---------------------------------------------------------------------------


def ReadManifest(raw):
    """A snapshot manifest (cow.h:48-53). `ok` is exactly the mount's test
    (tagfs_init's manifest check): the magic, and a count the block can hold."""
    magic, count = struct.unpack_from("<II", raw, 0)
    entries = []
    for i in range(min(count, COW_MANIFEST_MAX)):
        e = COW_SNAPSHOT_DISK.Unpack(raw, COW_MANIFEST_HEADER + i * COW_SNAPSHOT_DISK.size)
        e["name"] = e["name"].split(b"\0", 1)[0].decode("utf-8", "replace")
        entries.append(e)
    return {"magic": magic, "count": count, "entries": entries,
            "ok": magic == COW_MANIFEST_MAGIC and count <= COW_MANIFEST_MAX}


# ---------------------------------------------------------------------------
# The chains
# ---------------------------------------------------------------------------


def ReadRegistryBlock(raw, upto=REGISTRY_DATA):
    """A tag registry block (tagfs.h:52-59) and its records, walked exactly as
    tag_registry_load's RegistryAdoptBlock walks them: entry_count records
    packed from the start of the data, and none past `upto` — the whole data,
    or the bytes a seal names of the block it names."""
    d = REGISTRY_BLOCK.Unpack(raw)
    upto = min(upto, REGISTRY_DATA)
    data = raw[CHAIN_HEADER:CHAIN_HEADER + REGISTRY_DATA]
    records, offset = [], 0
    for _ in range(d["entry_count"]):
        if offset + REGISTRY_RECORD.size > upto:
            break
        r = REGISTRY_RECORD.Unpack(data, offset)
        size = REGISTRY_RECORD.size + r["key_len"] + r["value_len"]
        if offset + size > upto:
            break
        key_at = offset + REGISTRY_RECORD.size
        value_at = key_at + r["key_len"]
        r["at"] = CHAIN_HEADER + offset
        # A spelling is its bytes and their length (tag_registry.c entry_is):
        # a NUL inside a key is a byte of it, and a value of no bytes is none.
        r["key"] = bytes(data[key_at:value_at])
        r["value"] = (bytes(data[value_at:value_at + r["value_len"]])
                      if r["value_len"] else None)
        records.append(r)
        offset += size
    d["records"] = records
    return d


def ReadTableBlock(raw):
    """A file table block (tagfs.h:71-77). Only the first entry_count rows are
    loaded, clamped to 510 (file_table.c:152-158); the rest read as empty. A
    row is a file when meta_block != 0 (file_table.c:191)."""
    d = FILE_TABLE_BLOCK.Unpack(raw)
    d["entries"] = min(d["entry_count"], FTABLE_PER_BLOCK)
    d["rows"] = [struct.unpack_from("<II", raw, CHAIN_HEADER + i * FILE_TABLE_ENTRY.size)
                 for i in range(d["entries"])]
    return d


def PoolUsed(raw):
    """The bytes of records a pool block states, as every walk bounds them
    (meta_pool.c PoolCountKept, meta_pool_mount.c WalkRecords): its used_bytes,
    no more than the payload."""
    return min(U16(raw, 8), MPOOL_DATA)


def PoolRecords(raw, upto):
    """The records of a pool block up to byte `upto` of its payload, walked as
    the mount walks them (meta_pool_mount.c WalkRecords, WalkFidsPast;
    meta_pool.c PoolCountKept): a record begins where the one before ended, at
    least a header's length must be left, and a length under a header or past
    `upto` ends the walk. [(at, record_len, file_id)], `at` counted from the
    block's start, as a table row counts it; and the byte the walk stopped at."""
    walk, pos = [], 0
    while pos + RECORD_HEADER <= upto:
        record_len = U16(raw, CHAIN_HEADER + pos)
        if record_len < RECORD_HEADER or record_len > upto - pos:
            break
        walk.append((CHAIN_HEADER + pos, record_len, U32(raw, CHAIN_HEADER + pos + 2)))
        pos += record_len
    return walk, pos


def ReadPoolBlock(raw, upto=None):
    """A metadata pool block (tagfs.h:100-108) and its records.

    `mirror_walk` is the walk of its records over used_bytes. `walk_stop` is
    (at, record_len) where the mount's walk stopped short of `upto` — the bytes
    it reads the block's records to: its used bytes, or the bytes a seal names
    of its open block (meta_pool_mount.c WalkOnce) — or None: what lies there
    is never read by anybody. Past a seal's bytes nothing is a record the mount
    takes up, so nothing there is a shape it misses.

    `kept_counted` is PoolCountKept (meta_pool.c), the number the mount
    believes instead of the header's `kept`."""
    d = META_POOL_BLOCK.Unpack(raw)
    used = PoolUsed(raw)
    walk, _pos = PoolRecords(raw, used)
    upto = used if upto is None else min(upto, used)
    _walk, pos = PoolRecords(raw, upto)
    d["mirror_walk"] = walk
    d["walk_stop"] = (CHAIN_HEADER + pos, U16(raw, CHAIN_HEADER + pos)) if pos < upto else None
    d["kept_counted"] = sum(1 for _a, _l, fid in walk if fid != 0)
    return d


def PoolBlockMix(block, raw):
    """A closed pool block's part in a seal's fold (meta_pool.c PoolBlockMix):
    the CRC32 of its records, summed again with the block's number and the
    records' length."""
    used = PoolUsed(raw)
    payload = raw[CHAIN_HEADER:CHAIN_HEADER + used]
    return Crc32(struct.pack("<III", block, used, Crc32(bytes(payload))))


class Record:
    """One metadata record at byte `at` of a 4096-byte block.

    `unpack_ok` is UnpackV1's acceptance (plot/plot_pack.c): a length in
    [42, 4080], a CRC16 over exactly that length with the crc field zeroed,
    the record lying inside the block, and what its counts say it carries —
    42 + 2t + 6e + n — inside its length. `crc_ok` is the sum alone.
    `shape_ok` is what a writer produces (PackV1): the length is exactly
    42 + 2t + 6e + n. `extents_at_mount` is what the mount keeps: nothing at
    all when one extent has a zero count, runs past 2^32-1, or ends past the
    data run of `run` blocks — the Deed states it before the mount reads a
    record (PlotExtentsInRun); `outside` says which of those it was.
    """

    __slots__ = ("block", "at", "header_ok", "len", "fid", "flags", "size",
                 "created", "modified", "tag_count", "extent_count", "name_len",
                 "crc_stored", "crc_ok", "shape_ok", "unpack_ok", "tags",
                 "extents", "name", "extents_at_mount", "outside")

    def __init__(self, raw, at, block=None, run=None):
        self.block, self.at = block, at
        self.outside = False
        self.header_ok = at + RECORD_HEADER <= len(raw)
        self.tags, self.extents, self.name = [], [], ""
        self.extents_at_mount = []
        self.crc_ok = self.shape_ok = self.unpack_ok = False
        if not self.header_ok:
            self.len = self.fid = self.flags = self.size = 0
            self.created = self.modified = 0
            self.tag_count = self.extent_count = self.name_len = self.crc_stored = 0
            return
        (self.len, self.fid, self.flags, self.size, self.created, self.modified,
         self.tag_count, self.extent_count, self.name_len,
         self.crc_stored) = META_RECORD.struct.unpack_from(raw, at)
        if RECORD_HEADER <= self.len <= MPOOL_DATA and at + self.len <= len(raw):
            probe = bytearray(raw[at:at + self.len])
            probe[RECORD_CRC_AT:RECORD_CRC_AT + 2] = b"\0\0"
            self.crc_ok = Crc16(probe) == self.crc_stored
            self.unpack_ok = self.crc_ok
        tag_at = at + RECORD_HEADER
        ext_at = tag_at + 2 * self.tag_count
        name_at = ext_at + FILE_EXTENT.size * self.extent_count
        end = name_at + self.name_len
        self.shape_ok = self.len == end - at and end <= len(raw)
        self.unpack_ok = self.unpack_ok and end - at <= self.len
        if end <= len(raw):
            self.tags = list(struct.unpack_from("<%dH" % self.tag_count, raw, tag_at))
            self.extents = [FILE_EXTENT.struct.unpack_from(raw, ext_at + FILE_EXTENT.size * e)
                            for e in range(self.extent_count)]
            self.name = bytes(raw[name_at:end]).decode("utf-8", "replace")
            self.outside = run is not None and any(c > 0 and s + c > run for s, c in self.extents)
            if all(c > 0 and s + c <= EXTENT_END_LIMIT for s, c in self.extents) and not self.outside:
                self.extents_at_mount = list(self.extents)

    def Blocks(self):
        return sum(c for _s, c in self.extents_at_mount)


def ParseRecord(raw, at, block=None, run=None):
    """The record at byte `at` of `raw` (a block), as a Record — what the
    mount keeps of its extents held to a data run of `run` blocks."""
    return Record(raw, at, block, run)


def ReadUseChainLink(raw):
    """A TUSE link (volume_ledger.h UseChainLink): the 44-byte header and the
    text it carries, at most 4052 bytes."""
    d = TUSE_LINK.Unpack(raw)
    d["text"] = bytes(raw[TUSE_LINK.size:TUSE_LINK.size + min(d["used"], TUSE_TEXT)])
    return d


def ReadUseChain(read_block, ref, run):
    """The chain `ref` names (first, attempt, seq, bytes, crc32), read as the
    kernel's UseChainRead reads it: exactly ceil(bytes / 4052) hops, never more
    than the data run has blocks; a link carrying another magic or another
    (seq, attempt) is its block's previous content ("foreign") and is not the
    chain's; one of this pair that disagrees in index, n_links, total_bytes,
    text_crc32 or used, or a text that does not sum, is "rotten". `blocks` are
    the links read as this chain's own, in link order."""
    out = {"state": "whole", "blocks": [], "text": None}
    links = (ref["bytes"] + TUSE_TEXT - 1) // TUSE_TEXT
    if not ref["first"] or not ref["bytes"] or links > run:
        out["state"] = "rotten"
        return out
    b, crc, text = ref["first"], 0, []
    for i in range(links):
        if not 1 <= b < run:
            out["state"] = "rotten"
            return out
        raw = read_block(b)
        if raw is None:
            out["state"] = "unread"
            return out
        link = ReadUseChainLink(raw)
        if (link["magic"] != TUSE_MAGIC or link["seq"] != ref["seq"] or
                link["attempt"] != ref["attempt"]):
            out["state"] = "foreign"
            return out
        out["blocks"].append(b)
        want = TUSE_TEXT if i + 1 < links else ref["bytes"] - i * TUSE_TEXT
        if (link["index"] != i or link["n_links"] != links or link["total_bytes"] != ref["bytes"] or
                link["text_crc32"] != ref["crc32"] or link["used"] != want):
            out["state"] = "rotten"
            return out
        crc = Crc32(link["text"], crc)
        text.append(link["text"])
        b = link["next"]
    if crc != ref["crc32"]:
        out["state"] = "rotten"
        return out
    out["text"] = b"".join(text)
    return out


def SpareShape(raw):
    """A stamped pool, registry or table header with nothing in it and no
    link: the shape of a Spare (Quill design §6). Returns the chain's name or
    None."""
    magic, nxt = struct.unpack_from("<II", raw, 0)
    kinds = {MPOOL_MAGIC: "pool", REGISTRY_MAGIC: "registry", FILETBL_MAGIC: "table"}
    if magic not in kinds or nxt != 0 or any(raw[8:CHAIN_HEADER]) or any(raw[CHAIN_HEADER:BLOCK]):
        return None
    return kinds[magic]


# ---------------------------------------------------------------------------
# Runs of data blocks — every block question is answered as [(start, count)]
# ---------------------------------------------------------------------------

_UNIFORM = re.compile(rb"\x00+|\xff+|[\x01-\xfe]")


def RunsFromBits(bits, nbits):
    """The used runs of a bitmap (bit b of byte b//8, LSB first — the block
    map's MapBit), stepping over whole 0x00 and 0xFF bytes the way the block
    map measures a window (block_map.c, MapMeasureView)."""
    runs, cur = [], None
    data = bytes(bits[:(nbits + 7) // 8])
    for m in _UNIFORM.finditer(data):
        s, e, first = m.start() * 8, min(m.end() * 8, nbits), data[m.start()]
        if first == 0:
            if cur is not None:
                runs.append((cur, s - cur))
                cur = None
        elif first == 0xFF:
            if cur is None:
                cur = s
        else:
            for b in range(s, e):
                if first >> (b - s) & 1:
                    if cur is None:
                        cur = b
                elif cur is not None:
                    runs.append((cur, b - cur))
                    cur = None
    if cur is not None:
        runs.append((cur, nbits - cur))
    return runs


def RunsNormal(runs):
    out = []
    for s, n in sorted(r for r in runs if r[1] > 0):
        if out and s <= out[-1][0] + out[-1][1]:
            ps, pn = out[-1]
            out[-1] = (ps, max(pn, s + n - ps))
        else:
            out.append((s, n))
    return out


def RunsIntersect(a, b):
    out, i, j = [], 0, 0
    while i < len(a) and j < len(b):
        lo = max(a[i][0], b[j][0])
        hi = min(a[i][0] + a[i][1], b[j][0] + b[j][1])
        if lo < hi:
            out.append((lo, hi - lo))
        if a[i][0] + a[i][1] < b[j][0] + b[j][1]:
            i += 1
        else:
            j += 1
    return out


def RunsSubtract(a, b):
    out, j = [], 0
    for s, n in a:
        cur, end = s, s + n
        while j < len(b) and b[j][0] + b[j][1] <= cur:
            j += 1
        k = j
        while k < len(b) and b[k][0] < end:
            if b[k][0] > cur:
                out.append((cur, b[k][0] - cur))
            cur = max(cur, b[k][0] + b[k][1])
            k += 1
        if cur < end:
            out.append((cur, end - cur))
    return out


def RunsCount(runs):
    return sum(n for _s, n in runs)


def RunsText(runs):
    return ", ".join("%u+%u" % r for r in runs) if runs else "none"
