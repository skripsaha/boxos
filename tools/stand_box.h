#ifndef STAND_BOX_H
#define STAND_BOX_H

/*
 * A box for the stands of what BoxLib reads out of programs (form_stand,
 * recipe_stand): the least a box is — code and its door `start` — and the
 * parts a case adds, written by the one pen (box_pen.h) with BoxLib's own
 * SHA-256, into a heap buffer of exactly its length. Integer types come from
 * BoxLib's headers, included before this one.
 */

#include "box/sha256.h"
#include "box_pen.h"

#define STAND_BOX_PARTS_MAX 8u

static void StandBoxHashBegin(void *state)
{
    sha256_begin((Sha256 *)state);
}

static void StandBoxHashAdd(void *state, const uint8_t *bytes, uint64_t count)
{
    sha256_add((Sha256 *)state, bytes, (size_t)count);
}

static void StandBoxHashEnd(void *state, uint8_t digest[BOX_HASH_BYTES])
{
    sha256_end((Sha256 *)state, digest);
}

/* The box of code, doors and the `extra_count` parts at `extra`; *bytes its
 * length. NULL when the pen refuses what it was given — a case that wants a
 * box the reader would not take writes the bytes itself. */
static uint8_t *StandBox(const BoxPenPart *extra, uint32_t extra_count, uint64_t *bytes)
{
    static const uint8_t code[16] = { 0xF3, 0x0F, 0x1E, 0xFA, 0xC3 };
    static uint8_t       doors[64];
    BoxPenDoor           start = { "start", 0 };
    uint64_t             doors_bytes;
    BoxPenDoors(&start, 1, NULL, &doors_bytes);
    if (doors_bytes > sizeof(doors) || extra_count > STAND_BOX_PARTS_MAX - 2) return NULL;
    BoxPenDoors(&start, 1, doors, &doors_bytes);

    BoxPenPart parts[STAND_BOX_PARTS_MAX] = {
        { "code", code, sizeof(code), sizeof(code), 0, BOX_FLAG_MUST_UNDERSTAND },
        { "doors", doors, doors_bytes, doors_bytes, 0, BOX_FLAG_MUST_UNDERSTAND },
    };
    for (uint32_t i = 0; i < extra_count; i++) parts[2 + i] = extra[i];

    uint32_t count = 2 + extra_count, refused;
    if (BoxPenMeasure(parts, count, bytes) != BOX_OK) return NULL;
    uint8_t *out = calloc(1, (size_t)*bytes);
    if (!out) return NULL;
    Sha256    state;
    BoxHasher hasher = { &state, StandBoxHashBegin, StandBoxHashAdd, StandBoxHashEnd };
    if (BoxPenWrite(parts, count, out, *bytes, &hasher, &refused) != BOX_OK) {
        free(out);
        return NULL;
    }
    return out;
}

/* Where `text` is in the `size` bytes at `box` — a part's bytes, found to be
 * damaged by a case. */
static uint8_t *StandBoxFind(uint8_t *box, uint64_t size, const char *text)
{
    size_t length = strlen(text);
    for (uint64_t at = 0; length <= size && at <= size - length; at++)
        if (memcmp(box + at, text, length) == 0) return box + at;
    return NULL;
}

#endif /* STAND_BOX_H */
