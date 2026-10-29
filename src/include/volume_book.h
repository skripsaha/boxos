#ifndef VOLUME_BOOK_H
#define VOLUME_BOOK_H

/*
 * The book's alphabet — what every sector of a format-v2 volume's book is,
 * and how a number, a record and a pointer are written in it.
 *
 * A format-v2 volume keeps everything but its files' bytes in a BOOK: the
 * ribbon of stitches the Settle cycles sew, the Tally that sums the ribbon up,
 * and the Spine slots that say where both are (docs: spec_v2 §1.4, §2). All of
 * it is written in the letters below and in nothing else, so one reader of
 * these letters reads every part of it — the kernel, the UEFI loader, mkfs,
 * and volume-read on the host (tools/volumeread/bookread.py, which self-test
 * holds to this header).
 *
 *
 * ‼ EVERY SECTOR SAYS WHAT IT IS
 *
 * Each 512-byte sector of the book opens with 24 bytes of its own: a sum; the
 * unit it belongs to (its kind and its seq); where in that unit it sits (at,
 * of); and where the first record that begins in it begins (lead). Once a
 * sector, not once a block, because a write cut short lands in 512-byte pieces
 * inside a 4 KiB block — the FAST'13 study saw it so, with 13 of 15 SSDs
 * failing at a power cut. A sector that outlived its neighbours still names
 * its unit whole, and the record stream is taken up again after the one that
 * was lost. The price is 24 / 512 = 4.7 % of the book's room.
 *
 * The sum is CRC32C over the volume's uuid, the sector's address and the
 * sector's own bytes 4..511. The first two cost nothing on the medium and
 * tell apart three things one sum over the bytes cannot: a sector of another
 * volume, a sector written or read at the wrong address, and rot. Over 512
 * bytes CRC32C keeps a Hamming distance of 6 where CRC-32 keeps 4.
 *
 *
 * ‼ ONE TEXT FOR THREE BUILDS
 *
 * The kernel (-mno-sse, ktypes.h), the UEFI loader (-nostdinc, uefi.h, which
 * has no int8_t, no int16_t and no bool) and the host's mkfs (an arm64 clang
 * on the desk this is written on) take this same header. So it uses only the
 * unsigned widths, int32_t, int64_t and int; it moves bytes with loops, not a
 * library; it keeps no state of its own, so any core reads with it at once
 * and without a lock; and it carries no x86 instruction. The kernel sums with
 * the crc32 instruction where the processor has one (crypto.c, KCrc32c), and
 * its answers are this table's — the startup suite `book` holds them to it on
 * every boot that mounts a volume, for the startup suites run inside the
 * volume's door (tests.c, TagFS_RunTests).
 *
 * Integer types come from the includer, as volume_deed.h says. Answers are
 * 1 and 0 for yes and no, and 0 for "refused" where a count is answered.
 */

#include "volume_deed.h"

/* ------------------------------------------------------------------------- */
/* The sector                                                                */
/* ------------------------------------------------------------------------- */

#define BOOK_SECTOR_BYTES   512u
#define BOOK_HEAD_BYTES     24u
#define BOOK_PAYLOAD_BYTES  (BOOK_SECTOR_BYTES - BOOK_HEAD_BYTES)   /* 488 */

/* What a unit is. A unit is the run of sectors one write of the book puts
 * down whole: a stitch, a node of the Tally, a slot of the Spine. */
#define BOOK_UNIT_STITCH    1u   /* a Settle cycle's stitch on the ribbon */
#define BOOK_UNIT_SHEET     2u   /* a node of the Tally: one or more Sheets */
#define BOOK_UNIT_SLOT      3u   /* a slot of the Spine ring */

typedef struct __attribute__((packed)) {
    uint32_t crc32c;  /*  0  CRC32C(uuid[16] ‖ le64(vlba) ‖ sector[4..512))            */
    uint16_t lead;    /*  4  offset in this sector of the first record that begins here;
                             0 when none does (the sector only continues one)          */
    uint16_t kind;    /*  6  BOOK_UNIT_*                                               */
    uint32_t at;      /*  8  this sector's index inside its unit, 0..of-1              */
    uint32_t of;      /* 12  sectors in the unit                                       */
    uint64_t seq;     /* 16  the unit's book sequence number, > 0                      */
} BookSectorHead;
_Static_assert(sizeof(BookSectorHead) == BOOK_HEAD_BYTES, "a book sector's head");

/* A book sector is a Deed's sector, and a Sheet — the Tally's unit of room —
 * is one block of the volume: eight of them. Said here so the two headers
 * cannot drift apart. */
_Static_assert(BOOK_SECTOR_BYTES == VOLUME_DEED_SECTOR_BYTES, "a book sector is a Deed sector");
_Static_assert(VOLUME_BLOCK_SECTORS == 8u, "a Sheet is one volume block of eight book sectors");

/* ------------------------------------------------------------------------- */
/* CRC32C                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Castagnoli: reflected polynomial 0x82F63B78, the register started at
 * 0xFFFFFFFF and inverted at the end. CRC-32/ISCSI's check value, the sum of
 * the nine bytes "123456789":
 */
#define BOOK_CRC32C_CHECK  0xE3069283u

/*
 * The one table every build without the crc32 instruction sums by — a const,
 * not something filled in at boot, so no core can read it half-written. A
 * translation unit that never sums carries none of it.
 */
static const uint32_t BookCrc32cTable[256] = {
    0x00000000u, 0xF26B8303u, 0xE13B70F7u, 0x1350F3F4u, 0xC79A971Fu, 0x35F1141Cu,
    0x26A1E7E8u, 0xD4CA64EBu, 0x8AD958CFu, 0x78B2DBCCu, 0x6BE22838u, 0x9989AB3Bu,
    0x4D43CFD0u, 0xBF284CD3u, 0xAC78BF27u, 0x5E133C24u, 0x105EC76Fu, 0xE235446Cu,
    0xF165B798u, 0x030E349Bu, 0xD7C45070u, 0x25AFD373u, 0x36FF2087u, 0xC494A384u,
    0x9A879FA0u, 0x68EC1CA3u, 0x7BBCEF57u, 0x89D76C54u, 0x5D1D08BFu, 0xAF768BBCu,
    0xBC267848u, 0x4E4DFB4Bu, 0x20BD8EDEu, 0xD2D60DDDu, 0xC186FE29u, 0x33ED7D2Au,
    0xE72719C1u, 0x154C9AC2u, 0x061C6936u, 0xF477EA35u, 0xAA64D611u, 0x580F5512u,
    0x4B5FA6E6u, 0xB93425E5u, 0x6DFE410Eu, 0x9F95C20Du, 0x8CC531F9u, 0x7EAEB2FAu,
    0x30E349B1u, 0xC288CAB2u, 0xD1D83946u, 0x23B3BA45u, 0xF779DEAEu, 0x05125DADu,
    0x1642AE59u, 0xE4292D5Au, 0xBA3A117Eu, 0x4851927Du, 0x5B016189u, 0xA96AE28Au,
    0x7DA08661u, 0x8FCB0562u, 0x9C9BF696u, 0x6EF07595u, 0x417B1DBCu, 0xB3109EBFu,
    0xA0406D4Bu, 0x522BEE48u, 0x86E18AA3u, 0x748A09A0u, 0x67DAFA54u, 0x95B17957u,
    0xCBA24573u, 0x39C9C670u, 0x2A993584u, 0xD8F2B687u, 0x0C38D26Cu, 0xFE53516Fu,
    0xED03A29Bu, 0x1F682198u, 0x5125DAD3u, 0xA34E59D0u, 0xB01EAA24u, 0x42752927u,
    0x96BF4DCCu, 0x64D4CECFu, 0x77843D3Bu, 0x85EFBE38u, 0xDBFC821Cu, 0x2997011Fu,
    0x3AC7F2EBu, 0xC8AC71E8u, 0x1C661503u, 0xEE0D9600u, 0xFD5D65F4u, 0x0F36E6F7u,
    0x61C69362u, 0x93AD1061u, 0x80FDE395u, 0x72966096u, 0xA65C047Du, 0x5437877Eu,
    0x4767748Au, 0xB50CF789u, 0xEB1FCBADu, 0x197448AEu, 0x0A24BB5Au, 0xF84F3859u,
    0x2C855CB2u, 0xDEEEDFB1u, 0xCDBE2C45u, 0x3FD5AF46u, 0x7198540Du, 0x83F3D70Eu,
    0x90A324FAu, 0x62C8A7F9u, 0xB602C312u, 0x44694011u, 0x5739B3E5u, 0xA55230E6u,
    0xFB410CC2u, 0x092A8FC1u, 0x1A7A7C35u, 0xE811FF36u, 0x3CDB9BDDu, 0xCEB018DEu,
    0xDDE0EB2Au, 0x2F8B6829u, 0x82F63B78u, 0x709DB87Bu, 0x63CD4B8Fu, 0x91A6C88Cu,
    0x456CAC67u, 0xB7072F64u, 0xA457DC90u, 0x563C5F93u, 0x082F63B7u, 0xFA44E0B4u,
    0xE9141340u, 0x1B7F9043u, 0xCFB5F4A8u, 0x3DDE77ABu, 0x2E8E845Fu, 0xDCE5075Cu,
    0x92A8FC17u, 0x60C37F14u, 0x73938CE0u, 0x81F80FE3u, 0x55326B08u, 0xA759E80Bu,
    0xB4091BFFu, 0x466298FCu, 0x1871A4D8u, 0xEA1A27DBu, 0xF94AD42Fu, 0x0B21572Cu,
    0xDFEB33C7u, 0x2D80B0C4u, 0x3ED04330u, 0xCCBBC033u, 0xA24BB5A6u, 0x502036A5u,
    0x4370C551u, 0xB11B4652u, 0x65D122B9u, 0x97BAA1BAu, 0x84EA524Eu, 0x7681D14Du,
    0x2892ED69u, 0xDAF96E6Au, 0xC9A99D9Eu, 0x3BC21E9Du, 0xEF087A76u, 0x1D63F975u,
    0x0E330A81u, 0xFC588982u, 0xB21572C9u, 0x407EF1CAu, 0x532E023Eu, 0xA145813Du,
    0x758FE5D6u, 0x87E466D5u, 0x94B49521u, 0x66DF1622u, 0x38CC2A06u, 0xCAA7A905u,
    0xD9F75AF1u, 0x2B9CD9F2u, 0xFF56BD19u, 0x0D3D3E1Au, 0x1E6DCDEEu, 0xEC064EEDu,
    0xC38D26C4u, 0x31E6A5C7u, 0x22B65633u, 0xD0DDD530u, 0x0417B1DBu, 0xF67C32D8u,
    0xE52CC12Cu, 0x1747422Fu, 0x49547E0Bu, 0xBB3FFD08u, 0xA86F0EFCu, 0x5A048DFFu,
    0x8ECEE914u, 0x7CA56A17u, 0x6FF599E3u, 0x9D9E1AE0u, 0xD3D3E1ABu, 0x21B862A8u,
    0x32E8915Cu, 0xC083125Fu, 0x144976B4u, 0xE622F5B7u, 0xF5720643u, 0x07198540u,
    0x590AB964u, 0xAB613A67u, 0xB831C993u, 0x4A5A4A90u, 0x9E902E7Bu, 0x6CFBAD78u,
    0x7FAB5E8Cu, 0x8DC0DD8Fu, 0xE330A81Au, 0x115B2B19u, 0x020BD8EDu, 0xF0605BEEu,
    0x24AA3F05u, 0xD6C1BC06u, 0xC5914FF2u, 0x37FACCF1u, 0x69E9F0D5u, 0x9B8273D6u,
    0x88D28022u, 0x7AB90321u, 0xAE7367CAu, 0x5C18E4C9u, 0x4F48173Du, 0xBD23943Eu,
    0xF36E6F75u, 0x0105EC76u, 0x12551F82u, 0xE03E9C81u, 0x34F4F86Au, 0xC69F7B69u,
    0xD5CF889Du, 0x27A40B9Eu, 0x79B737BAu, 0x8BDCB4B9u, 0x988C474Du, 0x6AE7C44Eu,
    0xBE2DA0A5u, 0x4C4623A6u, 0x5F16D052u, 0xAD7D5351u
};

/*
 * Carry the CRC32C register `state` over `n` more bytes. The register is the
 * sum before its final inversion — the value the crc32 instruction keeps — so
 * a sum is taken in as many pieces as the text arrives in: start at
 * 0xFFFFFFFF, carry it on piece by piece, invert once at the end.
 */
static inline uint32_t BookCrc32cOn(uint32_t state, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        state = BookCrc32cTable[(state ^ p[i]) & 0xFFu] ^ (state >> 8);
    return state;
}

/*
 * The register after a sector's seed: the volume's uuid, then the sector's
 * address `vlba` — counted from the start of the volume's ground, like every
 * number in a Deed — as eight little-endian bytes. A reader of many sectors of
 * one volume may keep the register after the uuid; that is a saving, not the
 * format.
 */
static inline uint32_t BookSectorSeed(const uint8_t uuid[16], uint64_t vlba)
{
    uint8_t le[8];
    for (uint32_t i = 0; i < 8u; i++) le[i] = (uint8_t)(vlba >> (8u * i));
    return BookCrc32cOn(BookCrc32cOn(0xFFFFFFFFu, uuid, 16), le, 8);
}

/*
 * Is this sector's head one a writer writes: a known kind, its place inside
 * its unit, a seq above zero, and a lead that is either none or a byte of the
 * payload? The head goes to *head (when it is asked for). Half of whether the
 * sector holds — its sum is the other half (BookSectorCheck) — asked apart, so
 * a reader that sums by an instruction of its own holds the head to this one
 * rule all the same: the kernel's crc32 (crypto.c, KCrc32c).
 */
static inline int BookSectorHeadHolds(const uint8_t *sector, BookSectorHead *head)
{
    BookSectorHead h;
    uint8_t       *to = (uint8_t *)&h;
    for (uint32_t i = 0; i < BOOK_HEAD_BYTES; i++) to[i] = sector[i];

    if (h.kind < BOOK_UNIT_STITCH || h.kind > BOOK_UNIT_SLOT) return 0;
    if (h.at >= h.of || h.seq == 0) return 0;
    if (h.lead != 0 && (h.lead < BOOK_HEAD_BYTES || h.lead >= BOOK_SECTOR_BYTES)) return 0;
    if (head) *head = h;
    return 1;
}

/* Does this sector hold, at `vlba` of the volume `uuid`? Its head holds, and
 * its sum agrees. */
static inline int BookSectorCheck(const uint8_t *sector, const uint8_t uuid[16], uint64_t vlba)
{
    BookSectorHead h;
    if (!BookSectorHeadHolds(sector, &h)) return 0;
    return h.crc32c == ~BookCrc32cOn(BookSectorSeed(uuid, vlba), sector + 4, BOOK_SECTOR_BYTES - 4u);
}

/*
 * The golden sector: one sector, defined by a rule, whose sum every
 * implementation of the book must reach — the kernel's startup suite `book`
 * by the table and by its own CRC32C, and volume-read's self-test by its own
 * code. A rule and one number, not 512 bytes spelled twice that could drift.
 *
 *   the volume  uuid 00 01 02 .. 0F
 *   the address vlba 0x2000 (block 1024 — where mkfs starts a data run)
 *   the head    lead 24, kind BOOK_UNIT_STITCH, at 0, of 8, seq 1
 *   the payload byte i, for i in 24..511, is (i * 7 + 3) & 0xFF
 *
 * The same bytes at vlba 0x2001 sum to 0x37BBCDD9: an address in the seed is
 * what tells a sector put down in the wrong place.
 */
#define BOOK_GOLDEN_SECTOR_CRC  0x4206698Du

/* ------------------------------------------------------------------------- */
/* Numbers: varint and zigzag                                                */
/* ------------------------------------------------------------------------- */

/*
 * Every number in a stream is a varint — LEB128 without a sign: seven bits a
 * byte, the low ones first, 0x80 meaning "more follows" — so the book has no
 * width to outgrow: a file id, a block, a count, a length or a tag id is as
 * wide as its value, up to 2^64 - 1 in ten bytes.
 *
 * Written one way only. A number is refused when it is cut short by the end
 * of what was read, when it runs past ten bytes or its tenth byte carries more
 * than the one bit u64 has left, and when its last byte is zero after a first
 * (the same number spelled longer). One value, one spelling: two writers of
 * the same book write the same bytes, and a sum over a stream means one thing.
 */
static inline uint32_t BookVarintPut(uint8_t *out, uint64_t v)
{
    uint32_t n = 0;
    while (v >= 0x80u) {
        out[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

/* The bytes the number at `p` takes, out of `room`, with the number in *v —
 * or 0, refused. */
static inline uint32_t BookVarintGet(const uint8_t *p, uint32_t room, uint64_t *v)
{
    uint64_t got = 0;
    for (uint32_t i = 0; i < 10u && i < room; i++) {
        uint8_t b = p[i];
        if (i == 9u && b > 0x01u) return 0;
        got |= (uint64_t)(b & 0x7Fu) << (7u * i);
        if (!(b & 0x80u)) {
            if (i > 0 && b == 0) return 0;
            *v = got;
            return i + 1u;
        }
    }
    return 0;
}

/* A signed difference as an unsigned varint: small either side of zero stays
 * small (-1 is 1, 1 is 2). */
static inline uint64_t BookZigzag(int64_t n)
{
    return ((uint64_t)n << 1) ^ (uint64_t)(n >> 63);
}

static inline int64_t BookUnzigzag(uint64_t z)
{
    return (int64_t)(z >> 1) ^ -(int64_t)(z & 1u);
}

/* ------------------------------------------------------------------------- */
/* Units and their streams                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Do these `count` sectors — read from `vlba` on, one after another — make
 * one whole unit? Every one holds at its own address, every one names the
 * same kind and seq and an `of` of exactly `count`, and each sits at its own
 * `at`. The first sector's head goes to *head (when it is asked for), so the
 * caller can hold the unit to what it expected: a node is a SHEET of
 * 8 * sheets sectors whose seq its pointer names.
 *
 * How large a unit a reader can hold in memory is the reader's to say, aloud,
 * before it reads one (spec_v2 §2.4: the widths of memory are the kernel's,
 * not the format's) — not this check's, which takes what it is given.
 */
static inline int BookUnitHeadsAgree(const uint8_t *sectors, uint32_t count, BookSectorHead *head);

static inline int BookUnitCheck(const uint8_t *sectors, uint32_t count, const uint8_t uuid[16],
                                uint64_t vlba, BookSectorHead *head)
{
    if (count == 0) return 0;
    for (uint32_t at = 0; at < count; at++)
        if (!BookSectorCheck(sectors + (uint64_t)at * BOOK_SECTOR_BYTES, uuid, vlba + at)) return 0;
    return BookUnitHeadsAgree(sectors, count, head);
}

/*
 * BookUnitCheck's second half, for sectors already found to hold one by one:
 * do their heads make one unit — of one kind and one seq, an `of` of exactly
 * `count`, each at its own `at`? The first sector's head to *head (when it is
 * asked for).
 */
static inline int BookUnitHeadsAgree(const uint8_t *sectors, uint32_t count, BookSectorHead *head)
{
    BookSectorHead first, h;
    uint8_t       *to = (uint8_t *)&h, *keep = (uint8_t *)&first;

    if (count == 0) return 0;
    for (uint32_t at = 0; at < count; at++) {
        const uint8_t *s = sectors + (uint64_t)at * BOOK_SECTOR_BYTES;
        for (uint32_t i = 0; i < BOOK_HEAD_BYTES; i++) to[i] = s[i];
        if (at == 0)
            for (uint32_t i = 0; i < BOOK_HEAD_BYTES; i++) keep[i] = to[i];
        if (h.at != at || h.of != count || h.kind != first.kind || h.seq != first.seq) return 0;
    }
    if (head) {
        uint8_t *out = (uint8_t *)head;
        for (uint32_t i = 0; i < BOOK_HEAD_BYTES; i++) out[i] = keep[i];
    }
    return 1;
}

/*
 * A unit's stream: the payloads of its `of` sectors, in order, into `out`
 * (of * BOOK_PAYLOAD_BYTES bytes, which is what is answered). Records run on
 * across sectors and Sheets without a count of parts, so a reader walks the
 * stream flat. The stream offset of sector k's payload is k * 488, and the
 * record its `lead` names begins at k * 488 + (lead - 24): the walk is taken
 * up there when a sector before it is lost. `of` is one BookUnitCheck held.
 */
static inline uint64_t BookUnitStream(const uint8_t *sectors, uint32_t of, uint8_t *out)
{
    for (uint64_t k = 0; k < of; k++) {
        const uint8_t *from = sectors + k * BOOK_SECTOR_BYTES + BOOK_HEAD_BYTES;
        uint8_t       *to   = out + k * BOOK_PAYLOAD_BYTES;
        for (uint32_t i = 0; i < BOOK_PAYLOAD_BYTES; i++) to[i] = from[i];
    }
    return (uint64_t)of * BOOK_PAYLOAD_BYTES;
}

/* ------------------------------------------------------------------------- */
/* Records                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * One step of the walk along a stream of `room` bytes, from *at.
 *
 * A record is `tag varint · len varint · body[len]`, its tag `kind << 1 |
 * ignorable`. Answered:
 *    1  a record: its tag in *tag, its body at *body for *len bytes, and *at
 *       moved past it;
 *    0  the end — a tag of 0 (PAD: the rest of the unit is filler) or the
 *       stream's last byte reached;
 *   -1  broken: a number refused, or a length running past the stream's end —
 *       which is seen BEFORE the body is looked at. *at does not move.
 *
 * What a kind means is not the walk's business. A reader that meets a kind it
 * does not know steps over it when the ignorable bit is set; when it is clear,
 * or the body is longer than the reader knows the kind to be, the unit was
 * written by a kernel that knows more, and the volume may only be read — the
 * caller decides that, aloud, never this walk in silence.
 */
static inline int BookEntryNext(const uint8_t *stream, uint32_t room, uint32_t *at,
                                uint64_t *tag, uint32_t *body, uint32_t *len)
{
    uint64_t t, l;
    uint32_t p = *at, n;

    if (p >= room) return 0;
    if (!(n = BookVarintGet(stream + p, room - p, &t))) return -1;
    if (t == 0) return 0;
    p += n;
    if (!(n = BookVarintGet(stream + p, room - p, &l))) return -1;
    p += n;
    if (l > room - p) return -1;
    *tag  = t;
    *body = p;
    *len  = (uint32_t)l;
    *at   = p + (uint32_t)l;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* The self-checking pointer                                                 */
/* ------------------------------------------------------------------------- */

/*
 * BookPtr = seq · sheets · copies · block × copies, every one a varint.
 *
 * It names a node of the Tally by where its copies lie AND by the seq the node
 * was written with. A copy is the node only if its 8 * sheets sectors from
 * block * 8 make a SHEET unit (BookUnitCheck) whose seq is this one: a write
 * that never landed leaves the old node in place with a sum that still holds,
 * and only the seq tells it from the new one.
 *
 * The copies are tried in their order. The writer writes two; how many there
 * are is the pointer's to say, so there is no array of them here: `blocks`
 * points at the copies' varints where they stand, and the reader takes them
 * one after another with BookVarintGet — every one already found well formed.
 *
 * The varints have no width but u64's; what bounds a pointer is what it can
 * name. A node is one unit, so its 8 * sheets sectors are a count `of` holds:
 * sheets is at most 0xFFFFFFFF / 8. Each copy's sectors, block * 8 to
 * block * 8 + 8 * sheets - 1, are vlbas: block + sheets is at most 2^61. A
 * pointer past either names nothing a medium could hold, and is refused as
 * broken. How large a node a reader can hold in memory is not a rule of the
 * pointer (BookUnitCheck).
 */
typedef struct {
    uint64_t       seq;
    uint64_t       sheets;
    uint64_t       copies;
    const uint8_t *blocks;          /* `copies` varints, in order */
    uint32_t       blocks_bytes;
} BookPtr;

/* The bytes the pointer at `p` takes, out of `room` — or 0, refused: a number
 * refused, a seq of 0, no sheets or more than a unit can count, no copies,
 * fewer copies than it says, or a copy whose sectors run past the last vlba. */
static inline uint32_t BookPtrGet(const uint8_t *p, uint32_t room, BookPtr *ptr)
{
    uint32_t at = 0, n;
    uint64_t block;

    if (!(n = BookVarintGet(p, room, &ptr->seq))) return 0;
    at += n;
    if (!(n = BookVarintGet(p + at, room - at, &ptr->sheets))) return 0;
    at += n;
    if (!(n = BookVarintGet(p + at, room - at, &ptr->copies))) return 0;
    at += n;
    if (ptr->seq == 0 || ptr->copies == 0 || ptr->sheets == 0 ||
        ptr->sheets > 0xFFFFFFFFu / VOLUME_BLOCK_SECTORS)
        return 0;
    if (ptr->copies > room - at) return 0;      /* every copy takes a byte at least */
    ptr->blocks = p + at;
    for (uint64_t k = 0; k < ptr->copies; k++) {
        if (!(n = BookVarintGet(p + at, room - at, &block))) return 0;
        if (block > ((uint64_t)1 << 61) - ptr->sheets) return 0;
        at += n;
    }
    ptr->blocks_bytes = at - (uint32_t)(ptr->blocks - p);
    return at;
}

/* Write a pointer to `copies` blocks into `out` (10 * (3 + copies) bytes at
 * most); the bytes it took, or 0 for a pointer BookPtrGet would refuse. */
static inline uint32_t BookPtrPut(uint8_t *out, uint64_t seq, uint64_t sheets,
                                  const uint64_t *blocks, uint32_t copies)
{
    if (seq == 0 || copies == 0 || sheets == 0 || sheets > 0xFFFFFFFFu / VOLUME_BLOCK_SECTORS)
        return 0;
    for (uint32_t k = 0; k < copies; k++)
        if (blocks[k] > ((uint64_t)1 << 61) - sheets) return 0;
    uint32_t at = BookVarintPut(out, seq);
    at += BookVarintPut(out + at, sheets);
    at += BookVarintPut(out + at, copies);
    for (uint32_t k = 0; k < copies; k++) at += BookVarintPut(out + at, blocks[k]);
    return at;
}

/* ------------------------------------------------------------------------- */
/* The atom, and the ground on it                                            */
/* ------------------------------------------------------------------------- */

/*
 * The atom: the least a write of the book may put down. A unit begins on an
 * atom and is written in whole atoms, and an atom that holds a sector of a
 * settled unit is never written again — because writing part of a medium's
 * physical block makes the device read, patch and rewrite all of it, and a
 * cut in that rewrite tears the NEIGHBOURS of what was written.
 *
 * The largest of the volume's block (4096, the floor), the physical block the
 * volume was laid out for (GEOMETRY), the physical block the medium reports,
 * and the medium's logical block. Bytes, all of them; 0 for one not known.
 */
static inline uint32_t BookAtomOf(uint32_t geometry_physical, uint32_t medium_physical,
                                  uint32_t logical)
{
    uint32_t a = VOLUME_BLOCK_BYTES;
    if (geometry_physical > a) a = geometry_physical;
    if (medium_physical > a)   a = medium_physical;
    if (logical > a)           a = logical;
    return a;
}

/*
 * ‼ ON A REMOVABLE MEDIUM THE RIBBON IS WRITTEN IN FLASH PAGES (spec_v2 §16.6
 * п.2, C18). A cheap stick programs its page whole, reading, patching and
 * rewriting it to put down part of one, and a cut in that rewrite spoils what
 * the page held before — a settled stitch beside the new one; the newest
 * stitch wiped whole, with nothing past it, reads as a cycle that never wrote,
 * a loss no reader can say. So a mount on a removable medium lays the ribbon
 * in atoms of a page — its Quires on them, each stitch ending on one
 * (volume_ribbon.h, RibbonStitchEnds) — and a stitch it lays past another
 * shares no page with it; only the first past an end a writer of a smaller
 * atom left inside a page may, said (ribbon.c). The
 * page is not on the medium: a stick says its physical block at the most
 * (READ CAPACITY(16)), never its page, and the pages it asks of its VPD a USB
 * stick is not asked for — Linux's usb-storage skips them (scsiglue.c
 * sdev_configure: "Some devices don't handle VPD pages correctly"). 16 KiB is
 * the largest page common in the NAND sticks are built from (4, 8 and 16
 * KiB); a medium that says a larger block keeps its own. A rule of the mount
 * alone: no atom is stored on the volume, and a reader takes the next unit
 * where the last one ends (§16.2 п.11), whatever atom laid it.
 */
#define BOOK_FLASH_PAGE_BYTES 16384u

/* The atom the ribbon is written in on a medium of atom `atom` (BookAtomOf),
 * bytes: a flash page on a removable one, the medium's own otherwise. */
static inline uint32_t BookRibbonAtomOf(uint32_t atom, int removable)
{
    return removable && atom < BOOK_FLASH_PAGE_BYTES ? BOOK_FLASH_PAGE_BYTES : atom;
}

/*
 * Does a ground whose first sector is `start` begin on an atom of `atom`
 * bytes? The grid is the MEDIUM's, not the volume's: its physical blocks begin
 * at `l0` and every atom / 512 sectors after — l0 the medium's own word, in
 * 512-byte sectors, already turned by its driver into "the first sector that
 * begins a physical block". Only then are the volume's atoms the medium's, and
 * only then is a whole atom written as a whole physical block.
 */
static inline int BookGroundOnAtom(uint64_t start, uint64_t l0, uint32_t atom)
{
    uint64_t grid = atom / BOOK_SECTOR_BYTES;
    if (grid == 0) return 0;
    return (start >= l0 ? start - l0 : l0 - start) % grid == 0;
}

/* ------------------------------------------------------------------------- */
/* Sealing: the writer's side of BookSectorCheck                             */
/* ------------------------------------------------------------------------- */

/*
 * Seal a sector a writer has filled: `head` (its crc32c left aside) is put in
 * its first 24 bytes, then the sum of all of it at `vlba` of the volume
 * `uuid`, little-endian as every number of the book. The payload, bytes
 * 24..511, is the caller's and is not touched. A writer of the book seals with
 * this and nothing of its own, so what it writes the readers above hold by the
 * one rule.
 */
static inline void BookSectorSeal(uint8_t *sector, const uint8_t uuid[16], uint64_t vlba,
                                  const BookSectorHead *head)
{
    const uint8_t *from = (const uint8_t *)head;
    for (uint32_t i = 0; i < BOOK_HEAD_BYTES; i++) sector[i] = from[i];
    uint32_t sum = ~BookCrc32cOn(BookSectorSeed(uuid, vlba), sector + 4, BOOK_SECTOR_BYTES - 4u);
    for (uint32_t i = 0; i < 4u; i++) sector[i] = (uint8_t)(sum >> (8u * i));
}

#endif /* VOLUME_BOOK_H */
