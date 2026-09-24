/*
 * buildplate — computes a tree's fingerprint, rivets it into a flat kernel
 * image, and refuses to let an image be packed or flashed that the tree in
 * front of it did not make.
 *
 * WHY THIS EXISTS. See the long note in src/include/buildplate_format.h: an
 * image built over a harness injection was written to a stick and booted on
 * live hardware, the harness put the sources back before anybody read the log,
 * and 54% of that log had been printed by code that was no longer in the tree.
 * A clean `git status` said nothing, because build/ is ignored and the mutation
 * was gone. The only thing that can answer "is this image made of this tree"
 * is a number the image carries.
 *
 * WHAT COUNTS AS THE TREE. Every file the build reads: sources, headers, asm,
 * linker scripts, the Makefile, the host tools and the scripts under tools/,
 * and the data files that are packed into the volume. Not docs, not build
 * products, not anything under build/ — editing a note in docs/ does not make
 * an image stale and must not be reported as though it did.
 *
 * ‼ AND NOT `git status`. A dirty tree is the normal state of this project —
 * the owner's tree was 73 files dirty for a whole session and every one of
 * those files was compiled into the image on purpose. "Dirty" is not the
 * question. The question is whether the bytes that were compiled are still
 * the bytes on the disk, and a fingerprint answers exactly that.
 *
 *   buildplate stamp  <image> --root <dir> --keys "..." [--allow-harness]
 *   buildplate verify <image> --root <dir>
 *   buildplate say    <image>
 *
 * Exit codes are the mechanism the Makefile gates on:
 *   0  the plate is readable, clean, and matches the tree
 *   1  bad usage, unreadable file, a tag that is not there or not unique
 *   3  the plate does not match the tree: this image is not made of this tree
 *   4  the plate says UNDER TEST
 *   5  there is no plate, or it cannot be read
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "buildplate_format.h"

#define EXIT_USAGE      1
#define EXIT_MISMATCH   3
#define EXIT_UNDERTEST  4
#define EXIT_NOPLATE    5

/* The two phrases every harness mutation carries, enforced by mut_done in
 * tools/logcheck.sh. They are the independent witness: the harness may forget
 * to pass --allow-harness, but it cannot make an injection that does not say
 * what it is. */
#define WITNESS_A  "logcheck injection"
#define WITNESS_B  "logcheck mutation"

/* ------------------------------------------------------------------ hashing */

/* FNV-1a, 64-bit. Chosen because it is ten lines, has no state to get wrong,
 * and is deterministic across every host this tree is built on. It is a
 * provenance check and not a signature: what it has to survive is an honest
 * mistake, not an adversary. */
#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME  1099511628211ULL

static void fnv_bytes(uint64_t *h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        *h ^= (uint64_t)b[i];
        *h *= FNV64_PRIME;
    }
}

/* ------------------------------------------------------- the file list */

typedef struct {
    char   **path;
    size_t   count;
    size_t   cap;
} PathList;

static void list_push(PathList *l, const char *p)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->path = (char **)realloc(l->path, l->cap * sizeof(char *));
        if (!l->path) { fprintf(stderr, "buildplate: out of memory\n"); exit(EXIT_USAGE); }
    }
    l->path[l->count++] = strdup(p);
}

static int path_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Which suffixes the build actually reads. A whitelist rather than a blacklist
 * because a new kind of build input is a deliberate act — somebody adds it
 * here — while a new kind of junk in the tree happens by itself. */
static int interesting(const char *name)
{
    static const char *suffix[] = {
        ".c", ".h", ".cc", ".cpp", ".hh", ".hpp", ".asm", ".inc", ".s", ".S",
        ".ld", ".sh", ".py", ".txt", ".mk", NULL
    };
    if (strcmp(name, "Makefile") == 0)
        return 1;
    size_t n = strlen(name);
    for (int i = 0; suffix[i]; i++) {
        size_t m = strlen(suffix[i]);
        if (n > m && strcmp(name + n - m, suffix[i]) == 0)
            return 1;
    }
    return 0;
}

/* macOS resource forks and Finder droppings are in the tree on every one of
 * these machines and are read by nothing. */
static int junk(const char *name)
{
    return name[0] == '.' ||
           (name[0] == '_' && name[1] == '_') ||
           strcmp(name, ".DS_Store") == 0;
}

static void walk(const char *dir, PathList *l)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *e;
    char child[4096];
    while ((e = readdir(d)) != NULL) {
        if (junk(e->d_name))
            continue;
        snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk(child, l);
        else if (S_ISREG(st.st_mode) && interesting(e->d_name))
            list_push(l, child);
    }
    closedir(d);
}

/* --------------------------------------------------- the fingerprint itself */

typedef struct {
    uint64_t fingerprint;
    uint32_t file_count;
    int      under_test;          /* a witness fired */
    char     witness[8][512];     /* the first few files that carry one */
    int      witness_count;
} TreeAnswer;

static int fingerprint_tree(const char *root, TreeAnswer *out)
{
    PathList l = { NULL, 0, 0 };
    char sub[4096];

    snprintf(sub, sizeof(sub), "%s/src", root);   walk(sub, &l);
    snprintf(sub, sizeof(sub), "%s/tools", root); walk(sub, &l);
    snprintf(sub, sizeof(sub), "%s/Makefile", root);
    {
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISREG(st.st_mode))
            list_push(&l, sub);
    }

    if (l.count == 0) {
        fprintf(stderr, "buildplate: no input files under %s — is --root the "
                        "top of the tree?\n", root);
        return -1;
    }

    qsort(l.path, l.count, sizeof(char *), path_cmp);

    memset(out, 0, sizeof(*out));
    uint64_t h = FNV64_OFFSET;

    for (size_t i = 0; i < l.count; i++) {
        FILE *f = fopen(l.path[i], "rb");
        if (!f) {
            fprintf(stderr, "buildplate: cannot read %s\n", l.path[i]);
            return -1;
        }

        /* The path goes into the hash as well as the content: two files whose
         * contents were swapped are a different tree, and a file that was
         * renamed is a different tree too. */
        const char *rel = l.path[i];
        size_t rootlen = strlen(root);
        if (strncmp(rel, root, rootlen) == 0)
            rel += rootlen;
        fnv_bytes(&h, rel, strlen(rel) + 1);

        static char buf[1 << 16];
        size_t got;
        int saw_a = 0, saw_b = 0;
        /* The witness search runs over a sliding window so a phrase split
         * across two reads is still seen: the tail of each buffer is carried
         * into the next. The longest phrase is 18 bytes. */
        char carry[64];
        size_t carry_len = 0;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            fnv_bytes(&h, buf, got);

            char window[64 + (1 << 16)];
            memcpy(window, carry, carry_len);
            memcpy(window + carry_len, buf, got);
            size_t wlen = carry_len + got;
            window[wlen < sizeof(window) ? wlen : sizeof(window) - 1] = 0;
            /* memmem is not portable; a NUL-safe search is not needed here
             * because source files do not contain NUL bytes, and a binary that
             * does simply never matches. */
            if (!saw_a && strstr(window, WITNESS_A)) saw_a = 1;
            if (!saw_b && strstr(window, WITNESS_B)) saw_b = 1;

            carry_len = wlen < 32 ? wlen : 32;
            memcpy(carry, window + wlen - carry_len, carry_len);
        }
        fclose(f);
        fnv_bytes(&h, "", 1);

        if ((saw_a || saw_b) && strstr(l.path[i], "/tools/") == NULL) {
            out->under_test = 1;
            if (out->witness_count < 8)
                snprintf(out->witness[out->witness_count++], 512, "%s", l.path[i]);
        }

        free(l.path[i]);
    }
    free(l.path);

    out->fingerprint = h;
    out->file_count  = (uint32_t)l.count;
    return 0;
}

/* --------------------------------------------------------- the image itself */

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "buildplate: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "buildplate: %s is empty\n", path); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b);
        fprintf(stderr, "buildplate: cannot read all of %s\n", path);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* Exactly one, or nothing. A tag that appears twice means the image carries
 * two plates and a stamp would have to choose; a stamp that chooses is a stamp
 * that can be wrong, so this refuses instead. */
static long find_plate(const unsigned char *img, size_t len)
{
    static const char tag[BUILDPLATE_TAG_BYTES] = BUILDPLATE_TAG_INIT;
    long at = -1;
    int  seen = 0;
    if (len < sizeof(BuildPlate))
        return -1;
    for (size_t i = 0; i + sizeof(BuildPlate) <= len; i++) {
        if (memcmp(img + i, tag, BUILDPLATE_TAG_BYTES) == 0) {
            seen++;
            if (at < 0) at = (long)i;
        }
    }
    if (seen == 0) {
        fprintf(stderr, "buildplate: this image carries no plate tag — the "
                        "kernel was linked without core/buildplate in it\n");
        return -1;
    }
    if (seen > 1) {
        fprintf(stderr, "buildplate: the plate tag appears %d times in this "
                        "image; it must appear exactly once\n", seen);
        return -1;
    }
    return at;
}

static const char *verdict_word(uint32_t v)
{
    switch (v) {
    case BUILDPLATE_VERDICT_CLEAN:      return "clean";
    case BUILDPLATE_VERDICT_UNDER_TEST: return "UNDER TEST";
    default:                            return "unstamped";
    }
}

/* ---------------------------------------------------------------- the verbs */

static int cmd_stamp(const char *image, const char *root, const char *keys,
                     int allow_harness)
{
    if (!root || !keys) {
        fprintf(stderr, "buildplate stamp: --root and --keys are both required\n");
        return EXIT_USAGE;
    }
    if (strlen(keys) + 1 > BUILDPLATE_KEYS_MAX) {
        fprintf(stderr, "buildplate stamp: the key list is %zu bytes and the "
                        "plate holds %u — a truncated key list lies, so this "
                        "refuses instead\n", strlen(keys) + 1,
                (unsigned)BUILDPLATE_KEYS_MAX);
        return EXIT_USAGE;
    }

    TreeAnswer tree;
    if (fingerprint_tree(root, &tree) != 0)
        return EXIT_USAGE;

    if (tree.under_test && !allow_harness) {
        fprintf(stderr,
            "buildplate: REFUSING TO STAMP. This tree carries a test-harness "
            "injection:\n");
        for (int i = 0; i < tree.witness_count; i++)
            fprintf(stderr, "    %s\n", tree.witness[i]);
        fprintf(stderr,
            "An image packed from here would print lines no source in this "
            "tree contains, and\nnothing in the log could say so — that is "
            "exactly the run of 2026-09-22 that had to\nbe thrown away. Build "
            "it deliberately with HARNESS=on and the plate will say UNDER "
            "TEST,\nor put the tree back (tools/logcheck.sh does it on exit) "
            "and build again.\n");
        return EXIT_USAGE;
    }

    size_t len = 0;
    unsigned char *img = slurp(image, &len);
    if (!img)
        return EXIT_USAGE;

    long at = find_plate(img, len);
    if (at < 0) { free(img); return EXIT_USAGE; }

    BuildPlate p;
    memcpy(&p, img + at, sizeof(p));
    if (p.Magic != BUILDPLATE_MAGIC) {
        fprintf(stderr, "buildplate: the tag is there but the magic is 0x%08x, "
                        "not 0x%08x — this is not a plate\n",
                p.Magic, (unsigned)BUILDPLATE_MAGIC);
        free(img);
        return EXIT_USAGE;
    }

    p.Version         = BUILDPLATE_VERSION;
    p.TreeFingerprint = tree.fingerprint;
    p.StampedUnix     = (uint64_t)time(NULL);
    p.FileCount       = tree.file_count;
    p.Verdict         = tree.under_test ? BUILDPLATE_VERDICT_UNDER_TEST
                                        : BUILDPLATE_VERDICT_CLEAN;
    memset(p.Keys, 0, sizeof(p.Keys));
    memcpy(p.Keys, keys, strlen(keys));

    memcpy(img + at, &p, sizeof(p));

    FILE *f = fopen(image, "r+b");
    if (!f || fseek(f, at, SEEK_SET) != 0 ||
        fwrite(&p, 1, sizeof(p), f) != sizeof(p)) {
        fprintf(stderr, "buildplate: cannot write the plate into %s\n", image);
        if (f) fclose(f);
        free(img);
        return EXIT_USAGE;
    }
    fclose(f);
    free(img);

    printf("[buildplate] %s: tree %016llx, %u file(s), %s, keys: %s\n",
           image, (unsigned long long)p.TreeFingerprint, p.FileCount,
           verdict_word(p.Verdict), keys);
    return 0;
}

static int read_plate(const char *image, BuildPlate *out)
{
    size_t len = 0;
    unsigned char *img = slurp(image, &len);
    if (!img)
        return EXIT_USAGE;
    long at = find_plate(img, len);
    if (at < 0) { free(img); return EXIT_NOPLATE; }
    memcpy(out, img + at, sizeof(*out));
    free(img);
    if (out->Magic != BUILDPLATE_MAGIC || out->Version != BUILDPLATE_VERSION)
        return EXIT_NOPLATE;
    return 0;
}

static int cmd_verify(const char *image, const char *root)
{
    BuildPlate p;
    int rc = read_plate(image, &p);
    if (rc != 0) {
        fprintf(stderr, "buildplate: %s carries no readable plate, so nothing "
                        "can say which tree it was made from\n", image);
        return rc;
    }
    if (p.Verdict == BUILDPLATE_VERDICT_UNSTAMPED) {
        fprintf(stderr, "buildplate: %s was linked but never stamped\n", image);
        return EXIT_NOPLATE;
    }

    TreeAnswer tree;
    if (fingerprint_tree(root, &tree) != 0)
        return EXIT_USAGE;

    if (tree.fingerprint != p.TreeFingerprint) {
        fprintf(stderr,
            "buildplate: THIS IMAGE IS NOT MADE OF THIS TREE.\n"
            "    the image says  tree %016llx over %u file(s)\n"
            "    the tree is now tree %016llx over %u file(s)\n"
            "Something changed after the image was packed. The commonest cause "
            "is the test\nharness putting a mutated source back: the tree is "
            "then correct and the image is\nnot, and every conclusion drawn "
            "from booting it would be about code that is no\nlonger here. "
            "Build again.\n",
            (unsigned long long)p.TreeFingerprint, p.FileCount,
            (unsigned long long)tree.fingerprint, tree.file_count);
        return EXIT_MISMATCH;
    }

    if (p.Verdict == BUILDPLATE_VERDICT_UNDER_TEST) {
        fprintf(stderr,
            "buildplate: this image is stamped UNDER TEST — it was packed "
            "while the harness had\nthe tree mutated, and it matches that "
            "tree. A run from it is evidence about the\nharness, not about the "
            "kernel.\n");
        return EXIT_UNDERTEST;
    }

    printf("[buildplate] %s: tree %016llx over %u file(s), clean, keys: %.*s\n",
           image, (unsigned long long)p.TreeFingerprint, p.FileCount,
           (int)BUILDPLATE_KEYS_MAX, p.Keys);
    return 0;
}

static int cmd_say(const char *image)
{
    BuildPlate p;
    int rc = read_plate(image, &p);
    if (rc != 0) {
        printf("[buildplate] %s: no readable plate\n", image);
        return rc;
    }
    printf("[buildplate] %s: tree %016llx over %u file(s), %s, stamped %llu, "
           "keys: %.*s\n",
           image, (unsigned long long)p.TreeFingerprint, p.FileCount,
           verdict_word(p.Verdict), (unsigned long long)p.StampedUnix,
           (int)BUILDPLATE_KEYS_MAX, p.Keys);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: buildplate stamp  <image> --root <dir> --keys \"...\" "
            "[--allow-harness]\n"
            "       buildplate verify <image> --root <dir>\n"
            "       buildplate say    <image>\n");
        return EXIT_USAGE;
    }

    const char *verb  = argv[1];
    const char *image = argv[2];
    const char *root  = NULL;
    const char *keys  = NULL;
    int allow_harness = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)      root = argv[++i];
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc)  keys = argv[++i];
        else if (strcmp(argv[i], "--allow-harness") == 0)         allow_harness = 1;
        else {
            fprintf(stderr, "buildplate: unknown argument %s\n", argv[i]);
            return EXIT_USAGE;
        }
    }

    if (strcmp(verb, "stamp") == 0)  return cmd_stamp(image, root, keys, allow_harness);
    if (strcmp(verb, "verify") == 0) return cmd_verify(image, root ? root : ".");
    if (strcmp(verb, "say") == 0)    return cmd_say(image);

    fprintf(stderr, "buildplate: unknown verb %s\n", verb);
    return EXIT_USAGE;
}
