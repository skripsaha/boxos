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
static uint32_t  g_tag_id_most = 0;   /* the last id the volume's format carries */

/* ====================================================================
 * Tag interning
 *
 * ‼ AN ID IS A u32, AND THE FORMAT SAYS HOW FAR IT GOES. It was a u16 cast
 * of the count: the 65537th tag was given id 0 again, and every one after
 * it the id of a tag interned long before — the reserved keys among them, so
 * a file's header named god, bypass or network for tags it was never given,
 * and no census could tell (the ids were whole; only their meaning was
 * wrong). Now the count is the id, whole, and the format part that seeds
 * the vocabulary says the last id its volume carries: format v1's registry
 * and records hold an id in a u16 and stop at TAGFS_V1_TAG_ID_MOST, format
 * v2 writes a varint and stops where the kernel's ids do (§6.4). The tag
 * that would pass it is refused, not given another's number.
 * ==================================================================== */

/* The id of `key` (with `value`, or none), interned when it is new — `whose`
 * names the file it is for, in a refusal. 0, or -1 having said why. */
static int intern_tag(const char* key, const char* value, const char* whose, uint32_t* id) {
    for (uint32_t i = 0; i < g_tag_count; i++) {
        if (strcmp(g_tags[i].key, key) != 0) continue;
        if ((value == NULL && g_tags[i].value == NULL) ||
            (value && g_tags[i].value && strcmp(g_tags[i].value, value) == 0)) {
            *id = g_tags[i].tag_id;
            return 0;
        }
    }

    size_t key_len = strlen(key), value_len = value ? strlen(value) : 0;
    if (key_len > TAG_KEY_WIDTH || key_len + value_len > TAG_TEXT_ROOM) {
        fprintf(stderr, "%s: the tag '%.40s%s' has a key of %zu bytes and a value of %zu: the kernel holds "
                        "a tag's key in %u bytes and its key and value together in %u (tag_spelling.h)\n",
                whose, key, key_len > 40 ? "..." : "", key_len, value_len, TAG_KEY_WIDTH, TAG_TEXT_ROOM);
        return -1;
    }
    if (g_tag_count > g_tag_id_most) {
        fprintf(stderr, "%s: the tag '%.40s%s' would be the volume's tag id %u, and its format carries "
                        "none past %u\n", whose, key, key_len > 40 ? "..." : "", g_tag_count, g_tag_id_most);
        return -1;
    }
    if (g_tag_count >= g_tag_cap) {
        g_tag_cap = g_tag_cap == 0 ? 32 : g_tag_cap * 2;
        g_tags = realloc(g_tags, sizeof(TagEntry) * g_tag_cap);
        if (!g_tags) { fprintf(stderr, "realloc failed\n"); exit(1); }
    }
    g_tags[g_tag_count].key    = strdup(key);
    g_tags[g_tag_count].value  = value ? strdup(value) : NULL;
    g_tags[g_tag_count].tag_id = g_tag_count;
    *id = g_tag_count++;
    return 0;
}

/* Intern the reserved vocabulary first so it claims tag_ids 0..11 — the same
 * on-disk ID contract the kernel's format-seed produces. The kernel restores
 * each tag at its stored id (tag_registry.c RegistryAdopt), so the
 * auth-privilege keys (god/stopped/bypass/network) land at ids < 64 and their
 * (1ULL<<id) masks are non-zero. Reserved keys that a file also carries dedup
 * back onto these low ids; genuinely new file tags start at id 12. `id_most`
 * is the last id the volume's format carries; the twelve are under it in
 * either format, so none of them is ever refused. */
void seed_reserved_tags(uint32_t id_most) {
    uint32_t id;
    g_tag_id_most = id_most;
#define X(k) (void)intern_tag(k, NULL, "the reserved vocabulary", &id);
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

/* A file name's stem — "kernel.bin" is "kernel", "files.elf" "files" — in
 * a string of its own; NULL when there is no memory for it. */
static char* StemOf(const char* filename) {
    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    char* stem = malloc(len + 1);
    if (stem) {
        memcpy(stem, base, len);
        stem[len] = '\0';
    }
    return stem;
}

/* The role a manifest gives a file: the first word of its tag string, written
 * "@system", "@boot" or "@kernel", and no tag — a file's tags are its user's,
 * and the system does not read them. The word is taken off the string
 * (*tag_string moves past it, and the spaces or the comma after it); a string
 * with no such word leaves the file ordinary. The file given `@kernel` is the
 * one the Deed's BOOT stamp points a loader at. 0, or -1 having said why. */
static int RoleTake(const char** tag_string, const char* filename, uint32_t* role) {
    static const struct { const char* word; uint32_t role; } roles[] = {
        { "system", TALLY_FILE_ROLE_SYSTEM },
        { "boot",   TALLY_FILE_ROLE_BOOT },
        { "kernel", TALLY_FILE_ROLE_KERNEL },
    };
    const char* at = *tag_string;
    *role = 0;
    while (*at == ' ') at++;
    if (*at != '@') return 0;
    const char* end = ++at;
    while (*end && *end != ' ' && *end != ',') end++;
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        if (strlen(roles[i].word) != (size_t)(end - at) || strncmp(at, roles[i].word, (size_t)(end - at)) != 0)
            continue;
        *role = roles[i].role;
        while (*end == ' ' || *end == ',') end++;
        *tag_string = end;
        return 0;
    }
    fprintf(stderr, "%s: '@%.*s' is no role of a file — it is @system, @boot or @kernel\n", filename,
            (int)(end - at), at);
    return -1;
}

static int has_tag_id(const uint32_t* ids, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == id) return 1;
    }
    return 0;
}

/* One tag of a list, "key" or "key:value", interned and added to `ids` once. */
static int TagTake(char* token, const char* whose, uint32_t* ids, uint32_t* count) {
    uint32_t id;
    char*    colon = strchr(token, ':');
    if (colon) *colon = '\0';
    if (intern_tag(token, colon ? colon + 1 : NULL, whose, &id) != 0) return -1;
    if (!has_tag_id(ids, *count, id)) ids[(*count)++] = id;
    return 0;
}

/*
 * A file's role, if the string names one first ("@system utility,fun"), and its
 * tags: its name's stem first, then a comma-separated list like
 * "type:elf,utility", each once. 0, or -1 having said why.
 *
 * ‼ THE LIST IS TAKEN WHOLE. It was copied into 4096 bytes and the rest cut
 * off, its tags counted into 256 and the rest dropped, the stem cut at 255 —
 * each without a word, and the volume made of what was left. Now every tag
 * given is interned or the volume is refused: how many a file can carry is
 * its format's to judge (format v1's record in one pool block, V1PackPlace;
 * format v2 has no such width), and how wide one can be the kernel's
 * (TAG_KEY_WIDTH, TAG_TEXT_ROOM).
 */
int parse_tags(const char* tag_string, const char* filename, uint32_t** out_ids, uint32_t* out_count,
               uint32_t* out_role) {
    if (RoleTake(&tag_string, filename, out_role) != 0) return -1;
    uint32_t  room  = 2;   /* the stem, and one more tag than there are commas */
    uint32_t  count = 0;
    for (const char* c = tag_string; *c; c++) room += *c == ',';
    char*     stem  = StemOf(filename);
    char*     list  = strdup(tag_string);
    uint32_t* ids   = malloc(sizeof(uint32_t) * room);
    int       taken = stem && list && ids;
    if (!taken) fprintf(stderr, "%s: no memory for its tags\n", filename);

    /* The stem is a key whole, never split at ':' (tag_spelling.h, TagSpellKey). */
    if (taken && stem[0] != '\0') {
        taken = intern_tag(stem, NULL, filename, &ids[0]) == 0;
        count = taken ? 1u : 0u;
    }
    for (char* token = taken ? strtok(list, ",") : NULL; token; token = strtok(NULL, ",")) {
        while (*token == ' ') token++;
        if (TagTake(token, filename, ids, &count) != 0) {
            taken = 0;
            break;
        }
    }
    free(stem);
    free(list);
    if (!taken) {
        free(ids);
        return -1;
    }
    *out_ids   = ids;
    *out_count = count;
    return 0;
}

/* The vocabulary interned for this volume, given back. */
void CargoRelease(void) {
    for (uint32_t i = 0; i < g_tag_count; i++) {
        free(g_tags[i].key);
        free(g_tags[i].value);
    }
    free(g_tags);
}
