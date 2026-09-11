
#include "box/print.h"
#include "box/luggage.h"
#include "box/string.h"
#include "box/memtag.h"
#include "box/system.h"
#include "box/pku.h"
#include "box/convert.h"

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

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return -1;
        for (; *s; s++) {
            uint64_t d;
            if (*s >= '0' && *s <= '9')      d = (uint64_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') d = 10 + (uint64_t)(*s - 'a');
            else if (*s >= 'A' && *s <= 'F') d = 10 + (uint64_t)(*s - 'A');
            else return -1;
            v = (v << 4) | d;
        }
    } else {
        for (; *s; s++) {
            if (*s < '0' || *s > '9') return -1;
            v = v * 10 + (uint64_t)(*s - '0');
        }
    }
    *out = v;
    return 0;
}

static void print_tags_with_prefix(uint32_t rid, const char *prefix)
{
    char     buf[1024];
    uint32_t n = 0;
    if (mem_region_tags(rid, buf, sizeof(buf), &n) != 0) return;
    size_t plen = strlen(prefix);
    const char *cursor = buf;
    for (uint32_t i = 0; i < n; i++) {
        size_t tlen = strlen(cursor);
        if (tlen >= plen && memcmp(cursor, prefix, plen) == 0) {
            printf("    %s\n", cursor);
        }
        cursor += tlen + 1;
    }
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


static void do_pku_region(uint32_t rid, uint32_t pkey)
{
    if (pkey >= 16) {
        printf("%colorpkey must be 0..15%color\n", COLOR_RED, COLOR_DEFAULT);
        return;
    }
    int rc = pku_apply_region(rid, (uint8_t)pkey);
    if (rc == 0) {
        printf("applied pku:%u to region %u\n", pkey, rid);
    } else {
        printf("%colorpku-region failed (rc=%d)%color\n",
               COLOR_RED, rc, COLOR_DEFAULT);
    }
}


static void do_dump_cache(uint64_t phys)
{
    uint32_t rid = mem_region_from_phys(phys);
    if (rid == MEMTAG_INVALID_REGION_ID) {
        printf("phys %p: no region covers this address\n",
               (void *)(uintptr_t)phys);
        return;
    }
    printf("phys %p → region %u\n", (void *)(uintptr_t)phys, rid);
    printf("  cache tags:\n");
    print_tags_with_prefix(rid, "cache:");
}


static void do_dump_iommu(void)
{
    uint32_t ids[64];
    const char *req[2] = { "iommu:dma:mapped", 0 };
    int n = mem_query(req, 0, 0, ids, 64);
    if (n < 0) {
        printf("%coloriommu query failed (rc=%d)%color\n",
               COLOR_RED, n, COLOR_DEFAULT);
        return;
    }
    if (n == 0) {
        println("no IOMMU-tagged regions (IOMMU dormant or no DMA)");
        return;
    }
    printf("%u IOMMU-tagged region%s:\n", (unsigned)n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        mem_region_info_t info;
        if (mem_region_info(ids[i], &info) != 0) continue;
        printf("  region %u: phys=%p pages=%u\n",
               ids[i], (void *)(uintptr_t)info.base_phys,
               (unsigned)info.pages);
        print_tags_with_prefix(ids[i], "iommu:");
    }
}


static void do_dump_mce(void)
{
    uint32_t ids[64];
    const char *req[2] = { "mce:poisoned", 0 };
    int n = mem_query(req, 0, 0, ids, 64);
    if (n < 0) {
        printf("%colormce query failed (rc=%d)%color\n",
               COLOR_RED, n, COLOR_DEFAULT);
        return;
    }
    if (n == 0) {
        println("no poisoned pages (good)");
        return;
    }
    printf("%u poisoned region%s:\n", (unsigned)n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        mem_region_info_t info;
        if (mem_region_info(ids[i], &info) != 0) continue;
        printf("  region %u: phys=%p pages=%u\n",
               ids[i], (void *)(uintptr_t)info.base_phys,
               (unsigned)info.pages);
        print_tags_with_prefix(ids[i], "mce:");
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
    int argc = (int)luggage_word_count();

    if (argc < 2) { print_stats(); exit(0); return 0; }

    const char *cmd = luggage_word(1);

    if (strcmp(cmd, "stats") == 0) {
        print_stats();
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { println("usage: memtag info <region_id>"); exit(1); return 1; }
        uint32_t rid;
        if (parse_uint(luggage_word(2), &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_info(rid);
    } else if (strcmp(cmd, "tags") == 0) {
        if (argc < 3) { println("usage: memtag tags <region_id>"); exit(1); return 1; }
        uint32_t rid;
        if (parse_uint(luggage_word(2), &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_tags(rid);
    } else if (strcmp(cmd, "query") == 0) {
        if (argc < 3) { println("usage: memtag query <tag>"); exit(1); return 1; }
        print_query(luggage_word(2));
    } else if (strcmp(cmd, "guard") == 0) {
        if (argc < 3) { println("usage: memtag guard <tag> [on|off]"); exit(1); return 1; }
        int on = (argc >= 4 && strcmp(luggage_word(3), "off") == 0) ? 0 : 1;
        int rc = mem_set_guard(luggage_word(2), on);
        if (rc == 0) printf("guard %s: tag \"%s\"\n", on ? "ON " : "OFF", luggage_word(2));
        else         printf("%colorError: set_guard failed (rc=%d, need system tag)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "grant") == 0) {
        if (argc < 4) { println("usage: memtag grant <pid> <tag>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(luggage_word(2), &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        int rc = mem_cabin_grant(pid, luggage_word(3));
        if (rc == 0) printf("granted \"%s\" to cabin %u\n", luggage_word(3), pid);
        else         printf("%colorError: grant failed (rc=%d)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "revoke") == 0) {
        if (argc < 4) { println("usage: memtag revoke <pid> <tag>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(luggage_word(2), &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        int rc = mem_cabin_revoke(pid, luggage_word(3));
        if (rc == 0) printf("revoked \"%s\" from cabin %u\n", luggage_word(3), pid);
        else         printf("%colorError: revoke failed (rc=%d)%color\n",
                            COLOR_RED, rc, COLOR_DEFAULT);
    } else if (strcmp(cmd, "cabin") == 0) {
        if (argc < 3) { println("usage: memtag cabin <pid>"); exit(1); return 1; }
        uint32_t pid;
        if (parse_uint(luggage_word(2), &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        print_cabin(pid);
    } else if (strcmp(cmd, "check") == 0) {
        if (argc < 4) { println("usage: memtag check <pid> <region_id>"); exit(1); return 1; }
        uint32_t pid, rid;
        if (parse_uint(luggage_word(2), &pid) != 0) { println("invalid pid"); exit(1); return 1; }
        if (parse_uint(luggage_word(3), &rid) != 0) { println("invalid region_id"); exit(1); return 1; }
        print_check(pid, rid);
    } else if (strcmp(cmd, "pku-region") == 0) {
        if (argc < 4) { println("usage: memtag pku-region <region_id> <pkey>"); exit(1); return 1; }
        uint32_t rid, pkey;
        if (parse_uint(luggage_word(2), &rid)  != 0) { println("invalid region_id"); exit(1); return 1; }
        if (parse_uint(luggage_word(3), &pkey) != 0) { println("invalid pkey"); exit(1); return 1; }
        do_pku_region(rid, pkey);
    } else if (strcmp(cmd, "dump-cache") == 0) {
        if (argc < 3) { println("usage: memtag dump-cache <phys>"); exit(1); return 1; }
        uint64_t phys;
        if (parse_u64(luggage_word(2), &phys) != 0) { println("invalid phys (use 0x... or decimal)"); exit(1); return 1; }
        do_dump_cache(phys);
    } else if (strcmp(cmd, "dump-iommu") == 0) {
        do_dump_iommu();
    } else if (strcmp(cmd, "dump-mce") == 0) {
        do_dump_mce();
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
        println("  pku-region <rid> <k>  — stamp pku:k on region (Phase 2H+)");
        println("  dump-cache <phys>     — show cache:* tags at phys");
        println("  dump-iommu            — list iommu:domain:* regions");
        println("  dump-mce              — list mce:poisoned regions");
        exit(1);
        return 1;
    }

    exit(0);
    return 0;
}