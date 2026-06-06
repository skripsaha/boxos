/*
 * memtag — RAM region tagging introspection
 *
 *   memtag                 → global stats summary
 *   memtag stats           → full stats
 *   memtag query <tag>     → list region_ids tagged with <tag>
 *   memtag info <id>       → region descriptor
 *   memtag tags <id>       → all tags on a region
 */

#include "box/print.h"
#include "box/ipc.h"
#include "box/string.h"
#include "box/memtag.h"
#include "box/system.h"

static int parse_uint(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

static void print_stats(void)
{
    mem_stats_t s;
    if (mem_stats(&s) != 0) {
        printf("%colorError: mem_stats failed%color\n", COLOR_RED, COLOR_DEFAULT);
        return;
    }
    printf("%colorMemTag%color  tags=%u  active=%u/%u/%u\n",
           COLOR_CYAN, COLOR_DEFAULT,
           s.tag_count, s.region_active, s.region_slot_count, s.region_slot_cap);
    printf("  gen: reg=%u rgn=%u bmp=%u\n",
           (unsigned)s.registry_generation,
           (unsigned)s.region_generation,
           (unsigned)s.bitmap_generation);
    printf("  cache: %u hits / %u misses\n",
           (unsigned)s.cache_hits, (unsigned)s.cache_misses);
}

static void print_info(uint32_t rid)
{
    mem_region_info_t info;
    if (mem_region_info(rid, &info) != 0) {
        printf("%colorError: region %u not found%color\n",
               COLOR_RED, rid, COLOR_DEFAULT);
        return;
    }
    /* boxlib printf has no 'l' length modifier — render 64-bit values as
     * %p (hex pointer) for safety. */
    printf("region %u:\n", rid);
    printf("  base_phys = %p\n", (void *)(uintptr_t)info.base_phys);
    printf("  base_virt = %p\n", (void *)(uintptr_t)info.base_virt);
    printf("  pages     = %u (%u KiB)\n",
           (unsigned)info.pages, (unsigned)(info.pages * 4));
    printf("  flags     = 0x%x  tag_count=%u  gen=%u\n",
           info.flags, info.tag_count, info.generation);
}

static void print_tags(uint32_t rid)
{
    char     buf[1024];
    uint32_t n = 0;
    if (mem_region_tags(rid, buf, sizeof(buf), &n) != 0) {
        printf("%colorError: tags lookup failed%color\n", COLOR_RED, COLOR_DEFAULT);
        return;
    }
    printf("region %u has %u tag%s:\n", rid, n, n == 1 ? "" : "s");
    const char *cursor = buf;
    for (uint32_t i = 0; i < n; i++) {
        printf("  [%u] %s\n", i, cursor);
        cursor += strlen(cursor) + 1;
    }
}

static void print_query(const char *tag)
{
    uint32_t ids[64];
    const char *req[2] = { tag, 0 };
    int n = mem_query(req, 0, 0, ids, 64);
    if (n < 0) {
        printf("%colorError: query failed (rc=%d)%color\n",
               COLOR_RED, n, COLOR_DEFAULT);
        return;
    }
    printf("query \"%s\" → %d region%s\n", tag, n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        mem_region_info_t info;
        if (mem_region_info(ids[i], &info) == 0) {
            printf("  [%u] phys=%p pages=%u flags=0x%x\n",
                   ids[i], (void *)(uintptr_t)info.base_phys,
                   (unsigned)info.pages, info.flags);
        } else {
            printf("  [%u] (info unavailable)\n", ids[i]);
        }
    }
}

/* ─── Phase 2A subcommands ─────────────────────────────────────────── */

static void print_cabin(uint32_t pid)
{
    char     buf[2048];
    uint32_t n = 0;
    if (mem_cabin_tags(pid, buf, sizeof(buf), &n) != 0) {
        printf("%colorError: cabin tags lookup failed%color\n",
               COLOR_RED, COLOR_DEFAULT);
        return;
    }
    printf("cabin %u holds %u memtag capabilit%s:\n",
           pid, n, n == 1 ? "y" : "ies");
    const char *cursor = buf;
    for (uint32_t i = 0; i < n; i++) {
        printf("  [%u] %s\n", i, cursor);
        cursor += strlen(cursor) + 1;
    }
}

static void print_check(uint32_t pid, uint32_t rid)
{
    mem_check_t r;
    if (mem_check_access(pid, rid, &r) != 0) {
        printf("%colorError: check failed%color\n", COLOR_RED, COLOR_DEFAULT);
        return;
    }
    if (r.allowed) {
        printf("cabin %u → region %u: %colorALLOWED%color\n",
               pid, rid, COLOR_GREEN, COLOR_DEFAULT);
    } else {
        printf("cabin %u → region %u: %colorDENIED%color (missing guard tag_id=%u)\n",
               pid, rid, COLOR_RED, COLOR_DEFAULT, r.missing_tag_id);
    }
}

int main(void)
{
    int  argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) { print_stats(); exit(0); return 0; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "stats") == 0) {
        print_stats();
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { println("usage: memtag info <region_id>"); exit(1); return 1; }
        uint32_t rid;
        if (parse_uint(argv[2], &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_info(rid);
    } else if (strcmp(cmd, "tags") == 0) {
        if (argc < 3) { println("usage: memtag tags <region_id>"); exit(1); return 1; }
        uint32_t rid;
        if (parse_uint(argv[2], &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_tags(rid);
    } else if (strcmp(cmd, "query") == 0) {
        if (argc < 3) { println("usage: memtag query <tag>"); exit(1); return 1; }
        print_query(argv[2]);
    } else if (strcmp(cmd, "guard") == 0) {
        if (argc < 3) { println("usage: memtag guard <tag> [on|off]"); exit(1); return 1; }
        int on = (argc >= 4 && strcmp(argv[3], "off") == 0) ? 0 : 1;
        int rc = mem_set_guard(argv[2], on);
        if (rc == 0) printf("guard %s: tag \"%s\"\n", on ? "ON " : "OFF", argv[2]);
        else         printf("%colorError: set_guard failed (rc=%d, need system tag)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "grant") == 0) {
        if (argc < 4) { println("usage: memtag grant <pid> <tag>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(argv[2], &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        int rc = mem_cabin_grant(pid, argv[3]);
        if (rc == 0) printf("granted \"%s\" to cabin %u\n", argv[3], pid);
        else         printf("%colorError: grant failed (rc=%d)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "revoke") == 0) {
        if (argc < 4) { println("usage: memtag revoke <pid> <tag>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(argv[2], &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        int rc = mem_cabin_revoke(pid, argv[3]);
        if (rc == 0) printf("revoked \"%s\" from cabin %u\n", argv[3], pid);
        else         printf("%colorError: revoke failed (rc=%d)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "cabin") == 0) {
        if (argc < 3) { println("usage: memtag cabin <pid>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(argv[2], &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        print_cabin(pid);
    } else if (strcmp(cmd, "check") == 0) {
        if (argc < 4) { println("usage: memtag check <pid> <region_id>"); exit(1); return 1; }
        uint32_t pid, rid;
        if (parse_uint(argv[2], &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        if (parse_uint(argv[3], &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_check(pid, rid);
    } else {
        println("memtag commands:");
        println("  (no args)             — short summary");
        println("  stats                 — full stats");
        println("  query <tag>           — find regions");
        println("  info <region_id>      — region details");
        println("  tags <region_id>      — list region tags");
        println("  guard <tag> [on|off]  — enforce/release capability  [system]");
        println("  grant <pid> <tag>     — grant capability             [system]");
        println("  revoke <pid> <tag>    — revoke capability            [system]");
        println("  cabin <pid>           — list cabin's held caps");
        println("  check <pid> <rid>     — access check");
        exit(1);
        return 1;
    }

    exit(0);
    return 0;
}
