#ifndef DEED_PEN_H
#define DEED_PEN_H


#include "volume_deed.h"

#define DEED_PEN_SECTORS  8u
#define DEED_PEN_BYTES    (DEED_PEN_SECTORS * VOLUME_DEED_SECTOR_BYTES)

typedef struct {
    uint8_t *block;
    uint32_t bytes;
    uint32_t prologue;
    uint32_t stamps;
} DeedPen;


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

static inline void DeedPenReadHead(const DeedPen *pen, VolumeDeed *out)
{
    DeedPenCopy((uint8_t *)out, pen->block, (uint32_t)sizeof(VolumeDeed));
}

static inline void DeedPenWriteHead(DeedPen *pen, const VolumeDeed *in)
{
    DeedPenCopy(pen->block, (const uint8_t *)in, (uint32_t)sizeof(VolumeDeed));
}


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
    pen->stamps += step;
    return 0;
}


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


static inline void DeedPenSetUuid(DeedPen *pen, const uint8_t uuid[16])
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    DeedPenCopy(head.uuid, uuid, 16);
    DeedPenWriteHead(pen, &head);
}

static inline void DeedPenSetGround(DeedPen *pen, uint64_t sectors,
                                    uint64_t tail_sector)
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    head.sectors     = sectors;
    head.tail_sector = tail_sector;
    DeedPenWriteHead(pen, &head);
}

static inline void DeedPenSetRole(DeedPen *pen, uint32_t role)
{
    VolumeDeed head;
    DeedPenReadHead(pen, &head);
    head.role = role;
    DeedPenWriteHead(pen, &head);
}

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

#endif