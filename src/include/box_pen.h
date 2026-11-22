#ifndef BOX_PEN_H
#define BOX_PEN_H

/*
 * The pen a box (box_format.h) is written with — by tools/pack on the host,
 * by the assembler inside BoxOS — and the only place box bytes are composed.
 *
 * A writer that kept its own copy of the rules would come to disagree with
 * the reader the day one of them changed. So the pen lays the bytes out the
 * one way a box is written and then reads what it wrote with the one reader
 * (box_reader.h), parts with a layout of their own included: it hands back
 * only a box the reader accepts, and when the reader would refuse, the pen
 * says why in the reader's own verdict.
 *
 * Beside the box itself, the three parts with a layout of their own are
 * written here too — doors, the thread template, the fixups — each in two
 * calls on one function: with no `out` it measures, with one it writes.
 *
 * No allocation and no library: the caller gives the room the pen measured.
 * Integer types come from the includer (see box_format.h).
 */

#include "box_reader.h"

/* One part to put in a box. `Memory` is its size in memory — the same as
 * `Size` for every kind but data. `Place` is a loadable part's offset from the
 * image base, 0 for any other. */
typedef struct {
    const char    *Name;
    const uint8_t *Bytes;
    uint64_t       Size;
    uint64_t       Memory;
    uint64_t       Place;
    uint32_t       Flags;
} BoxPenPart;

typedef struct {
    const char *Name;
    uint64_t    Offset;   /* from the image base */
} BoxPenDoor;

/* ── bytes ───────────────────────────────────────────────────────────────── */

static inline void BoxPut16(uint8_t *at, uint16_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static inline void BoxPut32(uint8_t *at, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) at[i] = (uint8_t)(value >> (8 * i));
}

static inline void BoxPut64(uint8_t *at, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) at[i] = (uint8_t)(value >> (8 * i));
}

static inline void BoxPenCopy(uint8_t *to, const uint8_t *from, uint64_t count)
{
    for (uint64_t i = 0; i < count; i++) to[i] = from[i];
}

static inline uint64_t BoxPenLength(const char *text)
{
    uint64_t length = 0;
    while (text[length] != '\0') length++;
    return length;
}

static inline int BoxPenOrder(const char *a, const char *b)
{
    return BoxNameOrder((const uint8_t *)a, BoxPenLength(a), (const uint8_t *)b, BoxPenLength(b));
}

/* ── the box ─────────────────────────────────────────────────────────────── */

/* Where each part goes when the parts, in table order, are laid out after
 * names ending at `from`; *end where the last ends. False when a name is too
 * long for an entry or the sizes run past 2^64. */
static inline bool BoxPenLayout(const BoxPenPart *parts, uint32_t count, uint64_t from,
                                uint64_t *offsets, uint64_t *end)
{
    uint64_t at = from;
    for (uint32_t i = 0; i < count; i++) {
        bool     bare;
        uint64_t length = BoxPenLength(parts[i].Name), offset;
        if (length > 0xFFFFu) return false;
        BoxKind  named = BoxKindOf((const uint8_t *)parts[i].Name, length, &bare);
        uint64_t align = BoxKindLoadable(named) ? BOX_LOADABLE_ALIGN : BOX_PART_ALIGN;
        if (!BoxAlignUp(at, align, ~0ull, &offset) || parts[i].Size > ~0ull - offset) return false;
        if (offsets) offsets[i] = offset;
        at = offset + parts[i].Size;
    }
    *end = at;
    return true;
}

/* The parts put in table order — sorted by name, in place — and the bytes the
 * box of them takes. */
static inline BoxVerdict BoxPenMeasure(BoxPenPart *parts, uint32_t count, uint64_t *bytes)
{
    if (count == 0) return BOX_NO_PARTS;
    for (uint32_t i = 1; i < count; i++) {
        BoxPenPart held = parts[i];
        uint32_t   j    = i;
        for (; j > 0 && BoxPenOrder(parts[j - 1].Name, held.Name) > 0; j--) parts[j] = parts[j - 1];
        parts[j] = held;
    }

    uint64_t names = 0;
    for (uint32_t i = 0; i < count; i++) names += BoxPenLength(parts[i].Name);
    uint64_t prefix = BOX_HEADER_BYTES + (uint64_t)count * BOX_ENTRY_BYTES + names;
    return BoxPenLayout(parts, count, prefix, 0, bytes) ? BOX_OK : BOX_PART_OUTSIDE;
}

/* The box of `count` parts BoxPenMeasure put in order, written into the
 * `bytes` bytes it measured at `out`, its hashes and seal made with `hasher`,
 * and read back whole. BOX_OK, or the reader's verdict on what was written
 * and in *refused the part it is about (BOX_NO_PART for the box). */
static inline BoxVerdict BoxPenWrite(const BoxPenPart *parts, uint32_t count, uint8_t *out,
                                     uint64_t bytes, const BoxHasher *hasher, uint32_t *refused)
{
    static const uint8_t magic[8] = { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3,
                                      BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 };
    *refused = BOX_NO_PART;
    if (count == 0) return BOX_NO_PARTS;

    uint64_t table = BOX_HEADER_BYTES;
    uint64_t names = table + (uint64_t)count * BOX_ENTRY_BYTES;
    uint64_t names_size = 0, end = 0;
    for (uint32_t i = 0; i < count; i++) names_size += BoxPenLength(parts[i].Name);
    if (!BoxPenLayout(parts, count, names + names_size, 0, &end) || end != bytes)
        return BOX_FILE_SIZE;

    for (uint64_t i = 0; i < bytes; i++) out[i] = 0;
    BoxPenCopy(out + BOX_AT_MAGIC, magic, 8);
    BoxPut32(out + BOX_AT_HEADER_SIZE, BOX_HEADER_BYTES);
    BoxPut32(out + BOX_AT_PART_COUNT, count);
    BoxPut64(out + BOX_AT_TABLE, table);
    BoxPut64(out + BOX_AT_NAMES, names);
    BoxPut64(out + BOX_AT_NAMES_SIZE, names_size);
    BoxPut64(out + BOX_AT_FILE_SIZE, bytes);

    uint64_t name_at = 0, at = names + names_size;
    for (uint32_t i = 0; i < count; i++) {
        const BoxPenPart *part   = &parts[i];
        uint8_t          *entry  = out + table + (uint64_t)i * BOX_ENTRY_BYTES;
        uint64_t          length = BoxPenLength(part->Name);
        uint64_t          offset;
        bool              bare;
        BoxKind           named  = BoxKindOf((const uint8_t *)part->Name, length, &bare);

        BoxAlignUp(at, BoxKindLoadable(named) ? BOX_LOADABLE_ALIGN : BOX_PART_ALIGN, ~0ull, &offset);
        BoxPenCopy(out + names + name_at, (const uint8_t *)part->Name, length);
        BoxPenCopy(out + offset, part->Bytes, part->Size);

        BoxPut64(entry + BOX_ENTRY_OFFSET, offset);
        BoxPut64(entry + BOX_ENTRY_SIZE, part->Size);
        BoxPut64(entry + BOX_ENTRY_MEMORY, part->Memory);
        BoxPut64(entry + BOX_ENTRY_PLACE, part->Place);
        BoxPut32(entry + BOX_ENTRY_FLAGS, part->Flags);
        BoxPut32(entry + BOX_ENTRY_NAME_AT, (uint32_t)name_at);
        BoxPut16(entry + BOX_ENTRY_NAME_LEN, (uint16_t)length);
        BoxHashBytes(hasher, out + offset, part->Size, entry + BOX_ENTRY_HASH);

        name_at += length;
        at = offset + part->Size;
    }
    BoxSealCompute(out, names + names_size, hasher, out + BOX_AT_SEAL);

    /* Read back as any reader will read it. */
    BoxHead    head;
    BoxTable   box;
    BoxPart    part;
    BoxVerdict verdict = BoxReadHead(out, bytes, bytes, &head);
    if (verdict != BOX_OK) return verdict;
    verdict = BoxRead(out, bytes, &head, &box);
    if (verdict == BOX_OK) verdict = BoxGapsZero(out, &box);
    if (verdict != BOX_OK) {
        *refused = box.Refused;
        return verdict;
    }
    if (!BoxSealHolds(&box, hasher)) return BOX_SEAL;

    BoxDoors  doors;
    BoxThread thread;
    if (BoxPartOfKind(&box, BOX_KIND_DOORS, &part) &&
        (verdict = BoxDoorsRead(out + part.Offset, part.Size, &box, &doors)) != BOX_OK) {
        *refused = part.Index;
        return verdict;
    }
    if (BoxPartOfKind(&box, BOX_KIND_THREAD, &part) &&
        (verdict = BoxThreadRead(out + part.Offset, part.Size, &thread)) != BOX_OK) {
        *refused = part.Index;
        return verdict;
    }
    if (BoxPartOfKind(&box, BOX_KIND_FIXUPS, &part) &&
        (verdict = BoxFixupsCheck(out + part.Offset, part.Size, &box)) != BOX_OK) {
        *refused = part.Index;
        return verdict;
    }
    return BOX_OK;
}

/* The part at `index` of the box at `out`, written by BoxPenWrite and its
 * bytes changed since, hashed again into its entry, and the box sealed again:
 * for a part made after the box it is in — a signature over it. The caller
 * reads the box back. */
static inline void BoxPenReseal(uint8_t *out, uint32_t index, const BoxHasher *hasher)
{
    uint8_t *entry = out + BoxU64(out + BOX_AT_TABLE) + (uint64_t)index * BOX_ENTRY_BYTES;
    BoxHashBytes(hasher, out + BoxU64(entry + BOX_ENTRY_OFFSET), BoxU64(entry + BOX_ENTRY_SIZE),
                 entry + BOX_ENTRY_HASH);
    BoxSealCompute(out, BoxU64(out + BOX_AT_NAMES) + BoxU64(out + BOX_AT_NAMES_SIZE), hasher, out + BOX_AT_SEAL);
}

/* ── doors ───────────────────────────────────────────────────────────────── */

/* The `doors` part of `count` doors, sorted by name in place: measured into
 * *bytes, and written at `out` when there is one (*bytes of room). */
static inline void BoxPenDoors(BoxPenDoor *doors, uint32_t count, uint8_t *out, uint64_t *bytes)
{
    for (uint32_t i = 1; i < count; i++) {
        BoxPenDoor held = doors[i];
        uint32_t   j    = i;
        for (; j > 0 && BoxPenOrder(doors[j - 1].Name, held.Name) > 0; j--) doors[j] = doors[j - 1];
        doors[j] = held;
    }

    uint64_t names_at = BOX_DOORS_HEAD_BYTES + (uint64_t)count * BOX_DOOR_BYTES, used = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t length = BoxPenLength(doors[i].Name);
        if (out) {
            uint8_t *entry = out + BOX_DOORS_HEAD_BYTES + (uint64_t)i * BOX_DOOR_BYTES;
            BoxPut64(entry, doors[i].Offset);
            BoxPut32(entry + 8, (uint32_t)used);
            BoxPut16(entry + 12, (uint16_t)length);
            BoxPut16(entry + 14, 0);
            BoxPenCopy(out + names_at + used, (const uint8_t *)doors[i].Name, length);
        }
        used += length;
    }
    if (out) {
        BoxPut32(out, count);
        BoxPut32(out + 4, 0);
    }
    *bytes = names_at + used;
}

/* ── thread ──────────────────────────────────────────────────────────────── */

static inline void BoxPenThread(uint64_t align, const uint8_t *init, uint64_t init_bytes,
                                uint64_t total_bytes, uint8_t *out, uint64_t *bytes)
{
    if (out) {
        BoxPut64(out, align);
        BoxPut64(out + 8, init_bytes);
        BoxPut64(out + 16, total_bytes);
        BoxPut64(out + 24, 0);
        BoxPenCopy(out + BOX_THREAD_HEAD_BYTES, init, init_bytes);
    }
    *bytes = BOX_THREAD_HEAD_BYTES + init_bytes;
}

/* ── fixups ──────────────────────────────────────────────────────────────── */

/* The `fixups` part of `count` places, rising and each a multiple of eight, in
 * the encoding of RELR as its writers make it: a place, then as many bitmaps
 * as there are places within each next 63 words. False when the places do not
 * rise or one is not a multiple of eight. */
static inline bool BoxPenFixups(const uint64_t *places, uint64_t count, uint8_t *out, uint64_t *bytes)
{
    uint64_t words = 0;
    for (uint64_t i = 0; i < count;) {
        if (places[i] % 8 != 0 || (i > 0 && places[i] <= places[i - 1])) return false;
        if (out) BoxPut64(out + words * 8, places[i]);
        words++;
        uint64_t where = places[i] + 8;
        i++;
        for (;;) {
            uint64_t bitmap = 0;
            while (i < count) {
                if (places[i] % 8 != 0 || places[i] <= places[i - 1]) return false;
                uint64_t gap = places[i] - where;
                if (places[i] < where || gap >= (uint64_t)BOX_FIXUP_BITS * 8) break;
                bitmap |= 1ull << (gap / 8);
                i++;
            }
            if (bitmap == 0) break;
            if (out) BoxPut64(out + words * 8, bitmap << 1 | 1u);
            words++;
            where += (uint64_t)BOX_FIXUP_BITS * 8;
        }
    }
    *bytes = words * 8;
    return true;
}

#endif /* BOX_PEN_H */
