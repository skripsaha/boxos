#include "boarding.h"
#include "vmm.h"
#include "klib.h"
#include "crypto.h"

static uint8_t  g_pass[BOARDING_PASS_BYTES];
static bool     g_present = false;

static const BoardingPassHeader* pass_header(void)
{
    return (const BoardingPassHeader*)g_pass;
}

static uint16_t stamp_stride(uint16_t payload_bytes)
{
    uint32_t total = (uint32_t)sizeof(BoardingStampHeader) + payload_bytes;
    return (uint16_t)((total + 3u) & ~3u);
}

static bool pass_is_sound(const uint8_t* raw, unsigned* out_stamps,
                          bool* out_sealed)
{
    const BoardingPassHeader* h = (const BoardingPassHeader*)raw;

    if (out_sealed) {
        *out_sealed = false;
    }

    if (h->magic != BOARDING_PASS_MAGIC) {
        return false;
    }
    if (h->version == 0) {
        kprintf("[Boarding] the pass states version 0, which no loader "
                "writes — ignoring it\n");
        return false;
    }
    if (h->version > BOARDING_PASS_VERSION) {
        kprintf("[Boarding] the pass is version %u and this kernel was built "
                "for %u — reading the stamps it knows\n",
                h->version, BOARDING_PASS_VERSION);
    }
    if (h->header_bytes < sizeof(BoardingPassHeader) ||
        h->capacity > BOARDING_PASS_BYTES ||
        h->used_bytes > h->capacity ||
        h->used_bytes < h->header_bytes) {
        kprintf("[Boarding] the pass states lengths that do not fit inside it "
                "(header %u, used %u, capacity %u) — ignoring it\n",
                h->header_bytes, h->used_bytes, h->capacity);
        return false;
    }

    unsigned seen = 0;
    uint16_t at = h->header_bytes;
    uint16_t seal_at = 0;
    uint16_t seal_stride = 0;
    bool     have_seal = false;

    while (at + sizeof(BoardingStampHeader) <= h->used_bytes) {
        const BoardingStampHeader* s = (const BoardingStampHeader*)(raw + at);
        uint16_t stride = stamp_stride(s->bytes);

        if (stride < sizeof(BoardingStampHeader) ||
            (uint32_t)at + stride > h->used_bytes) {
            kprintf("[Boarding] a stamp on the pass runs past the end of it "
                    "— ignoring the pass\n");
            return false;
        }
        if (s->kind == BOARDING_STAMP_SEAL &&
            s->bytes >= sizeof(BoardingSeal)) {
            seal_at     = at;
            seal_stride = stride;
            have_seal   = true;
        }
        seen++;
        at = (uint16_t)(at + stride);
    }

    if (seen != h->count) {
        kprintf("[Boarding] the pass says it carries %u stamp(s) and carries "
                "%u — ignoring it\n", h->count, seen);
        return false;
    }

    if (have_seal) {
        const BoardingSeal* seal =
            (const BoardingSeal*)(raw + seal_at + sizeof(BoardingStampHeader));
        uint32_t covered = (uint32_t)seal_at + sizeof(BoardingStampHeader);

        if ((uint32_t)seal_at + seal_stride != h->used_bytes) {
            kprintf("[Boarding] the seal is not the last stamp on the pass "
                    "— ignoring it\n");
            return false;
        }

        uint32_t have = KCrc32(raw, covered);
        if (have != seal->crc32) {
            kprintf("[Boarding] the seal on the pass is broken: %u bytes sum "
                    "to 0x%x and the loader wrote 0x%x — ignoring it\n",
                    covered, have, seal->crc32);
            return false;
        }
    }

    if (out_stamps) {
        *out_stamps = seen;
    }
    if (out_sealed) {
        *out_sealed = have_seal;
    }
    return true;
}

const void* BoardingPassStamp(uint16_t kind, uint16_t* out_bytes)
{
    if (!g_present) {
        return NULL;
    }

    const BoardingPassHeader* h = pass_header();
    uint16_t at = h->header_bytes;

    while (at + sizeof(BoardingStampHeader) <= h->used_bytes) {
        const BoardingStampHeader* s = (const BoardingStampHeader*)(g_pass + at);
        if (s->kind == kind) {
            if (out_bytes) {
                *out_bytes = s->bytes;
            }
            return g_pass + at + sizeof(BoardingStampHeader);
        }
        at = (uint16_t)(at + stamp_stride(s->bytes));
    }
    return NULL;
}

bool BoardingPassPresent(void)
{
    return g_present;
}

bool BoardingPassVolume(uint8_t out_uuid[16])
{
    uint16_t bytes = 0;
    const BoardingVolume* v =
        (const BoardingVolume*)BoardingPassStamp(BOARDING_STAMP_VOLUME, &bytes);

    if (!v || bytes < sizeof(BoardingVolume)) {
        return false;
    }
    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
    }
    return true;
}

bool BoardingPassMedium(uint8_t* out_firmware, uint8_t* out_bios_drive)
{
    uint16_t bytes = 0;
    const BoardingMedium* m =
        (const BoardingMedium*)BoardingPassStamp(BOARDING_STAMP_MEDIUM, &bytes);

    if (!m || bytes < sizeof(BoardingMedium)) {
        return false;
    }
    if (out_firmware)   *out_firmware   = m->firmware;
    if (out_bios_drive) *out_bios_drive = m->bios_drive;
    return true;
}

static void uuid_to_text(const uint8_t uuid[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[(uuid[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[uuid[i] & 0xF];
    }
    out[32] = '\0';
}

static void boarding_describe(unsigned stamps, bool sealed)
{
    uint8_t uuid[16];
    char text[33];

    kprintf("[Boarding] the loader left a pass with %u stamp(s)\n", stamps);

    if (sealed) {
        kprintf("[Boarding]   sealed, and the seal agrees — the block is the "
                "one the loader wrote\n");
    } else {
        kprintf("[Boarding]   it carries no seal, so nothing here could be "
                "checked — taken at its word\n");
    }

    uint8_t firmware = 0, drive = 0xFF;
    if (BoardingPassMedium(&firmware, &drive)) {
        if (firmware == BOARDING_FIRMWARE_BIOS) {
            kprintf("[Boarding]   came in through BIOS, from drive 0x%x\n",
                    drive);
        } else {
            kprintf("[Boarding]   came in through UEFI\n");
        }
    }

    uint16_t bytes = 0;
    const BoardingLoader* l =
        (const BoardingLoader*)BoardingPassStamp(BOARDING_STAMP_LOADER, &bytes);
    if (l && bytes >= sizeof(BoardingLoader)) {
        char name[13];
        memcpy(name, l->name, 12);
        name[12] = '\0';
        kprintf("[Boarding]   written by %s %u.%u\n", name, l->major, l->minor);
    }

    if (BoardingPassVolume(uuid)) {
        uuid_to_text(uuid, text);
        kprintf("[Boarding]   read out of the volume %s\n", text);
    } else {
        kprintf("[Boarding]   it does not say which volume — the Boardroom "
                "will have to choose by rule\n");
    }

}

void BoardingPassInit(void)
{
    const uint8_t* raw = (const uint8_t*)vmm_phys_to_virt(BOARDING_PASS_ADDR);
    if (!raw) {
        return;
    }

    unsigned stamps = 0;
    bool     sealed = false;
    if (!pass_is_sound(raw, &stamps, &sealed)) {
        kprintf("[Boarding] the loader left no pass — nothing to say which "
                "volume this kernel came from\n");
        g_present = false;
        return;
    }

    memcpy(g_pass, raw, ((const BoardingPassHeader*)raw)->used_bytes);
    g_present = true;

    boarding_describe(stamps, sealed);
}