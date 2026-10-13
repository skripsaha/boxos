/*
 * create_tagfs_cargo.c — what a volume is packed with: its files' tags and the
 * shipment's pedigree.
 *
 * Both formats intern the same vocabulary the same way — the reserved keys at
 * ids 0..11, a file's stem first — and stamp the same pedigree into the Deed,
 * so the part of a volume that is not its format is said once, here.
 */

#include "create_tagfs.h"
#include "buildplate_format.h"   /* the verdict this volume copies out of the kernel it packs */
#include "tagfs_reserved.h"      /* shared reserved-tag vocabulary (-I src/include) */

TagEntry* g_tags       = NULL;
uint32_t  g_tag_count  = 0;
static uint32_t  g_tag_cap    = 0;

/* ====================================================================
 * Tag interning
 * ==================================================================== */

static uint16_t intern_tag(const char* key, const char* value) {
    /* Check for existing */
    for (uint32_t i = 0; i < g_tag_count; i++) {
        if (strcmp(g_tags[i].key, key) != 0) continue;
        if (value == NULL && g_tags[i].value == NULL) return g_tags[i].tag_id;
        if (value && g_tags[i].value && strcmp(g_tags[i].value, value) == 0)
            return g_tags[i].tag_id;
    }

    /* Add new */
    if (g_tag_count >= g_tag_cap) {
        g_tag_cap = g_tag_cap == 0 ? 32 : g_tag_cap * 2;
        g_tags = realloc(g_tags, sizeof(TagEntry) * g_tag_cap);
        if (!g_tags) { fprintf(stderr, "realloc failed\n"); exit(1); }
    }

    uint16_t id = (uint16_t)g_tag_count;
    g_tags[g_tag_count].key    = strdup(key);
    g_tags[g_tag_count].value  = value ? strdup(value) : NULL;
    g_tags[g_tag_count].tag_id = id;
    g_tag_count++;
    return id;
}

/* Intern the reserved vocabulary first so it claims tag_ids 0..11 — the same
 * on-disk ID contract the kernel's format-seed produces. The kernel restores
 * each tag at its stored id (tag_registry.c RegistryAdopt), so the
 * auth-privilege keys (god/stopped/bypass/network) land at ids < 64 and their
 * (1ULL<<id) masks are non-zero. Reserved keys that a file also carries dedup
 * back onto these low ids; genuinely new file tags start at id 12. */
void seed_reserved_tags(void) {
#define X(k) (void)intern_tag(k, NULL);
    TAGFS_RESERVED_KEYS(X)
#undef X
}

/* ==================================================================
 * THE SHIPMENT'S PEDIGREE
 *
 * The kernel carries a plate saying which tree built it. It cannot say
 * anything about the other sixty-nine files on the volume, and a machine boots
 * all seventy. So the volume gets its own stamp: one number over every file
 * packed, in pack order, contents and all — and the kernel's verdict COPIED
 * out of the plate as the kernel goes by, so the two halves cannot hold two
 * opinions. See VolumePacked in volume_deed.h.
 * ================================================================== */

#define SHIPMENT_FNV_OFFSET 1469598103934665603ULL
#define SHIPMENT_FNV_PRIME  1099511628211ULL

static void shipment_fold(uint64_t* h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { *h ^= (uint64_t)b[i]; *h *= SHIPMENT_FNV_PRIME; }
}

/* Every file this volume is being given, in the order it is given them: the
 * name as written on the command line, then the bytes. The name is in the
 * number as well as the contents, because two files whose contents were
 * swapped are a different shipment. */
static uint64_t shipment_fingerprint(int file_count, char* argv[], int first) {
    uint64_t h = SHIPMENT_FNV_OFFSET;
    for (int i = 0; i < file_count; i++) {
        const char* path = argv[first + i * 2];
        shipment_fold(&h, path, strlen(path) + 1);
        FILE* f = fopen(path, "rb");
        if (!f) continue;            /* the pack itself will refuse it below */
        static unsigned char buf[1 << 16];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) shipment_fold(&h, buf, got);
        fclose(f);
        shipment_fold(&h, "", 1);
    }
    return h;
}

/* Read the plate out of the kernel being packed. Found by its tag, exactly as
 * tools/buildplate finds it, and required to appear exactly once — the same
 * rule, because a kernel with two plates is a kernel nothing can vouch for. */
static void kernel_plate(const char* path, uint64_t* tree, uint32_t* verdict) {
    *tree = 0;
    *verdict = BUILDPLATE_VERDICT_UNSTAMPED;
    if (!path) return;

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= (long)sizeof(BuildPlate)) { fclose(f); return; }
    unsigned char* img = (unsigned char*)malloc((size_t)n);
    if (!img) { fclose(f); return; }
    if (fread(img, 1, (size_t)n, f) != (size_t)n) { free(img); fclose(f); return; }
    fclose(f);

    static const char tag[BUILDPLATE_TAG_BYTES] = BUILDPLATE_TAG_INIT;
    long at = -1; int seen = 0;
    for (size_t i = 0; i + sizeof(BuildPlate) <= (size_t)n; i++) {
        if (memcmp(img + i, tag, BUILDPLATE_TAG_BYTES) == 0) { seen++; if (at < 0) at = (long)i; }
    }
    if (seen == 1) {
        BuildPlate p;
        memcpy(&p, img + at, sizeof(p));
        if (p.Magic == BUILDPLATE_MAGIC && p.Version == BUILDPLATE_VERSION) {
            *tree = p.TreeFingerprint;
            *verdict = p.Verdict;
        }
    } else if (seen > 1) {
        printf("  WARNING: the kernel being packed carries %d plates; the volume "
               "will say it knows nothing about which tree made it\n", seen);
    }
    free(img);
}

/*
 * The shipment's own half of the pedigree, computed once and written into
 * both copies of the Deed. The kernel's verdict rides along, copied out of the
 * plate in the kernel file itself — so a volume packed over a harness
 * injection says UNDER TEST at mount even if the kernel it carries never gets
 * far enough to say it. `kernel_path` is NULL for a volume with no kernel.
 */
VolumePacked PackedStamp(int file_count, char* argv[], int first,
                         const char* kernel_path) {
    VolumePacked packed;
    memset(&packed, 0, sizeof(packed));
    packed.packed_unix = (uint64_t)time(NULL);
    packed.shipment    = shipment_fingerprint(file_count, argv, first);
    packed.file_count  = (uint32_t)file_count;
    /* Through locals, because VolumePacked is packed and the address of a
     * packed member is not necessarily aligned — the compiler is right to say
     * so, and this tree builds with no warnings. */
    uint64_t plate_tree = 0;
    uint32_t plate_verdict = BUILDPLATE_VERDICT_UNSTAMPED;
    kernel_plate(kernel_path, &plate_tree, &plate_verdict);
    packed.tree    = plate_tree;
    packed.verdict = plate_verdict;
    printf("  Pedigree:      shipment %016llx over %d file(s), tree %016llx, %s\n",
           (unsigned long long)packed.shipment, file_count,
           (unsigned long long)packed.tree,
           packed.verdict == BUILDPLATE_VERDICT_CLEAN      ? "clean" :
           packed.verdict == BUILDPLATE_VERDICT_UNDER_TEST ? "UNDER TEST" :
                                                            "the kernel carries no plate");
    return packed;
}

/* Extract filename stem: "kernel.bin" → "kernel", "files.elf" → "files" */
static void extract_stem(const char* filename, char* stem, size_t stem_size) {
    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= stem_size) len = stem_size - 1;
    memcpy(stem, base, len);
    stem[len] = '\0';
}

/* Whether a file wears the label `kernel` — its name stems to it, or it was
 * given it — which is the file the Deed's BOOT stamp points a loader at. */
int KernelTagged(const uint16_t* ids, uint16_t count) {
    for (uint16_t t = 0; t < count; t++) {
        if (g_tags[ids[t]].value == NULL && strcmp(g_tags[ids[t]].key, "kernel") == 0)
            return 1;
    }
    return 0;
}

/* Check if tag_id already in array */
static int has_tag_id(const uint16_t* ids, uint16_t count, uint16_t id) {
    for (uint16_t i = 0; i < count; i++) {
        if (ids[i] == id) return 1;
    }
    return 0;
}

/* Parse comma-separated tag string like "system,type:elf,utility" */
void parse_tags(const char* tag_string, const char* filename,
                uint16_t** out_ids, uint16_t* out_count) {
    uint16_t  ids[256];
    uint16_t  count = 0;

    /* Auto-label tag from filename stem (always first) */
    char stem[256];
    extract_stem(filename, stem, sizeof(stem));
    if (stem[0] != '\0') {
        ids[count++] = intern_tag(stem, NULL);
    }

    char buf[4096];
    strncpy(buf, tag_string, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* token = strtok(buf, ",");
    while (token && count < 256) {
        while (*token == ' ') token++;

        uint16_t id;
        char* colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            id = intern_tag(token, colon + 1);
        } else {
            id = intern_tag(token, NULL);
        }

        /* Deduplicate: skip if already in the array */
        if (!has_tag_id(ids, count, id)) {
            ids[count++] = id;
        }
        token = strtok(NULL, ",");
    }

    *out_count = count;
    *out_ids = malloc(sizeof(uint16_t) * count);
    memcpy(*out_ids, ids, sizeof(uint16_t) * count);
}

/* The vocabulary interned for this volume, given back. */
void CargoRelease(void) {
    for (uint32_t i = 0; i < g_tag_count; i++) {
        free(g_tags[i].key);
        free(g_tags[i].value);
    }
    free(g_tags);
}
