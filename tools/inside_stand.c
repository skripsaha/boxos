/* inside_stand — the boxes tools/inside_stand.sh looks inside, written by the
 * one pen (box_pen.h): one with a part of every kind inside says something of,
 * one with one of each thing inside counts, one whose Form is not written as
 * text, and one whose settings miss a default.
 *
 *     inside_stand DIR     writes DIR/full.box, DIR/single.box, DIR/badform.box and
 *                          DIR/badsettings.box
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_host.h"
#include "box_pen.h"
#include "nameplate_format.h"

#define STAND_PARTS 16u

static BoxPenPart g_parts[STAND_PARTS];
static uint32_t   g_count;

static void Part(const char *name, const void *bytes, uint64_t size, uint64_t memory, uint64_t place,
                 uint32_t flags, uint16_t version)
{
    g_parts[g_count++] = (BoxPenPart){ name, (const uint8_t *)bytes, size, memory, place, flags, version };
}

static void Text(const char *name, const char *text, uint32_t flags, uint16_t version)
{
    Part(name, text, strlen(text), strlen(text), 0, flags, version);
}

static bool Write(const char *dir, const char *name)
{
    BoxHostHash state  = { 0 };
    BoxHasher   hasher = BoxHostHasher(&state);
    uint64_t    bytes;
    uint32_t    refused;
    if (BoxPenMeasure(g_parts, g_count, &bytes) != BOX_OK) return false;
    uint8_t *box = malloc(bytes);
    bool     ok  = box && BoxPenWrite(g_parts, g_count, box, bytes, &hasher, &refused) == BOX_OK;
    char     path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *out = ok ? fopen(path, "wb") : NULL;
    ok = out && fwrite(box, 1, bytes, out) == bytes;
    if (out) ok = fclose(out) == 0 && ok;
    free(box);
    BoxHostHashFree(&state);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: inside_stand DIR\n");
        return 2;
    }

    static const uint8_t code[32] = { 0xF3, 0x0F, 0x1E, 0xFA, 0xC3 };
    static const uint8_t data[24] = { 1, 2, 3 };
    uint8_t   doors[64], thread[64], fixups[64];
    uint64_t  doors_bytes, thread_bytes, fixups_bytes;
    BoxPenDoor door[2] = { { "start", 0x4 }, { "stop", 0x10 } };
    uint64_t   places[2] = { 0x1000, 0x1008 };
    BoxPenDoors(door, 2, NULL, &doors_bytes);
    BoxPenDoors(door, 2, doors, &doors_bytes);
    BoxPenThread(16, (const uint8_t *)"tls!", 4, 64, thread, &thread_bytes);
    BoxPenFixups(places, 2, fixups, &fixups_bytes);

    /* A Nameplate of two functions: header, three arrays of two, two names. */
    uint8_t names[NAMEPLATE_TOTAL_BYTES(2, 4)];
    memset(names, 0, sizeof(names));
    NameplateHeader header = { NAMEPLATE_MAGIC, NAMEPLATE_VERSION, 2, 4, 0, sizeof(names) };
    uint32_t arrays[6] = { 0, 8, 8, 8, 0, 2 };
    memcpy(names, &header, sizeof(header));
    memcpy(names + NAMEPLATE_OFFSETS_AT(2), arrays, sizeof(arrays));
    memcpy(names + NAMEPLATE_NAMES_AT(2), "a\0b\0", 4);

    const uint32_t must = BOX_FLAG_MUST_UNDERSTAND;
    Part("code", code, sizeof(code), sizeof(code), 0, must, BOX_PART_VERSION);
    Part("data", data, sizeof(data), 0x1800, 0x1000, must, BOX_PART_VERSION);
    Part("doors", doors, doors_bytes, doors_bytes, 0, must, BOX_PART_VERSION);
    Part("thread", thread, thread_bytes, thread_bytes, 0, must, BOX_PART_VERSION);
    Part("fixups", fixups, fixups_bytes, fixups_bytes, 0, must, BOX_PART_VERSION);
    Part("names", names, sizeof(names), sizeof(names), 0, 0, BOX_PART_VERSION);
    Text("needs", "needs 1\ncpu sse4.2\n", must, BOX_PART_VERSION);
    Text("hints", "hints 1\nstack 1 MiB\n", 0, BOX_PART_VERSION);
    Text("life", "life 1\nstays\nrise\nlonely\n", 0, BOX_PART_VERSION);
    Text("form", "form 1\ntakes words \"what to say\"\n", BOX_FLAG_LOAD_AT_START, BOX_PART_VERSION);
    Text("settings",
         "settings 1\n"
         "setting hours one-of 24 12 default 24 \"how the hours are counted\"\n"
         "setting width number from -5 to 200 cells default 72 next-start \"how wide the column is\"\n"
         "setting greeting text default \"\" \"what it says at start\"\n"
         "setting future gradient default x \"a kind of later\"\n",
         BOX_FLAG_LOAD_AT_START, BOX_PART_VERSION);
    Text("about", "about 1\nname Stand\n", 0, BOX_PART_VERSION);
    Text("about:ru", "about 1\nname Stand\n", BOX_FLAG_COMPRESSED, BOX_PART_VERSION);
    Text("works", "works 2\nsomething later\n", 0, 2);
    Text("paint:brushes", "brushes", 0, BOX_PART_VERSION);
    if (!Write(argv[1], "full.box")) {
        fprintf(stderr, "inside_stand: full.box was not written\n");
        return 2;
    }

    /* One fixup, one function, a byte of zeros, a template of zeros only. */
    uint8_t  one_thread[64], one_fixup[8];
    uint64_t one_thread_bytes, one_fixup_bytes, one_place = 0x1000;
    uint8_t  one_name[NAMEPLATE_TOTAL_BYTES(1, 2)];
    NameplateHeader one_header = { NAMEPLATE_MAGIC, NAMEPLATE_VERSION, 1, 2, 0, sizeof(one_name) };
    uint32_t one_arrays[3] = { 0, 8, 0 };
    memset(one_name, 0, sizeof(one_name));
    memcpy(one_name, &one_header, sizeof(one_header));
    memcpy(one_name + NAMEPLATE_OFFSETS_AT(1), one_arrays, sizeof(one_arrays));
    memcpy(one_name + NAMEPLATE_NAMES_AT(1), "a\0", 2);
    BoxPenThread(8, NULL, 0, 8, one_thread, &one_thread_bytes);
    BoxPenFixups(&one_place, 1, one_fixup, &one_fixup_bytes);
    g_count = 0;
    Part("code", code, sizeof(code), sizeof(code), 0, must, BOX_PART_VERSION);
    Part("data", data, 8, 9, 0x1000, must, BOX_PART_VERSION);
    Part("doors", doors, doors_bytes, doors_bytes, 0, must, BOX_PART_VERSION);
    Part("thread", one_thread, one_thread_bytes, one_thread_bytes, 0, must, BOX_PART_VERSION);
    Part("fixups", one_fixup, one_fixup_bytes, one_fixup_bytes, 0, must, BOX_PART_VERSION);
    Part("names", one_name, sizeof(one_name), sizeof(one_name), 0, 0, BOX_PART_VERSION);
    Part("paint:dot", data, 1, 1, 0, 0, BOX_PART_VERSION);
    if (!Write(argv[1], "single.box")) {
        fprintf(stderr, "inside_stand: single.box was not written\n");
        return 2;
    }

    g_count = 0;
    Part("code", code, sizeof(code), sizeof(code), 0, must, BOX_PART_VERSION);
    Part("doors", doors, doors_bytes, doors_bytes, 0, must, BOX_PART_VERSION);
    Text("form", "form 1\ntakes\twords\n", BOX_FLAG_LOAD_AT_START, BOX_PART_VERSION);
    if (!Write(argv[1], "badform.box")) {
        fprintf(stderr, "inside_stand: badform.box was not written\n");
        return 2;
    }

    g_count = 0;
    Part("code", code, sizeof(code), sizeof(code), 0, must, BOX_PART_VERSION);
    Part("doors", doors, doors_bytes, doors_bytes, 0, must, BOX_PART_VERSION);
    Text("settings", "settings 1\nsetting prompt word \"what stands before\"\n", BOX_FLAG_LOAD_AT_START,
         BOX_PART_VERSION);
    if (!Write(argv[1], "badsettings.box")) {
        fprintf(stderr, "inside_stand: badsettings.box was not written\n");
        return 2;
    }
    return 0;
}
