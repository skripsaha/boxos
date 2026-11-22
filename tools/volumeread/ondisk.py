"""ondisk — the TagFS format as the medium carries it, said once on the host.

Every number here is copied out of a C header or a C routine, and every one
names where it came from: a header by file and line, a routine by its name —
the lines of tagfs.c move with every step of the plan, its routines do not.
The `self-test` X1 case holds the constants and the struct layouts against the
headers themselves, so a drift there is FOUND, by a red line; a routine a
comment names is only found by reading it.

The parsers take bytes and give back plain dicts. None of them reads the
medium: what to read, and in which order, is the caller's business — the
book's mount (bookmount.py) reads it the way the mounting kernel does, the cut
reads it out of a write log. The book itself is said in bookread.py, tally.py
and stitch.py; this is what lies under it — the units, the Deed, a file's
flags, the partition tables.

Blocks of the data run are answered as runs, [(start, count)] sorted: a 64 GB
stick is sixteen million blocks, which nobody should hold as a set of Python
integers.
"""

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
STAMP_GEOMETRY = 1                      # volume_deed.h VOLUME_STAMP_GEOMETRY
STAMP_BORN, STAMP_BOOT, STAMP_PACKED = 2, 3, 4  # volume_deed.h VOLUME_STAMP_BORN, _BOOT, _PACKED
STAMP_FRAME = 5                         # volume_deed.h VOLUME_STAMP_FRAME
STAMP_HEADER = 4                        # volume_deed.h:161-164 VolumeStamp

VOLUME_DEED = Fields("<8sHHI16sQQI",    # volume_deed.h:142-151
                     "magic prologue_bytes stamp_bytes crc32 uuid sectors tail_sector role")
VOLUME_GEOMETRY = Fields("<IIII",       # volume_deed.h:196-201
                         "logical_bytes physical_bytes grain_bytes block_bytes")
VOLUME_BOOT = Fields("<IIII",           # volume_deed.h:254-259
                     "kernel_block kernel_blocks kernel_bytes reserved")
VOLUME_BORN = Fields("<Q16s",           # volume_deed.h:263-266
                     "created_unix maker")
VOLUME_PACKED = Fields("<QQQII",        # volume_deed.h:291-297
                       "packed_unix shipment tree file_count verdict")
VOLUME_FRAME = Fields("<QQII",          # volume_deed.h VolumeFrame
                      "data_block spine_block spine_slots spine_stride")
FRAME_SPINE_SLOTS = 3                   # volume_deed.h:338 VOLUME_FRAME_SPINE_SLOTS

# ---------------------------------------------------------------------------
# A file's flags, as its Born, its Labeled and the Tally's FILES carry them
# ---------------------------------------------------------------------------

FILE_ACTIVE = 1                         # tagfs_constants.h TAGFS_FILE_ACTIVE
FILE_UNDONE = 1 << 1                    # tagfs_constants.h TAGFS_FILE_UNDONE: rolled back by a mount's verdict
FILE_ROLE_MASK = 12                     # tagfs_constants.h TAGFS_FILE_ROLE_MASK: bits 2-3, what the system keeps the file as
FILE_ROLE_SYSTEM = 1 << 2               # TAGFS_FILE_ROLE_SYSTEM
FILE_ROLE_BOOT = 1 << 3                 # TAGFS_FILE_ROLE_BOOT
FILE_ROLE_KERNEL = 12                   # TAGFS_FILE_ROLE_KERNEL: the loader's first choice, then boot, then system
FILE_DELETED = 1 << 4                   # tagfs_constants.h TAGFS_FILE_DELETED: deleted, kept until the keep_ns of its header
FILE_MADE_BY = 1 << 6                   # tagfs_constants.h TAGFS_FILE_MADE_BY: made by a program, whose file the header's made_by names
FILE_SHOWN = 1 << 7                     # tagfs_constants.h TAGFS_FILE_SHOWN: a program's file that shows its tags to the programs the header names
FILE_HEAD = 1 << 8                      # tagfs_constants.h TAGFS_FILE_HEAD: the header carries the file's first bytes
HEAD_MOST = 16                          # tagfs_constants.h TAGFS_HEAD_MOST: as many first bytes as a card keeps
FILE_PASSPORT = 1 << 9                  # tagfs_constants.h TAGFS_FILE_PASSPORT: the header carries a signed program's passport
PASSPORT_BIT = 1 << 63                  # volume_tally.h TALLY_PASSPORT_BIT: every passport carries it, so none is a file id
FILE_LAID_BIT = 1 << 5                  # a header flag no kernel names, kept as laid (book_tests.c BOOK_TEST_LAID_BIT)

# ---------------------------------------------------------------------------
# Partition tables — the ground the kernel will stand on
# ---------------------------------------------------------------------------

MBR_TYPE_BOXOS = 0x7F                   # ground.c:17 GROUND_MBR_TYPE
MBR_TYPE_PROTECTIVE = 0xEE              # ground.c:27 GROUND_MBR_TYPE_PROTECTIVE
GPT_ARRAY_MAX_BYTES = 128 * 1024        # ground.c:83 GPT_ARRAY_MAX_BYTES
GPT_BLOCK_MAX_BYTES = 4096              # ground.c:122 GPT_BLOCK_MAX_BYTES
GROUND_MAX_PER_MEDIUM = 8               # ground.h:42 GROUND_MAX_PER_MEDIUM
BOXOS_TYPE_GUID = bytes([               # ground.c:19-22, gangway_medium.c — as on the medium
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
         "walk_error": None, "geometry": None,
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

    for key, kind, fields in (("geometry", STAMP_GEOMETRY, VOLUME_GEOMETRY),
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


def VolumeFrameCheck(d):
    """VolumeFrameCheck (volume_deed.h), the one rule every reader of a FRAME
    keeps, in its words: (why, note) for the FRAME a Deed carries — `why` None
    when it holds, `note` what a FRAME that holds still has to say (a longer
    one, a ring other than the one this format lays down)."""
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
    other = f["spine_slots"] != FRAME_SPINE_SLOTS
    if longer and other:
        return None, ("it is longer than the FRAME this reader knows and its Spine ring is not "
                      "the one this format lays down — written by a kernel that knows more, "
                      "or by another maker")
    if longer:
        return None, "it is longer than the FRAME this reader knows — written by a kernel that knows more"
    if other:
        return None, "its Spine ring is not the one this format lays down — a volume made by another maker"
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


# ---------------------------------------------------------------------------
# Runs of data blocks — every block question is answered as [(start, count)]
# ---------------------------------------------------------------------------


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
