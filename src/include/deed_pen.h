#ifndef DEED_PEN_H
#define DEED_PEN_H

/*
 * The pen a Deed is written with.
 *
 * volume_deed.h says what a Deed IS. This says how one is put down, and it is
 * the only place in the tree where Deed bytes are composed — by the host tool
 * that makes a volume and by the kernel that amends one when the volume takes
 * more ground.
 *
 *
 * ‼ WHY THIS EXISTS AT ALL
 *
 * Until now a Deed was built in exactly one place, tools/create_tagfs.c, and
 * the kernel only ever read one. The moment a volume can GROW, the kernel has
 * to write one too — and a format with two writers in two languages of care is
 * a format that will disagree with itself on the day one of them is changed.
 * There is one writer. Both sides hold the same pen.
 *
 *
 * ‼ TWO WAYS TO START, AND THE SECOND IS THE WHOLE POINT
 *
 * MAKE (DeedPenOpen) composes a Deed from parts: an empty block, a stamp at a
 * time, then a seal. That is what mkfs does.
 *
 * AMEND (DeedPenTake) picks up a Deed that already exists, in the bytes it was
 * read in, changes what has changed and seals it again. Every stamp it does
 * not touch survives byte-for-byte — INCLUDING a stamp this build has never
 * heard of. That is not a nicety: a Deed carries stamps rather than a version
 * field precisely so that an older reader can step over what it does not know,
 * and a kernel that grew a volume by REBUILDING its Deed from the three stamps
 * it happens to understand would silently throw the rest away. Growth changes
 * three numbers. It must change three numbers.
 *
 *
 * ‼ NO LIBRARY, ON PURPOSE
 *
 * Bytes are moved with an explicit loop, not memcpy. The kernel has klib, the
 * host tool has <string.h>, and the UEFI loader (src/boot/uefi/tagboot.c) has
 * neither — it carries its own MemCopy. A shared builder that reached for a
 * library would be shared between two of the three. Six lines buys the third.
 *
 * Integer types come from the includer, exactly as volume_deed.h requires:
 *   Kernel:    #include "ktypes.h"    Host tool: #include <stdint.h>
 */

#include "volume_deed.h"

/*
 * How much of the ground a Deed occupies, in one place instead of four.
 *
 * Eight sectors is four kilobytes — one physical block on every medium in use,
 * so reading a Deed is one physical read and writing one is one physical
 * write, with no straddle either way.
 *
 * ‼ src/boot/stage2/stage2.asm spells this number for itself (DEED_SECTORS
 * equ 8), because an assembler cannot ask a C header. That is the same
 * arrangement the prologue offsets have, and deed.h checks those with static
 * asserts; this one is checked by the fact that stage2 stops booting.
 */
#define DEED_PEN_SECTORS  8u
#define DEED_PEN_BYTES    (DEED_PEN_SECTORS * VOLUME_DEED_SECTOR_BYTES)

/*
 * A Deed being written.
 *
 * `block` is the caller's — DEED_PEN_BYTES of it for a whole Deed. `prologue`
 * and `stamps` are the two lengths the seal writes into the prologue and sums
 * over; keeping them in the pen rather than reading them back out of the block
 * is what lets AMEND preserve a prologue longer than this build's struct.
 */
typedef struct {
    uint8_t *block;
    uint32_t bytes;      /* how much room the block has */
    uint32_t prologue;   /* prologue_bytes: where the first stamp starts */
    uint32_t stamps;     /* stamp_bytes: how much of the block the stamps use */
} DeedPen;

/* ------------------------------------------------------------------------- */

static inline void DeedPenCopy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

static inline void DeedPenZero(uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) dst[i] = 0;
}

static inline int DeedPenSame(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/*
 * CRC-32 (ISO 3309), the sum a Deed is checked with — and the same routine GPT
 * is specified in, so one implementation checks a partition table, a Deed and
 * a Ledger. Bitwise rather than table-driven: it runs over four kilobytes
 * twice per volume in the tool and twice per mount in the kernel, and a 1 KiB
 * table in a header included everywhere is a worse trade than those cycles.
 */
static inline uint32_t DeedPenSum(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* The eight bytes that say this is a Deed, written and recognised in one
 * place so a reader and a writer cannot come to spell them differently. */
static inline void DeedPenMagic(uint8_t out[8])
{
    out[0] = VOLUME_DEED_MAGIC_0; out[1] = VOLUME_DEED_MAGIC_1;
    out[2] = VOLUME_DEED_MAGIC_2; out[3] = VOLUME_DEED_MAGIC_3;
    out[4] = VOLUME_DEED_MAGIC_4; out[5] = VOLUME_DEED_MAGIC_5;
    out[6] = VOLUME_DEED_MAGIC_6; out[7] = VOLUME_DEED_MAGIC_7;
}

static inline int DeedPenMagicIsHere(const uint8_t *raw)
{
    uint8_t want[8];
    DeedPenMagic(want);
    return DeedPenSame(raw, want, 8);
}

/* Read the prologue out of a block into a struct, and put it back. Both go
 * through a byte copy because the block is unaligned medium bytes and the
 * struct is packed. */
static inline void DeedPenReadHead(const DeedPen *pen, VolumeDeed *out)
{
    DeedPenCopy((uint8_t *)out, pen->block, (uint32_t)sizeof(VolumeDeed));
}

static inline void DeedPenWriteHead(DeedPen *pen, const VolumeDeed *in)
{
    DeedPenCopy(pen->block, (const uint8_t *)in, (uint32_t)sizeof(VolumeDeed));
}

/* =========================================================================
 *  MAKE — a Deed from nothing
 * ========================================================================= */

/*
 * Start a fresh Deed in `block`.
 *
 * The block is zeroed here rather than left to the caller, and that is a
 * correctness requirement, not tidiness: each stamp is followed by padding to
 * the next four-byte boundary, that padding is INSIDE stamp_bytes, and the sum
 * covers it. Padding that is not zero makes a Deed whose checksum depends on
 * whatever was in the caller's buffer.
 */
static inline void DeedPenOpen(DeedPen *pen, uint8_t *block, uint32_t bytes)
{
    pen->block    = block;
    pen->bytes    = bytes;
    pen->prologue = (uint32_t)sizeof(VolumeDeed);
    pen->stamps   = 0;
    DeedPenZero(block, bytes);

    VolumeDeed head;
    DeedPenZero((uint8_t *)&head, (uint32_t)sizeof(head));
    DeedPenMagic(head.magic);
    head.prologue_bytes = (uint16_t)pen->prologue;
    DeedPenWriteHead(pen, &head);
}

/*
 * Lay one stamp down. 0 on success, -1 when it will not fit.
 *
 * The next stamp starts at the next four-byte boundary after this payload —
 * the rule every walker in the tree steps by, and the same one the Boarding
 * Pass uses one layer up.
 */
static inline int DeedPenSay(DeedPen *pen, uint16_t kind,
                             const void *payload, uint16_t bytes)
{
    uint32_t at   = pen->prologue + pen->stamps;
    uint32_t need = (uint32_t)sizeof(VolumeStamp) + bytes;
    uint32_t step = (need + 3u) & ~3u;
    if (at > pen->bytes || step > pen->bytes - at) return -1;

    VolumeStamp s;
    s.kind  = kind;
    s.bytes = bytes;
    DeedPenCopy(pen->block + at, (const uint8_t *)&s, (uint32_t)sizeof(s));
    DeedPenCopy(pen->block + at + sizeof(s), (const uint8_t *)payload, bytes);
    /* The padding to the boundary stays as DeedPenOpen left it: zero. */
    pen->stamps += step;
    return 0;
}

/* =========================================================================
 *  AMEND — a Deed that already exists
 * ========================================================================= */

/*
 * Pick up the Deed already in `block` — the bytes as they came off the medium.
 *
 * 0 when this is a Deed whose two lengths fit the room it lives in; -1
 * otherwise. It deliberately does NOT check the checksum: the kernel's reader
 * has already done that before handing these bytes to anybody, and a pen that
 * checked again would be a second opinion nobody asked for. What it does check
 * is what it is about to write through.
 */
static inline int DeedPenTake(DeedPen *pen, uint8_t *block, uint32_t bytes)
{
    pen->block = block;
    pen->bytes = bytes;
    pen->prologue = 0;
    pen->stamps   = 0;

    if (bytes < sizeof(VolumeDeed))  return -1;
    if (!DeedPenMagicIsHere(block))  return -1;

    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    if (head.prologue_bytes < sizeof(VolumeDeed)) return -1;
    if (head.prologue_bytes > bytes)              return -1;
    if (head.stamp_bytes > bytes - head.prologue_bytes) return -1;

    pen->prologue = head.prologue_bytes;
    pen->stamps   = head.stamp_bytes;
    return 0;
}

/*
 * Find a stamp and hand back its payload TO BE WRITTEN ON. NULL when this Deed
 * does not carry that kind — a fact and not a failure, exactly as it is for
 * the reader.
 *
 * The walk is the reader's walk (deed.c DeedStamp), and it must stay the
 * reader's walk: a writer that stepped differently would put a stamp where the
 * reader will not look for it.
 */
static inline void *DeedPenFind(const DeedPen *pen, uint16_t kind,
                                uint16_t *out_bytes)
{
    if (!pen->block || pen->prologue == 0) return 0;

    uint32_t left = pen->stamps;
    uint8_t *p    = pen->block + pen->prologue;

    while (left >= sizeof(VolumeStamp)) {
        VolumeStamp s;
        DeedPenCopy((uint8_t *)&s, p, (uint32_t)sizeof(s));

        uint32_t payload = s.bytes;
        if (payload > left - sizeof(VolumeStamp)) return 0;

        if (s.kind == kind) {
            if (out_bytes) *out_bytes = s.bytes;
            return p + sizeof(VolumeStamp);
        }

        uint32_t step = ((uint32_t)sizeof(VolumeStamp) + payload + 3u) & ~3u;
        if (step > left) return 0;
        p    += step;
        left -= step;
    }
    return 0;
}

/* =========================================================================
 *  The three numbers, and the seal
 * ========================================================================= */

static inline void DeedPenSetUuid(DeedPen *pen, const uint8_t uuid[16])
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    DeedPenCopy(head.uuid, uuid, 16);
    DeedPenWriteHead(pen, &head);
}

/* How far the volume's ground runs, and where inside it the far copy sits.
 * These two and the LAYOUT stamp are the whole of what growth changes. */
static inline void DeedPenSetGround(DeedPen *pen, uint64_t sectors,
                                    uint64_t tail_sector)
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    head.sectors     = sectors;
    head.tail_sector = tail_sector;
    DeedPenWriteHead(pen, &head);
}

/* Which of the two copies this one is. The pair is written from the same
 * numbers and differs in this byte alone, so a head read where a tail belongs
 * is a misdirected read that says so instead of a valid-looking Deed. */
static inline void DeedPenSetRole(DeedPen *pen, uint32_t role)
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    head.role = role;
    DeedPenWriteHead(pen, &head);
}

/*
 * Sign it. Nothing written before this is believed by anybody.
 *
 * The sum covers the prologue with its own crc32 field zeroed, then exactly
 * stamp_bytes of stamps — over what the Deed says it is, never over the
 * padding that follows it to the end of the block. That is the one pass the
 * reader takes, in the one order the reader takes it.
 */
static inline void DeedPenSeal(DeedPen *pen)
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    head.prologue_bytes = (uint16_t)pen->prologue;
    head.stamp_bytes    = (uint16_t)pen->stamps;
    head.crc32          = 0;
    DeedPenWriteHead(pen, &head);

    head.crc32 = DeedPenSum(pen->block, pen->prologue + pen->stamps);
    DeedPenWriteHead(pen, &head);
}

#endif /* DEED_PEN_H */
