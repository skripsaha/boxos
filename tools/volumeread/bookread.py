"""bookread — the book's alphabet and the block digest on the host.

src/include/volume_book.h says what a sector of a format-v2 volume's book is
and how its numbers and pointers are written; src/include/volume_digest.h says
how a block's digest is taken. This is the same said again in Python, by code
of its own — not translated out of the headers at run time — so that a reader
of a volume on the host is a second witness to the format, not an echo of the
first.

Every number names the header line it is copied from. The `self-test` X3 case
holds the numbers and the sector head's layout against the headers by reading
them (as X1 does for ondisk.py), holds the CRC32C table against the header's,
and reaches the known answers the kernel's startup suites reach — the golden
sector's sum (book suite) and the golden block's digest (boxhash suite) — each
by this module's own arithmetic. A drift on either side is a red line.

Bytes in, plain values out; nothing here reads a medium.
"""

import re
import struct

from .ondisk import Fields, SECTORS_PER_BLOCK

# ---------------------------------------------------------------------------
# The sector
# ---------------------------------------------------------------------------

SECTOR_BYTES = 512                      # volume_book.h:58 BOOK_SECTOR_BYTES
HEAD_BYTES = 24                         # volume_book.h:59 BOOK_HEAD_BYTES
PAYLOAD_BYTES = SECTOR_BYTES - HEAD_BYTES   # volume_book.h:60 BOOK_PAYLOAD_BYTES
UNIT_STITCH = 1                         # volume_book.h:64 BOOK_UNIT_STITCH
UNIT_SHEET = 2                          # volume_book.h:65 BOOK_UNIT_SHEET
UNIT_SLOT = 3                           # volume_book.h:66 BOOK_UNIT_SLOT

# volume_book.h:68 BookSectorHead
SECTOR_HEAD = Fields("<IHHIIQ", "crc32c lead kind at of seq")

# ---------------------------------------------------------------------------
# CRC32C — Castagnoli, reflected 0x82F63B78, register from 0xFFFFFFFF,
# inverted at the end (volume_book.h:94 and the comment above it)
# ---------------------------------------------------------------------------

CRC32C_CHECK = 0xE3069283               # volume_book.h:94 BOOK_CRC32C_CHECK


def _Table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
        table.append(c)
    return tuple(table)


CRC32C_TABLE = _Table()                 # volume_book.h:101 BookCrc32cTable


def Crc32cOn(state, data):
    """The register carried over more bytes (volume_book.h:153 BookCrc32cOn)."""
    table = CRC32C_TABLE
    for b in data:
        state = table[(state ^ b) & 0xFF] ^ (state >> 8)
    return state


def Crc32c(data):
    return ~Crc32cOn(0xFFFFFFFF, data) & 0xFFFFFFFF


def SectorSeed(uuid, vlba):
    """The register after uuid ‖ le64(vlba) (volume_book.h:167 BookSectorSeed)."""
    return Crc32cOn(Crc32cOn(0xFFFFFFFF, uuid), struct.pack("<Q", vlba))


def SectorSum(sector, uuid, vlba):
    """What a sector's head must carry as crc32c at `vlba` of volume `uuid`."""
    return ~Crc32cOn(SectorSeed(uuid, vlba), sector[4:SECTOR_BYTES]) & 0xFFFFFFFF


def SectorCheck(sector, uuid, vlba):
    """Does the sector hold there? (volume_book.h:179 BookSectorCheck)"""
    h = SECTOR_HEAD.Unpack(sector)
    if not UNIT_STITCH <= h["kind"] <= UNIT_SLOT or h["at"] >= h["of"] or h["seq"] == 0:
        return False
    if h["lead"] != 0 and not HEAD_BYTES <= h["lead"] < SECTOR_BYTES:
        return False
    return h["crc32c"] == SectorSum(sector, uuid, vlba)


def SectorSeal(sector, uuid, vlba):
    """The sector with its head's crc32c set for `vlba` of `uuid`."""
    out = bytearray(sector)
    struct.pack_into("<I", out, 0, SectorSum(out, uuid, vlba))
    return bytes(out)


# ---------------------------------------------------------------------------
# Numbers (volume_book.h:223-262)
# ---------------------------------------------------------------------------

U64 = (1 << 64) - 1


def VarintPut(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def VarintGet(buf, at=0, end=None):
    """(value, where the next thing begins), or None when refused: cut short by
    `end`, past ten bytes, a tenth byte above 0x01, or a trailing zero byte."""
    end = len(buf) if end is None else end
    v = 0
    for i in range(10):
        if at + i >= end:
            return None
        b = buf[at + i]
        if i == 9 and b > 0x01:
            return None
        v |= (b & 0x7F) << (7 * i)
        if not b & 0x80:
            if i > 0 and b == 0:
                return None
            return v, at + i + 1
    return None


def Zigzag(n):
    return ((n << 1) ^ (n >> 63)) & U64


def Unzigzag(z):
    return (z >> 1) ^ -(z & 1)


def IdsPut(ids):
    """A rising list as every list of ids in the book is written (§2.8 Born's
    tags, Labeled, Undone; §2.9 a header's tags): its count, the first id
    whole, and each next one as its distance from the last less one."""
    out, last = [VarintPut(len(ids))], None
    for i in ids:
        out.append(VarintPut(i if last is None else i - last - 1))
        last = i
    return b"".join(out)


# ---------------------------------------------------------------------------
# Units, their streams and their records (volume_book.h:264-360)
# ---------------------------------------------------------------------------


def EntryNext(stream, at, end=None):
    """volume_book.h BookEntryNext, one step of the walk from `at`: (1, tag,
    body, len, next) for a record; (0, ...) at PAD or the stream's end; (-1,
    ...) broken — a number refused, or a length past the end, seen before the
    body is looked at."""
    end = len(stream) if end is None else end
    if at >= end:
        return 0, 0, at, 0, at
    r = VarintGet(stream, at, end)
    if r is None:
        return -1, 0, at, 0, at
    if r[0] == 0:
        return 0, 0, at, 0, at
    tag, p = r
    r = VarintGet(stream, p, end)
    if r is None or r[0] > end - r[1]:
        return -1, 0, at, 0, at
    return 1, tag, r[1], r[0], r[1] + r[0]


def EntryPut(kind, body, ignorable=False):
    """A record: tag (kind << 1 | ignorable), the body's length, the body."""
    return VarintPut(kind << 1 | (1 if ignorable else 0)) + VarintPut(len(body)) + bytes(body)


def Leads(starts, of):
    """The `lead` of each of `of` sectors of a unit (§2.1): where in the
    sector the first of `starts` (the stream offsets its records begin at —
    a record, a NodeHead or an element, never PAD) that begins in it begins,
    counted from the sector's first byte; 0 when none does."""
    starts, out, i = sorted(starts), [], 0
    for k in range(of):
        lo, hi = k * PAYLOAD_BYTES, (k + 1) * PAYLOAD_BYTES
        while i < len(starts) and starts[i] < lo:
            i += 1
        out.append(HEAD_BYTES + starts[i] - lo if i < len(starts) and starts[i] < hi else 0)
    return out


def LeadsWrong(starts, leads):
    """[(sector, its lead, the lead its records give it)] where a unit read
    whole names a lead its own records do not (§2.1) — what a reader takes a
    stream up by when a sector before is lost."""
    return [(k, have, want) for k, (have, want) in enumerate(zip(leads, Leads(starts, len(leads))))
            if have != want]


def UnitSeal(stream, starts, kind, seq, vlbas, uuid):
    """The sectors of one unit, each sealed at its own vlba: `stream` padded
    with zeros to len(vlbas) * 488 bytes and cut into the sectors' payloads,
    each sector's head naming its kind, place and seq, and its lead (Leads)."""
    of = len(vlbas)
    data = bytes(stream).ljust(of * PAYLOAD_BYTES, b"\0")
    if len(data) != of * PAYLOAD_BYTES:
        raise ValueError("a stream of %u bytes does not fit %u sectors" % (len(stream), of))
    out = []
    for k, lead in enumerate(Leads(starts, of)):
        head = SECTOR_HEAD.struct.pack(0, lead, kind, k, of, seq)
        out.append(SectorSeal(head + data[k * PAYLOAD_BYTES:(k + 1) * PAYLOAD_BYTES], uuid, vlbas[k]))
    return out


class Short(Exception):
    """A body that ends before its fields do, or holds a number written in a
    way the book never writes one."""


class Cursor:
    """A walk along one record's body, field by field: every read refused
    raises Short, so a decoder says only what a body holds."""

    __slots__ = ("buf", "at", "end")

    def __init__(self, buf, at=0, end=None):
        self.buf, self.at = buf, at
        self.end = len(buf) if end is None else end

    def V(self):
        r = VarintGet(self.buf, self.at, self.end)
        if r is None:
            raise Short("a number at byte %u will not read" % self.at)
        self.at = r[1]
        return r[0]

    def Z(self):
        return Unzigzag(self.V())

    def Fixed(self, fmt):
        n = struct.calcsize(fmt)
        if self.end - self.at < n:
            raise Short("%u bytes wanted at byte %u, %u left" % (n, self.at, self.end - self.at))
        v = struct.unpack_from(fmt, self.buf, self.at)[0]
        self.at += n
        return v

    def Take(self, n):
        if n > self.end - self.at:
            raise Short("%u bytes wanted at byte %u, %u left" % (n, self.at, self.end - self.at))
        self.at += n
        return bytes(self.buf[self.at - n:self.at])

    def Ids(self):
        """IdsPut's list read back; a list past u64 is refused."""
        ids = []
        for _ in range(self.V()):
            ids.append(self.V() if not ids else ids[-1] + 1 + self.V())
            if ids[-1] > U64:
                raise Short("a list of ids runs past 2^64")
        return ids

    def Ptr(self):
        p = PtrGet(self.buf, self.at, self.end)
        if p is None:
            raise Short("a pointer at byte %u will not read" % self.at)
        self.at = p["end"]
        return p

    def Left(self):
        return self.end - self.at


# ---------------------------------------------------------------------------
# The self-checking pointer (volume_book.h:367-439)
# ---------------------------------------------------------------------------

# What a pointer can name (volume_book.h, BookPtr): a node's 8 * sheets
# sectors are a count a unit's `of` (u32) holds, and every copy's last
# sector, block * 8 + 8 * sheets - 1, has a vlba.
_SHEETS_MOST = 0xFFFFFFFF // SECTORS_PER_BLOCK
_BLOCKS_END = 1 << 61


def PtrGet(buf, at=0, end=None):
    """{seq, sheets, copies, blocks, end} or None (volume_book.h:399 BookPtrGet)."""
    end = len(buf) if end is None else end
    got = []
    for _ in range(3):
        r = VarintGet(buf, at, end)
        if r is None:
            return None
        got.append(r[0])
        at = r[1]
    seq, sheets, copies = got
    if seq == 0 or copies == 0 or sheets == 0 or sheets > _SHEETS_MOST or copies > end - at:
        return None
    blocks = []
    for _ in range(copies):
        r = VarintGet(buf, at, end)
        if r is None:
            return None
        if r[0] + sheets > _BLOCKS_END:
            return None
        blocks.append(r[0])
        at = r[1]
    return {"seq": seq, "sheets": sheets, "copies": copies, "blocks": blocks, "end": at}


def PtrPut(seq, sheets, blocks):
    """The pointer's bytes, or None for one PtrGet would refuse."""
    if seq == 0 or not blocks or sheets == 0 or sheets > _SHEETS_MOST or \
            any(b + sheets > _BLOCKS_END for b in blocks):
        return None
    return b"".join(VarintPut(v) for v in [seq, sheets, len(blocks)] + list(blocks))


# ---------------------------------------------------------------------------
# The block digest (volume_digest.h)
# ---------------------------------------------------------------------------

SECRET = (0x9E3779B97F4A7C15, 0xBF58476D1CE4E5B9, 0x94D049BB133111EB,
          0x4CF5AD432745937F, 0x3A478BE4AC9E0D17)   # volume_digest.h:45 WySecret
DIGEST_GOLDEN_4096 = 0x0911A376FAC2E968             # volume_digest.h:149


def WyMix(a, b):
    r = (a & U64) * (b & U64)
    return (r ^ (r >> 64)) & U64


def WyHash64(data, seed):
    """volume_digest.h:79 WyHash64."""
    n, p = len(data), data
    r64 = lambda i: struct.unpack_from("<Q", p, i)[0]
    r32 = lambda i: struct.unpack_from("<I", p, i)[0]
    seed ^= WyMix(seed ^ SECRET[0], SECRET[1])
    if n <= 16:
        if n >= 4:
            off = (n >> 3) << 2
            a = (r32(0) << 32) | r32(off)
            b = (r32(n - 4) << 32) | r32(n - 4 - off)
        elif n > 0:
            a, b = (p[0] << 16) | (p[n >> 1] << 8) | p[n - 1], 0
        else:
            a = b = 0
    else:
        i, q = n, 0
        if i > 48:
            s1 = s2 = seed
            while True:
                seed = WyMix(r64(q) ^ SECRET[1], r64(q + 8) ^ seed)
                s1 = WyMix(r64(q + 16) ^ SECRET[2], r64(q + 24) ^ s1)
                s2 = WyMix(r64(q + 32) ^ SECRET[3], r64(q + 40) ^ s2)
                q, i = q + 48, i - 48
                if i <= 48:
                    break
            seed ^= s1 ^ s2
        while i > 16:
            seed = WyMix(r64(q) ^ SECRET[1], r64(q + 8) ^ seed)
            q, i = q + 16, i - 16
        a, b = r64(q + i - 16), r64(q + i - 8)
    r = (a ^ SECRET[1]) * (b ^ seed)
    return WyMix((r & U64) ^ SECRET[0] ^ n, (r >> 64) ^ SECRET[1])


def SeedLanes(seed):
    """volume_digest.h:134 WySeedLanes: the four lanes a seed's bytes give."""
    base = WyHash64(seed, SECRET[0]) if seed else SECRET[0]
    lanes = []
    for j in range(4):
        base = WyMix(base ^ SECRET[j % 5], SECRET[(j + 1) % 5])
        lanes.append(base)
    return lanes


def Integrity(data, lanes):
    """BoxHashIntegrity (tagfs/box_hash/box_hash.c): 0 for no bytes."""
    return WyHash64(data, lanes[0]) if data else 0


# ---------------------------------------------------------------------------
# X3: held against the headers
# ---------------------------------------------------------------------------

GOLDEN_UUID = bytes(range(16))


def GoldenSector():
    """volume_book.h:205's rule, sealed: head {24, STITCH, 0, 8, 1}, payload
    byte i = (i * 7 + 3) & 0xFF, at vlba 0x2000 of uuid 00..0F."""
    s = bytearray((i * 7 + 3) & 0xFF for i in range(SECTOR_BYTES))
    SECTOR_HEAD.struct.pack_into(s, 0, 0, 24, UNIT_STITCH, 0, 8, 1)
    return SectorSeal(s, GOLDEN_UUID, 0x2000)


def GoldenBlock():
    return bytes((i * 13 + 5) & 0xFF for i in range(4096))


_RFC3720 = ((bytes(32), 0x8A9136AA), (b"\xff" * 32, 0x62A8AB43), (bytes(range(32)), 0x46DD794E),
            (bytes(range(31, -1, -1)), 0x113FDB5C),
            (bytes.fromhex("01c00000" + "00" * 12 + "14000000" + "00000400" + "00000014" + "00000018"
                           "28000000" + "00" * 4 + "02000000" + "00" * 4), 0xD9963A56))


def _Answers():
    """[(what, have, want)] the module reaches by its own arithmetic."""
    golden = GoldenSector()
    head = SECTOR_HEAD.Unpack(golden)
    out = [("CRC32C(%r) of RFC 3720 B.4" % data[:4], Crc32c(data), want) for data, want in _RFC3720]
    out += [("the golden sector holds", SectorCheck(golden, GOLDEN_UUID, 0x2000), True),
            ("the golden sector at vlba 0x2001", SectorSum(golden, GOLDEN_UUID, 0x2001), 0x37BBCDD9),
            ("the golden sector of another volume holds",
             SectorCheck(golden, bytes(range(1, 17)), 0x2000), False),
            ("the golden sector's head", (head["lead"], head["kind"], head["at"], head["of"], head["seq"]),
             (24, UNIT_STITCH, 0, 8, 1))]
    for v, size in ((0, 1), (127, 1), (128, 2), (16384, 3), (2**32 - 1, 5), (2**63, 10), (U64, 10)):
        b = VarintPut(v)
        out.append(("varint %u" % v, (len(b), VarintGet(b)), (size, (v, size))))
    for bad in (b"\x80\x00", b"\xff\x80\x00", b"\xff" * 9 + b"\x02", b"\xff" * 9 + b"\x81\x00", b"\x80\x80"):
        out.append(("varint %s refused" % bad.hex(), VarintGet(bad), None))
    out += [("zigzag -1, 1, least", (Zigzag(-1), Zigzag(1), Zigzag(-2**63)), (1, 2, U64)),
            ("unzigzag back", (Unzigzag(Zigzag(-2**63)), Unzigzag(Zigzag(2**63 - 1))), (-2**63, 2**63 - 1))]
    p = PtrPut(77, 2, [1024, 0x123456789, 7])
    out += [("pointer of three copies", PtrGet(p),
             {"seq": 77, "sheets": 2, "copies": 3, "blocks": [1024, 0x123456789, 7], "end": len(p)}),
            ("pointer cut short refused", PtrGet(p[:-1]), None),
            ("pointer naming more copies than it carries refused", PtrGet(bytes([3, 1, 5, 8, 9])), None),
            ("pointer to seq 0 not written", PtrPut(0, 1, [8]), None)]
    most = 0xFFFFFFFF // SECTORS_PER_BLOCK
    top = (1 << 61) - most
    wide = PtrPut(3, most, [top])
    v = lambda *n: b"".join(VarintPut(x) for x in n)
    out += [("pointer to the widest node, ending on the last sector there is", PtrGet(wide or b""),
             {"seq": 3, "sheets": most, "copies": 1, "blocks": [top], "end": len(wide or b"")}),
            ("pointer of one Sheet more not written", PtrPut(3, most + 1, [8]), None),
            ("pointer to a copy one block further on not written", PtrPut(3, most, [top + 1]), None),
            ("pointer of one Sheet more refused", PtrGet(v(3, most + 1, 1, 8)), None),
            ("pointer to a copy no vlba can end refused", PtrGet(v(3, most, 2, 8, top + 1)), None)]
    return out


def AlphabetCheck(defines, structs, book_text):
    """What is wrong here against the headers — `defines` read out of
    volume_deed.h, volume_book.h and volume_digest.h, `structs` out of
    volume_book.h, `book_text` its text for the table — and how many things
    were held."""
    wrong, n = [], 0

    def Eq(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            wrong.append("%s: the header says %r and bookread.py %r" % (name, want, have))

    for py, c in ((SECTOR_BYTES, "BOOK_SECTOR_BYTES"), (HEAD_BYTES, "BOOK_HEAD_BYTES"),
                  (PAYLOAD_BYTES, "BOOK_PAYLOAD_BYTES"), (UNIT_STITCH, "BOOK_UNIT_STITCH"),
                  (UNIT_SHEET, "BOOK_UNIT_SHEET"), (UNIT_SLOT, "BOOK_UNIT_SLOT"),
                  (CRC32C_CHECK, "BOOK_CRC32C_CHECK")):
        Eq(c, py, defines.get(c))
    fields = {f: (o, s) for f, o, s in structs.get("BookSectorHead", [])}
    for field, off, size in SECTOR_HEAD.Offsets():
        Eq("BookSectorHead.%s" % field, (off, size), fields.get(field))
    table = re.search(r"BookCrc32cTable\[256\]\s*=\s*\{(.*?)\};", book_text, re.S)
    said = tuple(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", table.group(1))) if table else ()
    n += 1
    if said != CRC32C_TABLE:
        at = next((i for i, (a, b) in enumerate(zip(said, CRC32C_TABLE)) if a != b), min(len(said), 256))
        wrong.append("BookCrc32cTable: %u entries, and entry %u is %s in the header and 0x%08X here"
                     % (len(said), at, "0x%08X" % said[at] if at < len(said) else "missing",
                        CRC32C_TABLE[at] if at < 256 else 0))

    def Known(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            show = lambda v: "0x%X" % v if isinstance(v, int) and not isinstance(v, bool) else repr(v)
            wrong.append("%s: %s where %s is the answer" % (name, show(have), show(want)))

    Known("CRC32C(\"123456789\")", Crc32c(b"123456789"), defines.get("BOOK_CRC32C_CHECK"))
    Known("the golden sector's sum", SectorSum(GoldenSector(), GOLDEN_UUID, 0x2000),
          defines.get("BOOK_GOLDEN_SECTOR_CRC"))
    Known("the golden block's digest", Integrity(GoldenBlock(), SeedLanes(GOLDEN_UUID)),
          defines.get("VOLUME_DIGEST_GOLDEN_4096"))
    Eq("VOLUME_DIGEST_GOLDEN_4096", DIGEST_GOLDEN_4096, defines.get("VOLUME_DIGEST_GOLDEN_4096"))
    for what, have, want in _Answers():
        Known(what, have, want)
    return wrong, n
