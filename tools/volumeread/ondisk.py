"""ondisk — the TagFS v1 format as the medium carries it, said once on the host.

Every number here is copied out of a C header or a C routine, and every one
names where it came from: a header by file and line, a routine by its name —
the lines of tagfs.c move with every step of the plan, its routines do not.
The `self-test` X1 case holds the constants and the struct layouts against the
headers themselves, so a drift there is FOUND, by a red line; a routine a
comment names is only found by reading it.

The parsers take bytes and give back plain dicts (or a Record). None of them
reads the medium: what to read, and in which order, is the caller's business —
the census reads it the way the mounting kernel does, the cut reads it out of a
write log. The three walkers (WalkChain, MirrorWalk, KernelRollCall) take a
`read_block` callable instead of a medium, because they ARE format semantics —
how far a chain may run, which record of a file wins, what the mount's roll
call does with a file it cannot read — and those have to be said in exactly one
place for `list`, `get` and `check` alike.

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
BLOCK = 4096                            # tagfs.h:30 TAGFS_BLOCK_SIZE
SECTORS_PER_BLOCK = BLOCK // SECTOR     # tagfs_constants.h:12 TAGFS_BLOCK_SECTORS

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
DEED_CRC_AT = 12                        # volume_deed.h:127 VolumeDeed.crc32
STAMP_GEOMETRY, STAMP_LAYOUT, STAMP_BORN, STAMP_BOOT, STAMP_PACKED = 1, 2, 3, 4, 5  # volume_deed.h:149-153
STAMP_HEADER = 4                        # volume_deed.h:142-145 VolumeStamp

VOLUME_DEED = Fields("<8sHHI16sQQI",    # volume_deed.h:123-132
                     "magic prologue_bytes stamp_bytes crc32 uuid sectors tail_sector role")
VOLUME_GEOMETRY = Fields("<IIII",       # volume_deed.h:176-181
                         "logical_bytes physical_bytes grain_bytes block_bytes")
VOLUME_LAYOUT = Fields("<QIIIIIIIIIIIIII",   # volume_deed.h:198-214
                       "total_blocks state_block state_blocks tag_registry_block "
                       "tag_registry_blocks file_table_block file_table_blocks "
                       "metadata_pool_block metadata_pool_blocks block_bitmap_block "
                       "block_bitmap_blocks disk_book_block disk_book_blocks "
                       "data_block data_blocks")
VOLUME_BOOT = Fields("<IIII",           # volume_deed.h:230-235
                     "kernel_block kernel_blocks kernel_bytes reserved")
VOLUME_BORN = Fields("<Q16s",           # volume_deed.h:239-242
                     "created_unix maker")
VOLUME_PACKED = Fields("<QQQII",        # volume_deed.h:267-273
                       "packed_unix shipment tree file_count verdict")

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

# ---------------------------------------------------------------------------
# The three chains and the metadata record
# ---------------------------------------------------------------------------

REGISTRY_MAGIC = 0x54524547             # boxos_magic.h:13 TAGFS_REGISTRY_MAGIC "TREG"
FILETBL_MAGIC = 0x54465442              # boxos_magic.h:14 TAGFS_FILETBL_MAGIC  "TFTB"
MPOOL_MAGIC = 0x544D504C                # boxos_magic.h:15 TAGFS_MPOOL_MAGIC    "TMPL"
REGISTRY_DATA = 4080                    # tagfs.h:71 TAGFS_REGISTRY_DATA_SIZE
MPOOL_DATA = 4080                       # tagfs.h:72 TAGFS_MPOOL_DATA_SIZE
FTABLE_PER_BLOCK = 510                  # tagfs.h:73 TAGFS_FTABLE_PER_BLOCK
CHAIN_HEADER = 16                       # tagfs.h:83, 105, 116 — every chain header is 16 bytes

REGISTRY_BLOCK = Fields("<IIHH4x",      # tagfs.h:87-94 TagRegistryBlock header
                        "magic next_block entry_count used_bytes")
REGISTRY_RECORD = Fields("<HBBH",       # tagfs.h:84-86; tag_registry.c RegistryPack
                         "tag_id flags key_len value_len")
FILE_TABLE_BLOCK = Fields("<IIII",      # tagfs.h:106-112 FileTableBlock header
                          "magic next_block entry_count reserved")
FILE_TABLE_ENTRY = Fields("<II",        # tagfs.h:98-101 FileTableEntry
                          "meta_block meta_offset")
META_POOL_BLOCK = Fields("<IIHHH2x",    # tagfs.h:140-148 MetaPoolBlock header
                         "magic next_block used_bytes record_count kept")
FILE_EXTENT = Fields("<IH",             # tagfs.h:152-155 FileExtent
                     "start_block block_count")

RECORD_HEADER = 42                      # meta_pool.c:11 RECORD_HEADER_SIZE
MPOOL_BLOCK_HEADER = 16                 # meta_pool.c:12
RECORD_CRC_AT = 40                      # meta_pool.c:13 RECORD_CRC_OFFSET
META_RECORD = Fields("<HIIQQQHHHH",     # tagfs_boot.h:53-67; meta_pool.c:492-504 pack_record
                     "record_len file_id flags size created_time modified_time "
                     "tag_count extent_count name_len crc16")

FILE_ACTIVE, FILE_TRASHED, FILE_HIDDEN = 1, 2, 4   # tagfs.h:59-61; bit 3 is spent (tagfs.h:62)
MAX_TAG_ID = 0x7FFE                     # tagfs.h:56 TAGFS_MAX_TAG_ID
TAG_FLAG_HAS_VALUE, TAG_FLAG_SYSTEM = 1, 2   # tagfs_constants.h:36-37
MIRROR_SLACK = 64                       # tagfs_init: meta_pool_mirror_init(next_file_id + 64); meta_pool.c:726 adds 1
EXTENT_END_LIMIT = 0xFFFFFFFF           # meta_pool.c:631 start + count past 2^32-1 is an overflow

# ---------------------------------------------------------------------------
# DiskBook
# ---------------------------------------------------------------------------

DISKBOOK_MAGIC = 0x44424F4F             # disk_book.h:34 DISK_BOOK_MAGIC    "DBOO"
DISKBOOK_SB_MAGIC = 0x44425342          # disk_book.h:35 DISK_BOOK_SB_MAGIC "DBSB"
DISKBOOK_VERSION = 2                    # disk_book.h:36
DISKBOOK_ENTRY_BYTES = 1024             # disk_book.h:37
DISKBOOK_SECTORS_PER_ENTRY = 2          # disk_book.h:38
DISKBOOK_CAPACITY = 512                 # disk_book.h:39
DISKBOOK_TYPE_REDIRECT = 1              # disk_book.h:42
DISKBOOK_HEAD_CRC_AT = 32               # disk_book.h:52 DiskBookSuperblock.crc32
DISKBOOK_ENTRY_CRC_AT = 32              # disk_book.h:71 DiskBookEntry.record_crc32
# Head, its copy, then the records (create_tagfs.c:634-635, tagfs_init's DiskBookInit).
DISKBOOK_BLOCKS = 2 + DISKBOOK_CAPACITY * DISKBOOK_SECTORS_PER_ENTRY // SECTORS_PER_BLOCK
DISKBOOK_SUPERBLOCK = Fields("<IIQIIIII16s",   # disk_book.h:44-55
                             "magic version start_sector capacity count "
                             "generation flags crc32 uuid")
DISKBOOK_ENTRY = Fields("<IIHHIIIQI",   # disk_book.h:62-73
                        "magic sequence type flags snapshot_id old_block "
                        "new_block tag_bits record_crc32")

# ---------------------------------------------------------------------------
# Copy-on-write manifest, integrity map, and the Quill TUSE link
# ---------------------------------------------------------------------------

COW_MANIFEST_MAGIC = 0x434F564D         # cow.h:16 "COVM"
COW_MANIFEST_MAX = 60                   # cow.h:17
COW_MANIFEST_HEADER = 8                 # cow.h:69-72 magic + count
COW_SNAPSHOT_DISK = Fields("<IIQI32sB3xIQ",    # cow.h:55-65 CowSnapshotDisk
                           "snapshot_id parent_file_id created_time "
                           "disk_book_checkpoint name flags file_count total_size")

INTEGRITY_PER_BLOCK = 512               # integrity.c:14 ENTRIES_PER_BLOCK

# Quill design §1.2 UseChainLink. Not written by anything on HEAD; recognised
# so that the census can name one the day a writer lays it down.
TUSE_MAGIC = 0x54555345                 # "TUSE"
TUSE_LINK = Fields("<IIQIIIIQI",
                   "magic next seq attempt index used n_links total_bytes text_crc32")
TUSE_TEXT = BLOCK - 44                  # 4052 bytes of text after the 44-byte header

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
         "boot": None, "born": None, "packed": None}
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
                              ("packed", STAMP_PACKED, VOLUME_PACKED)):
        payload = d["stamps"].get(kind)
        # Each is used only when the stamp is at least as long as the struct
        # this build knows (tagfs.c volume_take_ground; deed.c:296-329).
        if payload is not None and len(payload) >= fields.size:
            d[key] = fields.Unpack(payload)
    d["ok"] = True
    return d


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
    past a short record, and the remembered Use Context only if it lies inside
    the record. `why` carries the kernel's own words for a refusal."""
    rec = bytearray(sector[:SECTOR].ljust(SECTOR, b"\0"))
    d = VOLUME_LEDGER.Unpack(rec)
    d.update(copy=copy, ok=False, why=None, use_context=None, use_misplaced=False)
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
    if n < VOLUME_LEDGER.size:
        rec[n:VOLUME_LEDGER.size] = bytes(VOLUME_LEDGER.size - n)
        d.update(VOLUME_LEDGER.Unpack(rec))
        d.update(copy=copy, why=None, use_context=None, use_misplaced=False)
    if d["use_context_bytes"]:
        off, ln = d["use_context_offset"], d["use_context_bytes"]
        if off < LEDGER_REQUIRED_BYTES or off + ln > n:
            d["use_misplaced"] = True
        else:
            d["use_context"] = bytes(rec[off:off + ln]).decode("utf-8", "replace")
    d["ok"] = True
    return d


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
    """The DiskBook head (disk_book.h:44-55). `valid` is magic + crc over the
    whole 512 bytes with its crc field zeroed (disk_book.c:65-72); `acceptable`
    is what DiskBookInit takes instead of reformatting (disk_book.c:137-140)."""
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
    """One redirect record (disk_book.h:62-73); `valid` is magic + crc over the
    1024 bytes with its crc field zeroed (disk_book.c:37-41, 173-180)."""
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
    """A snapshot manifest (cow.h:69-74). `ok` is exactly the mount's test
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


def _CString(raw):
    """What the kernel holds after copying `raw` into a NUL-terminated buffer:
    everything up to the first NUL (tag_registry.c RegistryAdopt)."""
    return bytes(raw).split(b"\0", 1)[0]


def ReadRegistryBlock(raw):
    """A tag registry block (tagfs.h:87-94) and its records, walked exactly as
    tag_registry_load walks them: entry_count records packed from the start of
    the data, and none past its end."""
    d = REGISTRY_BLOCK.Unpack(raw)
    data = raw[CHAIN_HEADER:CHAIN_HEADER + REGISTRY_DATA]
    records, offset = [], 0
    for _ in range(d["entry_count"]):
        if offset + REGISTRY_RECORD.size > REGISTRY_DATA:
            break
        r = REGISTRY_RECORD.Unpack(data, offset)
        size = REGISTRY_RECORD.size + r["key_len"] + r["value_len"]
        if offset + size > REGISTRY_DATA:
            break
        key_at = offset + REGISTRY_RECORD.size
        value_at = key_at + r["key_len"]
        r["at"] = CHAIN_HEADER + offset
        r["key"] = _CString(data[key_at:value_at])
        r["value"] = (_CString(data[value_at:value_at + r["value_len"]])
                      if r["value_len"] else None)
        records.append(r)
        offset += size
    d["records"] = records
    return d


def ReadTableBlock(raw):
    """A file table block (tagfs.h:106-112). Only the first entry_count rows are
    loaded, clamped to 510 (file_table.c:152-158); the rest read as empty. A
    row is a file when meta_block != 0 (file_table.c:191)."""
    d = FILE_TABLE_BLOCK.Unpack(raw)
    d["entries"] = min(d["entry_count"], FTABLE_PER_BLOCK)
    d["rows"] = [struct.unpack_from("<II", raw, CHAIN_HEADER + i * FILE_TABLE_ENTRY.size)
                 for i in range(d["entries"])]
    return d


def ReadPoolBlock(raw):
    """A metadata pool block (tagfs.h:140-148) with its two walks.

    `mirror_walk` is the mount's mirror walk (meta_pool.c:763-791): by
    record_len while pos < used_bytes, stopping at a length of zero or one past
    the payload. Each step is (at, record_len, file_id) with `at` counted from
    the block start, the way a table row counts it. `walk_stop` is where that
    walk gave up inside used_bytes, or None.

    `kept_counted` is count_kept_records (meta_pool.c:124-142), the number the
    mount believes instead of the header's `kept`."""
    d = META_POOL_BLOCK.Unpack(raw)
    walk, pos, stop = [], 0, None
    while pos < d["used_bytes"]:
        record_len = U16(raw, CHAIN_HEADER + pos)
        if record_len == 0 or record_len > MPOOL_DATA - pos:
            stop = (CHAIN_HEADER + pos, record_len)
            break
        walk.append((CHAIN_HEADER + pos, record_len, U32(raw, CHAIN_HEADER + pos + 2)))
        pos += record_len
    d["mirror_walk"], d["walk_stop"] = walk, stop

    used = min(d["used_bytes"], MPOOL_DATA)
    kept, pos = 0, 0
    while pos + RECORD_HEADER <= used:
        record_len = U16(raw, CHAIN_HEADER + pos)
        if record_len < RECORD_HEADER or record_len > used - pos:
            break
        if U32(raw, CHAIN_HEADER + pos + 2) != 0:
            kept += 1
        pos += record_len
    d["kept_counted"] = kept
    return d


class Record:
    """One metadata record at byte `at` of a 4096-byte block.

    `unpack_ok` is unpack_record's acceptance (meta_pool.c:553-581): a length
    in [42, 4080] and a CRC16 over exactly that length with the crc field
    zeroed — plus the record lying inside the block, because past it the
    kernel sums its own stack. `shape_ok` is what a writer produces
    (meta_pool.c:474-480): the length is exactly 42 + 2t + 6e + n. The tags,
    extents and name are read where unpack_record reads them — from the counts,
    not bounded by the length (K3). `extents_at_mount` is what the mount keeps:
    nothing at all when one extent has a zero count or runs past 2^32-1
    (meta_pool.c:628-641; the past-the-run test is off while mounting, 625-627).
    """

    __slots__ = ("block", "at", "header_ok", "len", "fid", "flags", "size",
                 "created", "modified", "tag_count", "extent_count", "name_len",
                 "crc_stored", "crc_ok", "shape_ok", "unpack_ok", "tags",
                 "extents", "name", "extents_at_mount")

    def __init__(self, raw, at, block=None):
        self.block, self.at = block, at
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
        if end <= len(raw):
            self.tags = list(struct.unpack_from("<%dH" % self.tag_count, raw, tag_at))
            self.extents = [FILE_EXTENT.struct.unpack_from(raw, ext_at + FILE_EXTENT.size * e)
                            for e in range(self.extent_count)]
            self.name = bytes(raw[name_at:end]).decode("utf-8", "replace")
            if all(c > 0 and s + c <= EXTENT_END_LIMIT for s, c in self.extents):
                self.extents_at_mount = list(self.extents)

    def Blocks(self):
        return sum(c for _s, c in self.extents_at_mount)


def ParseRecord(raw, at, block=None):
    """The record at byte `at` of `raw` (a block), as a Record."""
    return Record(raw, at, block)


def ReadUseChainLink(raw):
    """A TUSE link (Quill design §1.2): the 44-byte header and the text it
    carries, at most 4052 bytes."""
    d = TUSE_LINK.Unpack(raw)
    d["text"] = bytes(raw[TUSE_LINK.size:TUSE_LINK.size + min(d["used"], TUSE_TEXT)])
    return d


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


# ---------------------------------------------------------------------------
# The walkers — format semantics that need blocks, not bytes
# ---------------------------------------------------------------------------

CHAIN_NAMES = {REGISTRY_MAGIC: "tag registry", FILETBL_MAGIC: "file table",
               MPOOL_MAGIC: "metadata pool"}


def WalkChain(read_block, first, magic, run):
    """A chain walked exactly as its loader walks it — the three are one rule
    (file_table.c:105-170, meta_pool.c:324-380, tag_registry_load): a link
    outside the run is refused before it is read, a block that will not read or
    does not carry the magic is refused, next_block == 0 ends the chain, and
    the run's own length in hops is a cycle.

    Returns (blocks, raws, refused) where `refused` is None or the sentence of
    the loader that would refuse the mount over it."""
    name = CHAIN_NAMES[magic]
    blocks, raws, block, hops = [], [], first, 0
    while True:
        if block >= run:
            return blocks, raws, ("the %s chain points at block %u and the data run "
                                  "is %u blocks" % (name, block, run))
        raw = read_block(block)
        if raw is None:
            return blocks, raws, "block %u of the %s would not read" % (block, name)
        found = struct.unpack_from("<I", raw, 0)[0]
        if found != magic:
            return blocks, raws, ("block %u holds 0x%08x where the %s should be"
                                  % (block, found, name))
        blocks.append(block)
        raws.append(raw)
        nxt = struct.unpack_from("<I", raw, 4)[0]
        if nxt == 0:
            return blocks, raws, None
        hops += 1
        if hops >= run:
            return blocks, raws, ("the %s chain has taken %u hops in a data run of "
                                  "%u blocks" % (name, hops, run))
        block = nxt


def MirrorWalk(pool_blocks, pool_raws, capacity):
    """The mount's mirror (meta_pool.c:744-791): every pool block in chain
    order, every record in append order, and for each 0 < fid < capacity the
    LAST record that unpacks. Returns {fid: Record}. Pass capacity None for the
    truth view, which has no capacity."""
    newest = {}
    for block, raw in zip(pool_blocks, pool_raws):
        for at, _record_len, fid in ReadPoolBlock(raw)["mirror_walk"]:
            if fid == 0 or (capacity is not None and fid >= capacity):
                continue
            rec = ParseRecord(raw, at, block)
            if rec.unpack_ok:
                newest[fid] = rec
    return newest


class FileVerdict:
    """What the mount's roll call decided about one file id."""

    __slots__ = ("fid", "record", "outcome", "corrected")

    def __init__(self, fid, record, outcome, corrected):
        self.fid, self.record, self.outcome, self.corrected = fid, record, outcome, corrected


def KernelRead(read_block, place):
    """meta_pool_read (meta_pool.c:1036-1053) as far as a host can follow it:
    the block the place names, unpacked at the offset the place names. An
    offset whose header would not fit the block is a failed read here — the
    kernel reads past a 4 KiB stack buffer there (K2, left for step 1.4)."""
    block, offset = place
    if offset + RECORD_HEADER > BLOCK:
        return None
    raw = read_block(block)
    if raw is None:
        return None
    rec = ParseRecord(raw, offset, block)
    return rec if rec.unpack_ok else None


def KernelRollCall(read_block, next_file_id, rows, mirror):
    """The per-file half of the mount's fsck (tagfs_init's roll call): fid 1 up to
    next_file_id, skipping one with no row; the pool's newest place wins over
    the row's; a place that will not unpack loses the file; a record that is
    not ACTIVE is quietly quitclaimed. Returns [FileVerdict] with outcome
    'visible', 'lost' or 'quitclaim'."""
    out = []
    for fid in range(1, next_file_id):
        row = rows.get(fid)
        if row is None:
            continue
        place, corrected = row, False
        newest = mirror.get(fid)
        if newest is not None and (newest.block, newest.at) != row:
            place, corrected = (newest.block, newest.at), True
        rec = KernelRead(read_block, place)
        if rec is None:
            outcome = "lost"
        elif not rec.flags & FILE_ACTIVE:
            outcome = "quitclaim"
        else:
            outcome = "visible"
        out.append(FileVerdict(fid, rec, outcome, corrected))
    return out


def TableRows(table_raws):
    """{fid: (meta_block, meta_offset)} for every row that names a file."""
    rows = {}
    for link, raw in enumerate(table_raws):
        for i, (mb, mo) in enumerate(ReadTableBlock(raw)["rows"]):
            if mb != 0:
                rows[link * FTABLE_PER_BLOCK + i] = (mb, mo)
    return rows


# ---------------------------------------------------------------------------
# Volume — list and get
# ---------------------------------------------------------------------------


class Volume:
    """A volume on a medium, for reading files out of it the way the mounted
    kernel would see them."""

    def __init__(self, medium, ground, deed):
        self.medium = medium
        self.ground = ground
        self.layout = deed["layout"]
        self.deed = deed

    def data_block(self, rel):
        """One 4 KiB block of the DATA RUN, counted from the run's start —
        block_to_vlba (tagfs.c)."""
        lba = self.ground + (self.layout["data_block"] + rel) * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def volume_block(self, abs_b):
        """One 4 KiB block counted from the START of the volume."""
        lba = self.ground + abs_b * SECTORS_PER_BLOCK
        return self.medium.sectors(lba, SECTORS_PER_BLOCK)

    def read_block(self, rel):
        """data_block, answering None where the medium ends — a failed read."""
        try:
            return self.data_block(rel)
        except IOError:
            return None

    def ledger(self):
        """The Ledger copy the kernel stands on, or None when neither reads."""
        copies = []
        for c in range(LEDGER_COPIES):
            try:
                sector = self.volume_block(self.layout["state_block"] + c)[:SECTOR]
            except IOError:
                sector = b""
            copies.append(ReadLedgerCopy(sector, c))
        return LedgerChoice(copies)

    def files(self):
        """Every file the mounted kernel would show, resolved the way its mount
        resolves them (KernelRollCall): ids below the Ledger's next_file_id
        that have a table row, each at the NEWEST record the pool holds for it
        — not at whatever place the row names — CRC-checked and ACTIVE.

        ‼ The pool is append-only: a file written in several goes leaves a
        record per go, every one of them ACTIVE. The row names one of them and
        the pool's newest names another whenever a table flush was lost, and
        the kernel believes the pool (tagfs_init's roll call). A reader that believed
        the row would hand back a file the machine never shows.

        A volume with no readable Ledger does not mount at all; its files are
        still listed, bounded by the table's own length, because this is the
        tool that gets evidence off a volume that will not mount."""
        run = self.layout["data_blocks"]
        table_head = self.layout["file_table_block"] - self.layout["data_block"]
        pool_head = self.layout["metadata_pool_block"] - self.layout["data_block"]
        _tb, table_raws, _why = WalkChain(self.read_block, table_head, FILETBL_MAGIC, run)
        pool_blocks, pool_raws, _why = WalkChain(self.read_block, pool_head, MPOOL_MAGIC, run)
        rows = TableRows(table_raws)
        led = self.ledger()
        next_fid = led["next_file_id"] if led else max(rows, default=0) + 1
        mirror = MirrorWalk(pool_blocks, pool_raws, next_fid + MIRROR_SLACK + 1)
        out = []
        for v in KernelRollCall(self.read_block, next_fid, rows, mirror):
            if v.outcome != "visible" or v.record.flags & FILE_TRASHED:
                continue
            r = v.record
            out.append({"fid": v.fid, "name": r.name, "size": r.size,
                        "flags": r.flags, "extents": r.extents,
                        "extents_at_mount": r.extents_at_mount})
        return out

    def contents(self, entry):
        """The file's bytes, following the extents the mount keeps and stopping
        at its size."""
        out = bytearray()
        for start, count in entry["extents_at_mount"]:
            for i in range(count):
                if len(out) >= entry["size"]:
                    break
                out += self.data_block(start + i)
            if len(out) >= entry["size"]:
                break
        return bytes(out[:entry["size"]])
