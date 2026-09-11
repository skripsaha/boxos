
#include "brook.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "memtag.h"
#include "atomics.h"
#include "error.h"
#include "kernel_config.h"
#include "acpi.h"
#include "amp.h"
#include "cpuid.h"
#include "kring.h"
#include "result.h"

#define BROOK_PAGE_CLASS_4K     12u
#define BROOK_PAGE_CLASS_2M     21u
#define BROOK_HUGE_THRESHOLD    VMM_LARGE_PAGE_2M_SIZE
#define BROOK_HUGE_SIZE         VMM_LARGE_PAGE_2M_SIZE
#define BROOK_HUGE_PAGES        VMM_LARGE_PAGE_2M_PAGES

#define BROOK_BUCKETS           256u
#define BROOK_BUCKET_MASK       (BROOK_BUCKETS - 1)

typedef struct BrookBucket {
    spinlock_t   lock;
    BrookObject *head;
} BrookBucket;

static BrookBucket g_brook_buckets[BROOK_BUCKETS];

static uint32_t brook_take_survivor_bell_locked(BrookHeader *kh, uint32_t departing_role);
static void     brook_ring_pid(uint32_t who);

static volatile uint64_t g_stat_objects;
static volatile uint64_t g_stat_claims;
static volatile uint64_t g_stat_pages;
static volatile uint64_t g_stat_releases;

static inline uint32_t brook_bucket_index(uint16_t tag_id)
{
    return ((uint32_t)tag_id ^ ((uint32_t)tag_id >> 4)) & BROOK_BUCKET_MASK;
}

void BrookInit(void)
{
    for (uint32_t i = 0; i < BROOK_BUCKETS; i++) {
        spinlock_init(&g_brook_buckets[i].lock);
        g_brook_buckets[i].head = NULL;
    }
    debug_printf("[Brook] init: %u buckets, 2 MiB huge-page threshold\n",
                 (unsigned)BROOK_BUCKETS);

    {
        unsigned line_min = (unsigned)g_cpu_caps.monitor_line_min;
        unsigned line_max = (unsigned)g_cpu_caps.monitor_line_max;
        if (line_min == 0) line_min = 64;
        if (line_max == 0) line_max = 64;

        const char *wait_path =
            g_cpu_caps.has_waitpkg ? "UMWAIT" : "PAUSE+yield";

        const acpi_numa_info_t *n = acpi_get_numa();
        unsigned domain_count = (n && n->present) ? n->domain_count : 0;

        if (line_min == 64 && line_max == 64 && domain_count <= 1) {
            debug_printf("[Brook] wait path=%s, monitor line=64 B, uniform memory\n",
                         wait_path);
        } else {
            debug_printf("[Brook] wait path=%s, monitor line=%u..%u B, NUMA domains=%u\n",
                         wait_path, line_min, line_max, domain_count);
        }

        if (line_max > 64) {
            debug_printf("[Brook] note: monitor line %u B > 64 B "
                         "(writer/reader cachelines share one line — wakes correct, "
                         "expect cross-peer wake amplification)\n",
                         line_max);
        }
    }
}

static uint16_t brook_resolve_tag(const char *tag, bool intern_if_missing)
{
    if (!tag || tag[0] == '\0') return TAGFS_INVALID_TAG_ID;
    return intern_if_missing ? tagfs_tag_intern(tag) : tagfs_tag_lookup(tag);
}

static uint32_t brook_preferred_domain(void)
{
    const acpi_numa_info_t *n = acpi_get_numa();
    if (!n || !n->present) return ACPI_NUMA_DOMAIN_UNKNOWN;

    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES) return ACPI_NUMA_DOMAIN_UNKNOWN;
    uint32_t lapic_id = g_amp.cores[core].lapic_id;

    for (uint16_t i = 0; i < n->cpu_count; i++) {
        if (!n->cpus[i].enabled) continue;
        if (n->cpus[i].apic_id == lapic_id) return n->cpus[i].domain;
    }
    return ACPI_NUMA_DOMAIN_UNKNOWN;
}

static void *brook_alloc_zero_in_domain(size_t pages, uint32_t domain)
{
    if (domain == ACPI_NUMA_DOMAIN_UNKNOWN) {
        return pmm_alloc_zero(pages);
    }
    void *p = pmm_alloc_in_domain(pages, domain);
    if (!p) {
        p = pmm_alloc_zero(pages);
        if (p) return p;
        return NULL;
    }
    memset((void *)vmm_phys_to_virt((uintptr_t)p), 0, pages * PMM_PAGE_SIZE);
    return p;
}

static error_t brook_alloc_slot_chunks(uint64_t total_size,
                                       uint32_t domain,
                                       uint16_t *out_class,
                                       uint64_t *out_chunk_size,
                                       uint32_t *out_chunk_count,
                                       uint64_t **out_chunks)
{
    if (total_size == 0) return ERR_INVALID_ARGUMENT;

    uint16_t cls;
    uint64_t cs;
    if (total_size >= BROOK_HUGE_THRESHOLD) {
        cls = BROOK_PAGE_CLASS_2M;
        cs  = BROOK_HUGE_SIZE;
    } else {
        cls = BROOK_PAGE_CLASS_4K;
        cs  = PMM_PAGE_SIZE;
    }
    uint32_t cc = (uint32_t)((total_size + cs - 1) / cs);

    uint64_t *chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
    if (!chunks) return ERR_NO_MEMORY;
    memset(chunks, 0, cc * sizeof(uint64_t));

    for (uint32_t i = 0; i < cc; i++) {
        size_t pages = (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1;
        void *p = brook_alloc_zero_in_domain(pages, domain);
        if (!p && cls == BROOK_PAGE_CLASS_2M) {
            for (uint32_t j = 0; j < i; j++) {
                if (chunks[j]) pmm_free((void *)chunks[j], BROOK_HUGE_PAGES);
            }
            kfree(chunks);

            cls = BROOK_PAGE_CLASS_4K;
            cs  = PMM_PAGE_SIZE;
            cc  = (uint32_t)((total_size + cs - 1) / cs);
            chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
            if (!chunks) return ERR_NO_MEMORY;
            memset(chunks, 0, cc * sizeof(uint64_t));
            i = (uint32_t)-1;
            continue;
        }
        if (!p) {
            for (uint32_t j = 0; j < i; j++) {
                if (chunks[j]) pmm_free((void *)chunks[j], 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }
        chunks[i] = (uint64_t)p;
    }

    *out_class       = cls;
    *out_chunk_size  = cs;
    *out_chunk_count = cc;
    *out_chunks      = chunks;
    return OK;
}

static void brook_memtag_attach_all(struct process_t *proc,
                                     uint64_t hdr_phys,
                                     uint64_t va_header,
                                     uint64_t *slot_chunks,
                                     uint32_t slot_cc,
                                     uint64_t va_slots,
                                     uint64_t slot_cs)
{
    const uint64_t att_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;

    if (hdr_phys != 0) {
        uint32_t hdr_rid = MemRegionFromPhys((uintptr_t)hdr_phys);
        if (hdr_rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionAttachCabin(hdr_rid, (void *)proc->cabin->vmm,
                                  va_header, 1,
                                  MEMTAG_ATTACH_CLASS_4K, att_flags);
        }
    }

    uint8_t  pc = (slot_cs == BROOK_HUGE_SIZE)
                  ? MEMTAG_ATTACH_CLASS_2M : MEMTAG_ATTACH_CLASS_4K;
    uint32_t pages_per_chunk = (slot_cs == BROOK_HUGE_SIZE)
                               ? (uint32_t)BROOK_HUGE_PAGES : 1u;
    for (uint32_t i = 0; i < slot_cc; i++) {
        if (slot_chunks[i] == 0) continue;
        uint64_t att_va = va_slots + (uint64_t)i * slot_cs;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)slot_chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionAttachCabin(rid, (void *)proc->cabin->vmm, att_va,
                                  pages_per_chunk, pc, att_flags);
        }
    }
}

static void brook_memtag_detach_all(struct process_t *proc,
                                     uint64_t hdr_phys,
                                     uint64_t va_header,
                                     uint64_t *slot_chunks,
                                     uint32_t slot_cc,
                                     uint64_t va_slots,
                                     uint64_t slot_cs)
{
    if (hdr_phys != 0) {
        uint32_t hdr_rid = MemRegionFromPhys((uintptr_t)hdr_phys);
        if (hdr_rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(hdr_rid, (void *)proc->cabin->vmm, va_header);
        }
    }
    for (uint32_t i = 0; i < slot_cc; i++) {
        if (slot_chunks[i] == 0) continue;
        uint64_t att_va = va_slots + (uint64_t)i * slot_cs;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)slot_chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(rid, (void *)proc->cabin->vmm, att_va);
        }
    }
}

static error_t brook_map_slots_into_cabin(struct process_t *proc,
                                          uint64_t user_va_base,
                                          uint64_t *chunks,
                                          uint32_t chunk_count,
                                          uint64_t chunk_size)
{
    if (!proc || !proc->cabin) return ERR_INVALID_ARGUMENT;

    const uint64_t vmm_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t va = user_va_base + (uint64_t)i * chunk_size;
        uint64_t pa = chunks[i];

        bool ok;
        if (chunk_size == BROOK_HUGE_SIZE) {
            ok = vmm_map_huge_2m(proc->cabin->vmm, va, pa, vmm_flags);
        } else {
            vmm_map_result_t r = vmm_map_page(proc->cabin->vmm, va, pa, vmm_flags);
            ok = r.success;
        }

        if (!ok) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t uva = user_va_base + (uint64_t)j * chunk_size;
                if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(proc->cabin->vmm, uva);
                else                               vmm_unmap_page(proc->cabin->vmm, uva);
            }
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

static void brook_unmap_slots_from_cabin(struct process_t *proc,
                                         uint64_t user_va_base,
                                         uint32_t chunk_count,
                                         uint64_t chunk_size)
{
    if (!proc || !proc->cabin) return;
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t uva = user_va_base + (uint64_t)i * chunk_size;
        if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(proc->cabin->vmm, uva);
        else                               vmm_unmap_page(proc->cabin->vmm, uva);
    }
}

static bool brook_map_header_into_cabin(struct process_t *proc,
                                        uint64_t user_va_header,
                                        uint64_t header_phys)
{
    if (!proc || !proc->cabin) return false;
    const uint64_t vmm_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;
    vmm_map_result_t r = vmm_map_page(proc->cabin->vmm, user_va_header,
                                      header_phys, vmm_flags);
    return r.success;
}

static void brook_unmap_header_from_cabin(struct process_t *proc,
                                          uint64_t user_va_header)
{
    if (!proc || !proc->cabin) return;
    vmm_unmap_page(proc->cabin->vmm, user_va_header);
}

static void brook_free_backing(BrookObject *brook)
{
    if (!brook) return;
    if (brook->slot_chunks) {
        size_t pages_per_chunk = (brook->slot_chunk_size == BROOK_HUGE_SIZE)
                                 ? BROOK_HUGE_PAGES : 1;
        for (uint32_t i = 0; i < brook->slot_chunk_count; i++) {
            if (brook->slot_chunks[i] == 0) continue;
            pmm_free((void *)brook->slot_chunks[i], pages_per_chunk);
        }
        kfree(brook->slot_chunks);
        brook->slot_chunks = NULL;
    }
    if (brook->header_phys) {
        pmm_free((void *)brook->header_phys, 1);
        brook->header_phys = 0;
    }
}

static error_t brook_reserve_user_va(struct process_t *proc,
                                     uint64_t slot_total_size,
                                     uint64_t slot_chunk_size,
                                     uint64_t *out_va_header,
                                     uint64_t *out_va_slots)
{
    if (!proc) return ERR_INVALID_ARGUMENT;
    if (slot_chunk_size < PMM_PAGE_SIZE) slot_chunk_size = PMM_PAGE_SIZE;

    spin_lock(&proc->cabin->brook_lock);

    uint64_t cur = (proc->cabin->brook_va_next + (PMM_PAGE_SIZE - 1))
                   & ~(PMM_PAGE_SIZE - 1);
    uint64_t va_header = cur;

    uint64_t va_slots = (va_header + PMM_PAGE_SIZE + slot_chunk_size - 1)
                        & ~(slot_chunk_size - 1);

    if (va_slots < va_header ||
        va_slots >= CABIN_BROOK_END ||
        slot_total_size > CABIN_BROOK_END - va_slots ||
        va_slots + slot_total_size < va_slots) {
        spin_unlock(&proc->cabin->brook_lock);
        return ERR_NO_MEMORY;
    }

    proc->cabin->brook_va_next = va_slots + slot_total_size;

    spin_unlock(&proc->cabin->brook_lock);

    *out_va_header = va_header;
    *out_va_slots  = va_slots;
    return OK;
}

static bool brook_link_claim(struct process_t *proc, BrookClaim *claim)
{
    spin_lock(&proc->cabin->brook_lock);
    if (proc->destroying) {
        spin_unlock(&proc->cabin->brook_lock);
        return false;
    }
    claim->proc_next = (BrookClaim *)proc->cabin->brook_claims_head;
    proc->cabin->brook_claims_head = claim;
    spin_unlock(&proc->cabin->brook_lock);
    return true;
}

static BrookClaim *brook_unlink_claim_by_va(struct process_t *proc,
                                            uint64_t user_va_header)
{
    spin_lock(&proc->cabin->brook_lock);
    BrookClaim **p = (BrookClaim **)&proc->cabin->brook_claims_head;
    while (*p) {
        if ((*p)->user_va_header == user_va_header) {
            BrookClaim *hit = *p;
            *p = hit->proc_next;
            spin_unlock(&proc->cabin->brook_lock);
            return hit;
        }
        p = &(*p)->proc_next;
    }
    spin_unlock(&proc->cabin->brook_lock);
    return NULL;
}

static BrookObject *brook_bucket_find_locked(BrookBucket *b, uint16_t tag_id)
{
    for (BrookObject *o = b->head; o; o = o->bucket_next) {
        if (o->tag_id == tag_id) return o;
    }
    return NULL;
}

static void brook_bucket_unlink_locked(BrookBucket *b, BrookObject *obj)
{
    BrookObject **p = &b->head;
    while (*p && *p != obj) p = &(*p)->bucket_next;
    if (*p) *p = obj->bucket_next;
    obj->bucket_next = NULL;
}

static inline uint64_t brook_total_pages(const BrookObject *brook)
{
    uint64_t pages_per_chunk = (brook->slot_chunk_size == BROOK_HUGE_SIZE)
                               ? BROOK_HUGE_PAGES : 1;
    uint64_t slot_pages = (uint64_t)brook->slot_chunk_count * pages_per_chunk;
    return slot_pages + 1;
}

static void brook_drop_ref_locked(BrookBucket *b, BrookObject *brook)
{
    brook->ref_count--;
    if (brook->ref_count == 0) {
        brook_bucket_unlink_locked(b, brook);
        spin_unlock(&b->lock);

        atomic_fetch_sub_u64(&g_stat_objects, 1);
        atomic_fetch_sub_u64(&g_stat_pages, brook_total_pages(brook));
        brook_free_backing(brook);
        kfree(brook);
        return;
    }
    spin_unlock(&b->lock);
}

static inline BrookHeader *brook_kernel_header(const BrookObject *brook)
{
    return (BrookHeader *)vmm_phys_to_virt(brook->header_phys);
}

static void brook_header_init(BrookHeader *hdr,
                              uint32_t frame_size,
                              uint32_t frame_count)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->head                 = 0;
    hdr->tail                 = 0;
    hdr->frame_size           = frame_size;
    hdr->frame_count          = frame_count;
    hdr->magic                = BROOK_HEADER_MAGIC;
    hdr->writer_alive         = 0;
    hdr->reader_alive         = 0;
    hdr->writer_ever_attached = 0;
    hdr->reader_ever_attached = 0;
}

static inline bool brook_is_pow2(uint32_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

error_t BrookOpenInternal(struct process_t *proc,
                          const char *tag,
                          uint32_t frame_size,
                          uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_user_va_header,
                          uint64_t *out_user_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count)
{
    if (!proc || !proc->cabin || !tag || !out_user_va_header ||
        !out_user_va_slots || !out_frame_size || !out_frame_count)
        return ERR_INVALID_ARGUMENT;

    if (proc->destroying) return ERR_INVALID_STATE;

    if (flags & ~BROOK_FLAGS_MASK)                   return ERR_INVALID_ARGUMENT;
    uint32_t role = flags & (BROOK_WRITER | BROOK_READER);
    if (role != BROOK_WRITER && role != BROOK_READER) return ERR_INVALID_ARGUMENT;

    bool wants_create = (flags & BROOK_CREATE) != 0;
    if (wants_create) {
        if (frame_size  < BROOK_FRAME_SIZE_MIN  || frame_size  > BROOK_FRAME_SIZE_MAX)
            return ERR_INVALID_ARGUMENT;
        if (frame_count < BROOK_FRAME_COUNT_MIN || frame_count > BROOK_FRAME_COUNT_MAX)
            return ERR_INVALID_ARGUMENT;
        if (!brook_is_pow2(frame_count))             return ERR_INVALID_ARGUMENT;
        if ((uint64_t)frame_size * frame_count > BROOK_MAX_TOTAL_SIZE)
            return ERR_INVALID_ARGUMENT;
    }

    uint16_t tag_id = brook_resolve_tag(tag, wants_create);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_TAG_NOT_FOUND;

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(tag_id)];

    spin_lock(&b->lock);
    BrookObject *brook = brook_bucket_find_locked(b, tag_id);

    if (!brook) {
        if (!wants_create) {
            spin_unlock(&b->lock);
            return ERR_TAG_NOT_FOUND;
        }
        spin_unlock(&b->lock);

        uint64_t slot_total = (uint64_t)frame_size * frame_count;
        uint32_t domain = brook_preferred_domain();
        uint16_t  cls;
        uint64_t  cs;
        uint32_t  cc;
        uint64_t *chunks;
        error_t rc = brook_alloc_slot_chunks(slot_total, domain,
                                             &cls, &cs, &cc, &chunks);
        if (rc != OK) return rc;

        void *hdr_phys = brook_alloc_zero_in_domain(1, domain);
        if (!hdr_phys) {
            for (uint32_t i = 0; i < cc; i++) {
                if (chunks[i]) pmm_free((void *)chunks[i],
                                        (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }

        BrookObject *fresh = (BrookObject *)kmalloc(sizeof(BrookObject));
        if (!fresh) {
            pmm_free(hdr_phys, 1);
            for (uint32_t i = 0; i < cc; i++) {
                if (chunks[i]) pmm_free((void *)chunks[i],
                                        (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }
        memset(fresh, 0, sizeof(*fresh));
        fresh->tag_id           = tag_id;
        fresh->slot_page_class  = cls;
        fresh->flags            = flags & ~(BROOK_WRITER | BROOK_READER);
        fresh->frame_size       = frame_size;
        fresh->frame_count      = frame_count;
        fresh->header_phys      = (uint64_t)hdr_phys;
        fresh->slot_total_size  = (uint64_t)cc * cs;
        fresh->slot_chunk_size  = cs;
        fresh->slot_chunk_count = cc;
        fresh->slot_chunks      = chunks;
        fresh->writer_pid       = 0;
        fresh->reader_pid       = 0;
        fresh->ref_count        = 0;
        fresh->create_pid       = proc->pid;
        fresh->create_tsc       = rdtsc();

        brook_header_init(brook_kernel_header(fresh), frame_size, frame_count);

        spin_lock(&b->lock);
        BrookObject *winner = brook_bucket_find_locked(b, tag_id);
        if (winner) {
            spin_unlock(&b->lock);
            brook_free_backing(fresh);
            kfree(fresh);
            spin_lock(&b->lock);
            brook = brook_bucket_find_locked(b, tag_id);
            if (!brook) {
                spin_unlock(&b->lock);
                return ERR_TAG_NOT_FOUND;
            }
        } else {
            fresh->bucket_next = b->head;
            b->head            = fresh;
            brook              = fresh;
            atomic_fetch_add_u64(&g_stat_objects, 1);
            atomic_fetch_add_u64(&g_stat_pages, brook_total_pages(fresh));

            char tag_buf[128];
            if (tagfs_tag_text(tag_id, tag_buf, sizeof(tag_buf))) {
                MemTagApplyByPhys(fresh->header_phys, 1, tag_buf);
                MemTagApplyByPhys(fresh->header_phys, 1, "purpose:brook");
                MemTagApplyByPhys(fresh->header_phys, 1, "purpose:brook-header");
                size_t pages_per_chunk =
                    (cs == BROOK_HUGE_SIZE) ? BROOK_HUGE_PAGES : 1;
                for (uint32_t ci = 0; ci < cc; ci++) {
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, tag_buf);
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:brook");
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:stream");
                }
            }
        }
    }


    if (frame_size != 0 && brook->frame_size != frame_size) {
        spin_unlock(&b->lock);
        return ERR_ALREADY_EXISTS;
    }
    if (frame_count != 0 && brook->frame_count != frame_count) {
        spin_unlock(&b->lock);
        return ERR_ALREADY_EXISTS;
    }

    if (role == BROOK_WRITER) {
        if (brook->writer_pid != 0) {
            spin_unlock(&b->lock);
            return ERR_BUSY;
        }
        brook->writer_pid = proc->pid;
    } else {
        if (brook->reader_pid != 0) {
            spin_unlock(&b->lock);
            return ERR_BUSY;
        }
        brook->reader_pid = proc->pid;
    }
    brook->ref_count++;

    uint32_t out_fs = brook->frame_size;
    uint32_t out_fc = brook->frame_count;
    uint64_t hdr_phys = brook->header_phys;
    uint64_t slot_total = brook->slot_total_size;
    uint64_t slot_cs    = brook->slot_chunk_size;
    uint32_t slot_cc    = brook->slot_chunk_count;
    spin_unlock(&b->lock);

    uint64_t va_header = 0, va_slots = 0;
    error_t rc = brook_reserve_user_va(proc, slot_total, slot_cs,
                                       &va_header, &va_slots);
    if (rc != OK) goto rollback_ref;

    if (!MemTagEnforcePhys(proc->pid, (uintptr_t)hdr_phys)) {
        rc = ERR_PERMISSION_DENIED;
        goto rollback_ref;
    }

    if (!brook_map_header_into_cabin(proc, va_header, hdr_phys)) {
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }

    rc = brook_map_slots_into_cabin(proc, va_slots, brook->slot_chunks,
                                    slot_cc, slot_cs);
    if (rc != OK) {
        brook_unmap_header_from_cabin(proc, va_header);
        goto rollback_ref;
    }

    brook_memtag_attach_all(proc, hdr_phys, va_header,
                             brook->slot_chunks, slot_cc, va_slots, slot_cs);

    BrookClaim *claim = (BrookClaim *)kmalloc(sizeof(BrookClaim));
    if (!claim) {
        brook_memtag_detach_all(proc, hdr_phys, va_header,
                                 brook->slot_chunks, slot_cc, va_slots, slot_cs);
        brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
        brook_unmap_header_from_cabin(proc, va_header);
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }
    memset(claim, 0, sizeof(*claim));
    claim->brook           = brook;
    claim->proc            = proc;
    claim->user_va_header  = va_header;
    claim->user_va_slots   = va_slots;
    claim->role            = role;
    claim->flags           = flags;

    BrookHeader *kh = brook_kernel_header(brook);
    volatile uint32_t *alive_ptr;
    volatile uint32_t *ever_ptr;
    if (role == BROOK_WRITER) {
        alive_ptr = &kh->writer_alive;
        ever_ptr  = &kh->writer_ever_attached;
    } else {
        alive_ptr = &kh->reader_alive;
        ever_ptr  = &kh->reader_ever_attached;
    }
    {
        uint32_t expected = 0u;
        if (!__atomic_compare_exchange_n(alive_ptr, &expected, 1u,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            brook_memtag_detach_all(proc, hdr_phys, va_header,
                                     brook->slot_chunks, slot_cc, va_slots, slot_cs);
            brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
            brook_unmap_header_from_cabin(proc, va_header);
            kfree(claim);
            rc = (expected == BROOK_ALIVE_FROZEN) ? ERR_INVALID_STATE
                                                  : ERR_BUSY;
            goto rollback_ref;
        }
        __atomic_store_n(ever_ptr, 1u, __ATOMIC_RELEASE);
    }

    if (!brook_link_claim(proc, claim)) {
        uint32_t one = 1u;
        __atomic_compare_exchange_n(alive_ptr, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        brook_memtag_detach_all(proc, hdr_phys, va_header,
                                 brook->slot_chunks, slot_cc, va_slots, slot_cs);
        brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
        brook_unmap_header_from_cabin(proc, va_header);
        kfree(claim);
        rc = ERR_INVALID_STATE;
        goto rollback_ref;
    }

    atomic_fetch_add_u64(&g_stat_claims, 1);

    *out_user_va_header = va_header;
    *out_user_va_slots  = va_slots;
    *out_frame_size     = out_fs;
    *out_frame_count    = out_fc;
    return OK;

rollback_ref:
    spin_lock(&b->lock);
    if (role == BROOK_WRITER && brook->writer_pid == proc->pid) {
        brook->writer_pid = 0;
    } else if (role == BROOK_READER && brook->reader_pid == proc->pid) {
        brook->reader_pid = 0;
    }
    brook_drop_ref_locked(b, brook);
    return rc;
}

error_t BrookReleaseInternal(struct process_t *proc, uint64_t user_va_header)
{
    if (!proc || user_va_header == 0) return ERR_INVALID_ARGUMENT;

    BrookClaim *claim = brook_unlink_claim_by_va(proc, user_va_header);
    if (!claim) return ERR_TAG_NOT_FOUND;

    BrookObject *brook = claim->brook;
    if (!brook) { kfree(claim); return ERR_INVALID_STATE; }

    brook_memtag_detach_all(proc, brook->header_phys, claim->user_va_header,
                             brook->slot_chunks, brook->slot_chunk_count,
                             claim->user_va_slots, brook->slot_chunk_size);

    brook_unmap_slots_from_cabin(proc, claim->user_va_slots,
                                 brook->slot_chunk_count,
                                 brook->slot_chunk_size);
    brook_unmap_header_from_cabin(proc, claim->user_va_header);

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];

    spin_lock(&b->lock);

    BrookHeader *kh = brook_kernel_header(brook);
    if (claim->role == BROOK_WRITER) {
        uint32_t one = 1u;
        __atomic_compare_exchange_n(&kh->writer_alive, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        if (brook->writer_pid == proc->pid) brook->writer_pid = 0;
    } else {
        uint32_t one = 1u;
        __atomic_compare_exchange_n(&kh->reader_alive, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        if (brook->reader_pid == proc->pid) brook->reader_pid = 0;
    }
    uint32_t ring_pid = brook_take_survivor_bell_locked(kh, claim->role);

    brook_drop_ref_locked(b, brook);

    brook_ring_pid(ring_pid);

    kfree(claim);
    if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
    atomic_fetch_add_u64(&g_stat_releases, 1);
    return OK;
}

void BrookCleanupProcess(struct process_t *proc)
{
    if (!proc || !proc->cabin) return;

    BrookClaim *to_free = NULL;
    spin_lock(&proc->cabin->brook_lock);
    BrookClaim **pp = (BrookClaim **)&proc->cabin->brook_claims_head;
    while (*pp) {
        BrookClaim *c = *pp;
        if (c->proc == proc) {
            *pp = c->proc_next;
            c->proc_next = to_free;
            to_free = c;
        } else {
            pp = &c->proc_next;
        }
    }
    spin_unlock(&proc->cabin->brook_lock);

    while (to_free) {
        BrookClaim *next = to_free->proc_next;
        BrookObject *brook = to_free->brook;

        if (brook) {
            brook_memtag_detach_all(proc, brook->header_phys, to_free->user_va_header,
                                     brook->slot_chunks, brook->slot_chunk_count,
                                     to_free->user_va_slots, brook->slot_chunk_size);
            brook_unmap_slots_from_cabin(proc, to_free->user_va_slots,
                                         brook->slot_chunk_count,
                                         brook->slot_chunk_size);
            brook_unmap_header_from_cabin(proc, to_free->user_va_header);

            BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
            spin_lock(&b->lock);

            BrookHeader *kh = brook_kernel_header(brook);
            if (to_free->role == BROOK_WRITER) {
                uint32_t one = 1u;
                __atomic_compare_exchange_n(&kh->writer_alive, &one, 0u,
                                            false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
                if (brook->writer_pid == proc->pid) brook->writer_pid = 0;
            } else {
                uint32_t one = 1u;
                __atomic_compare_exchange_n(&kh->reader_alive, &one, 0u,
                                            false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
                if (brook->reader_pid == proc->pid) brook->reader_pid = 0;
            }
            uint32_t ring_pid = brook_take_survivor_bell_locked(kh, to_free->role);

            brook_drop_ref_locked(b, brook);

            brook_ring_pid(ring_pid);

            if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
            atomic_fetch_add_u64(&g_stat_releases, 1);
        }

        kfree(to_free);
        to_free = next;
    }
}

bool BrookBellUnrung(uint32_t strand_pid, uint16_t *out_tag_id,
                     uint64_t *out_head, uint64_t *out_tail)
{
    if (strand_pid == 0) return false;

    bool     hung   = false;
    bool     served = false;
    uint16_t tag    = 0;
    uint64_t shead = 0, stail = 0;

    for (uint32_t i = 0; i < BROOK_BUCKETS; i++) {
        BrookBucket *b = &g_brook_buckets[i];
        spin_lock(&b->lock);
        for (BrookObject *o = b->head; o; o = o->bucket_next) {
            if (!o->header_phys) continue;
            bool is_reader = (o->reader_pid == strand_pid);
            bool is_writer = (o->writer_pid == strand_pid);
            if (!is_reader && !is_writer) continue;

            const BrookHeader *h = brook_kernel_header(o);
            if (!h) continue;

            uint32_t rb = __atomic_load_n(&h->reader_bell, __ATOMIC_ACQUIRE);
            uint32_t wb = __atomic_load_n(&h->writer_bell, __ATOMIC_ACQUIRE);
            if (is_reader && rb != 0) hung = true;
            if (is_writer && wb != 0) hung = true;

            uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
            uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
            bool     can  = is_reader ? (head != tail)
                                      : ((tail - head) < o->frame_count);
            if (!served && can) {
                served = true;
                tag    = o->tag_id;
                shead  = head;
                stail  = tail;
            }
        }
        spin_unlock(&b->lock);
        if (hung && served) break;
    }

    if (!hung || !served) return false;
    if (out_tag_id) *out_tag_id = tag;
    if (out_head)   *out_head   = shead;
    if (out_tail)   *out_tail   = stail;
    return true;
}


static uint32_t brook_take_survivor_bell_locked(BrookHeader *kh, uint32_t departing_role)
{
    if (!kh) return 0;
    volatile uint32_t *bell = (departing_role == BROOK_WRITER) ? &kh->reader_bell
                                                               : &kh->writer_bell;
    uint32_t who = __atomic_load_n(bell, __ATOMIC_ACQUIRE);
    if (who == 0) return 0;
    if (who & BROOK_BELL_RUNG) return 0;
    if (!__atomic_compare_exchange_n(bell, &who, who | BROOK_BELL_RUNG, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0;
    return BROOK_BELL_PID(who);
}

static void brook_ring_pid(uint32_t who)
{
    if (who == 0) return;
    process_t *target = process_find_ref(who);
    if (!target) return;

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code = ERR_WOULD_BLOCK;
    (void)KResultPush(target, &r);
    process_ref_dec(target);
}

void BrookStatsSnapshot(uint64_t out[4])
{
    out[0] = atomic_load_u64(&g_stat_objects);
    out[1] = atomic_load_u64(&g_stat_claims);
    out[2] = atomic_load_u64(&g_stat_pages);
    out[3] = atomic_load_u64(&g_stat_releases);
}