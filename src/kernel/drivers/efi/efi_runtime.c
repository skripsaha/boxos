
#include "efi.h"
#include "efi_runtime_internal.h"
#include "boot_info.h"
#include "e820.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "io.h"
#include "cpu_calibrate.h"
#include "atomics.h"
#include "crypto.h"

#define EFI_PAGE_SIZE      4096ULL
#define EFI_PAGE_SHIFT     12


static EfiRuntimeServices *g_rt           = NULL;
static bool                g_rt_available = false;
static spinlock_t          g_rt_lock;
static bool                g_rt_lock_init = false;

static uint32_t g_rt_svam_descriptors    = 0;

static uint32_t g_rt_runtime_descriptors = 0;
static uint64_t g_rt_runtime_pages_total = 0;
static uint32_t g_rt_fw_revision         = 0;


static inline uint64_t efi_pages_to_bytes(uint64_t pages)
{
    return pages << EFI_PAGE_SHIFT;
}

static uint64_t efi_rt_cache_flags(uint64_t attribute)
{
    if (attribute & EFI_MEMORY_WB) return 0;
    if (attribute & EFI_MEMORY_WT) return VMM_FLAG_WRITE_THROUGH;
    if (attribute & EFI_MEMORY_WC) return vmm_wc_pte_flags();
    return VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;
}

static uint64_t efi_rt_map_flags(const EfiMemoryDescriptor *d)
{
    if (d->type == EFI_MEMORY_MAPPED_IO_PORT_SPACE) return 0;

    uint64_t flags = VMM_FLAGS_KERNEL_RW | efi_rt_cache_flags(d->attribute);

    if (d->type != EFI_RUNTIME_SERVICES_CODE || (d->attribute & EFI_MEMORY_XP))
        flags |= VMM_FLAG_NO_EXECUTE;

    return flags;
}

static bool efi_table_header_valid(const EfiRuntimeServices *rt)
{
    if (!rt) return false;
    const EfiTableHeader *h = &rt->hdr;
    if (h->header_size < sizeof(EfiTableHeader)) return false;
    if (h->header_size > 4096) return false;

    static uint8_t scratch[4096];
    uint32_t len = h->header_size;
    memcpy(scratch, rt, len);
    *(uint32_t *)(scratch + 16) = 0;

    uint32_t computed = KCrc32(scratch, len);
    return computed == h->crc32;
}

static bool efi_map_rt_descriptor(EfiMemoryDescriptor *d, vmm_context_t *kctx)
{
    if (!(d->attribute & EFI_MEMORY_RUNTIME)) return true;

    uint64_t flags = efi_rt_map_flags(d);

    uintptr_t phys = (uintptr_t)d->physical_start;
    uintptr_t virt = EFI_RT_VA_BASE + phys;
    size_t    pages = (size_t)d->number_of_pages;

    d->virtual_start = virt;
    if (flags == 0) return true;

    vmm_map_result_t ri = vmm_map_pages(kctx, phys, phys, pages, flags);
    if (!ri.success) {
        kprintf("[EFI] identity mapping failed for RT desc type=%u "
                "phys=0x%lx pages=%lu: %s\n",
                d->type, (unsigned long)phys, (unsigned long)pages,
                ri.error_msg ? ri.error_msg : "(no msg)");
        return false;
    }

    vmm_map_result_t rv = vmm_map_pages(kctx, virt, phys, pages, flags);
    if (!rv.success) {
        kprintf("[EFI] runtime-VA mapping failed for RT desc type=%u "
                "phys=0x%lx virt=0x%lx pages=%lu: %s\n",
                d->type, (unsigned long)phys, (unsigned long)virt,
                (unsigned long)pages,
                rv.error_msg ? rv.error_msg : "(no msg)");
        return false;
    }

    g_rt_runtime_descriptors++;
    g_rt_runtime_pages_total += pages;
    return true;
}

static void efi_boot_services_alias(uint8_t *map, uint32_t map_size,
                                    uint32_t desc_size, vmm_context_t *kctx,
                                    bool install)
{
    uint32_t regions = 0;
    for (uint8_t *p = map; p + desc_size <= map + map_size; p += desc_size) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        if (d->type != EFI_BOOT_SERVICES_CODE &&
            d->type != EFI_BOOT_SERVICES_DATA) continue;
        if (d->number_of_pages == 0) continue;

        uintptr_t phys  = (uintptr_t)d->physical_start;
        size_t    pages = (size_t)d->number_of_pages;

        if (install) {
            vmm_map_result_t r = vmm_map_pages(kctx, phys, phys, pages,
                                               VMM_FLAGS_KERNEL_RW |
                                               VMM_FLAG_NO_EXECUTE);
            if (!r.success) {
                kprintf("[EFI] could not alias boot-services region "
                        "0x%lx (%lu page(s)): %s\n",
                        (unsigned long)phys, (unsigned long)pages,
                        r.error_msg ? r.error_msg : "(no msg)");
                continue;
            }
        } else {
            vmm_unmap_pages(kctx, phys, pages);
        }
        regions++;
    }

    debug_printf("[EFI] %s %u boot-services region(s) for the SVAM call\n",
                 install ? "aliased" : "released", regions);
}

static EfiMemoryDescriptor *efi_find_descriptor(uint8_t *map, uint32_t map_size,
                                                uint32_t desc_size,
                                                uint64_t phys)
{
    uint8_t *p   = map;
    uint8_t *end = map + map_size;
    while (p + desc_size <= end) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        uint64_t span = efi_pages_to_bytes(d->number_of_pages);
        if (phys >= d->physical_start && phys < d->physical_start + span)
            return d;
        p += desc_size;
    }
    return NULL;
}


const EfiGuid EFI_MEMORY_ATTRIBUTES_TABLE_GUID = {
    0xdcfa911d, 0x26eb, 0x469f,
    {0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20}
};

#define EFI_MEMORY_ATTRIBUTES_TABLE_MAX_VERSION 2u
#define EFI_MEMORY_ATTRIBUTES_MAX_ENTRIES       65536u

typedef struct {
    uint32_t version;
    uint32_t number_of_entries;
    uint32_t descriptor_size;
    uint32_t flags;
} EfiMemoryAttributesTable;

static void efi_apply_memory_attributes(vmm_context_t *kctx, uint8_t *map,
                                        uint32_t map_size, uint32_t desc_size)
{
    void *raw = efi_find_configuration_table(&EFI_MEMORY_ATTRIBUTES_TABLE_GUID);
    if (!raw) {
        kprintf("[EFI] firmware publishes no memory-attributes table — "
                "runtime code keeps write permission\n");
        return;
    }

    EfiMemoryAttributesTable *t =
        (EfiMemoryAttributesTable *)vmm_phys_to_virt((uintptr_t)raw);
    if (!t) return;

    if (t->version == 0 || t->version > EFI_MEMORY_ATTRIBUTES_TABLE_MAX_VERSION ||
        t->descriptor_size < sizeof(EfiMemoryDescriptor) ||
        t->descriptor_size > desc_size ||
        t->number_of_entries == 0 ||
        t->number_of_entries > EFI_MEMORY_ATTRIBUTES_MAX_ENTRIES) {
        kprintf("[EFI] memory-attributes table not understood "
                "(version=%u desc=%u entries=%u) — ignored\n",
                t->version, t->descriptor_size, t->number_of_entries);
        return;
    }

    uint8_t *first = (uint8_t *)t + sizeof(EfiMemoryAttributesTable);
    uint32_t tightened = 0, skipped = 0;

    for (uint32_t i = 0; i < t->number_of_entries; i++) {
        EfiMemoryDescriptor *d =
            (EfiMemoryDescriptor *)(first + (size_t)i * t->descriptor_size);

        uint64_t phys = d->physical_start;
        uint64_t span = d->number_of_pages * EFI_PAGE_SIZE;

        if ((d->type != EFI_RUNTIME_SERVICES_CODE &&
             d->type != EFI_RUNTIME_SERVICES_DATA) ||
            d->number_of_pages == 0 ||
            (phys & (EFI_PAGE_SIZE - 1)) != 0) {
            skipped++;
            continue;
        }

        EfiMemoryDescriptor *cover =
            efi_find_descriptor(map, map_size, desc_size, phys);
        if (!cover ||
            !(cover->attribute & EFI_MEMORY_RUNTIME) ||
            cover->type != d->type ||
            cover->physical_start + efi_pages_to_bytes(cover->number_of_pages)
                < phys + span) {
            skipped++;
            continue;
        }

        uint64_t flags = VMM_FLAG_PRESENT | efi_rt_cache_flags(cover->attribute);
        if (!(d->attribute & EFI_MEMORY_RO)) flags |= VMM_FLAG_WRITABLE;
        if (d->attribute & EFI_MEMORY_XP)    flags |= VMM_FLAG_NO_EXECUTE;

        bool a = vmm_protect(kctx, EFI_RT_VA_BASE + (uintptr_t)phys,
                             (size_t)span, flags);
        bool b = vmm_protect(kctx, (uintptr_t)phys, (size_t)span, flags);
        if (a && b) {
            tightened++;
        } else {
            skipped++;
            kprintf("[EFI] could not tighten runtime region 0x%lx "
                    "(%lu page(s))\n",
                    (unsigned long)phys, (unsigned long)d->number_of_pages);
        }
    }

    kprintf("[EFI] memory attributes: %u runtime region(s) tightened to what "
            "the firmware declared, %u left as they were\n",
            tightened, skipped);
}


bool efi_runtime_init(void)
{
    if (g_rt_available) return true;

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || !boot_info_has_efi_rt(bi)) {
        debug_printf("[EFI] boot_info lacks EFI RT handoff (boot_method=%u version=%u); "
                     "EFI runtime services unavailable\n",
                     bi ? bi->boot_method : 0,
                     bi ? bi->version    : 0);
        return false;
    }

    if (bi->boot_method != 1) {
        debug_printf("[EFI] BIOS boot — no EFI runtime services\n");
        return false;
    }

    if (!bi->efi_rt_services_phys || !bi->efi_mmap_phys ||
        !bi->efi_mmap_size || !bi->efi_mmap_desc_size) {
        debug_printf("[EFI] boot_info missing EFI handoff fields\n");
        return false;
    }

    g_rt_fw_revision = bi->efi_fw_revision;

    uint8_t *map = (uint8_t *)vmm_phys_to_virt((uintptr_t)bi->efi_mmap_phys);
    if (!map) {
        debug_printf("[EFI] vmm_phys_to_virt failed for mmap 0x%lx\n",
                     (unsigned long)bi->efi_mmap_phys);
        return false;
    }

    {
        e820_entry_t *e = memory_map_get_entries();
        size_t        n = memory_map_get_entry_count();
        uint64_t      a = bi->efi_mmap_phys;
        bool          loose = false;
        for (size_t i = 0; i < n && e; i++) {
            if (e[i].type != E820_USABLE || e[i].length == 0) continue;
            if (a >= e[i].base && a < e[i].base + e[i].length) { loose = true; break; }
        }
        if (loose) {
            kprintf("[EFI] the staged memory map at 0x%lx sits in memory the "
                    "allocator calls free — the loader's E820 is older than "
                    "its own allocations\n", (unsigned long)a);
        } else {
            debug_printf("[EFI] staged memory map at 0x%lx is out of the "
                         "allocator's reach\n", (unsigned long)a);
        }
    }

    debug_printf("[EFI] runtime init: mmap=0x%lx size=%u desc_size=%u "
                 "desc_ver=%u rt_services=0x%lx fw_rev=0x%x\n",
                 (unsigned long)bi->efi_mmap_phys,
                 bi->efi_mmap_size,
                 bi->efi_mmap_desc_size,
                 bi->efi_mmap_desc_ver,
                 (unsigned long)bi->efi_rt_services_phys,
                 bi->efi_fw_revision);

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx) {
        debug_printf("[EFI] no kernel VMM context\n");
        return false;
    }

    if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }

    uint8_t *p   = map;
    uint8_t *end = map + bi->efi_mmap_size;
    uint32_t desc_size = bi->efi_mmap_desc_size;

    g_rt_svam_descriptors    = 0;
    g_rt_runtime_descriptors = 0;
    g_rt_runtime_pages_total = 0;

    while (p + desc_size <= end) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)p;
        if (d->attribute & EFI_MEMORY_RUNTIME) {
            g_rt_svam_descriptors++;
            if (!efi_map_rt_descriptor(d, kctx)) {
                kprintf("[EFI] a runtime region could not be mapped — "
                        "no EFI runtime services on this boot\n");
                return false;
            }
        }
        p += desc_size;
    }

    if (g_rt_runtime_descriptors == 0) {
        debug_printf("[EFI] no EFI_MEMORY_RUNTIME descriptors found; "
                     "calling RT in physical mode\n");
        g_rt = (EfiRuntimeServices *)vmm_phys_to_virt(
            (uintptr_t)bi->efi_rt_services_phys);
        if (!g_rt) return false;
        if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
        g_rt_available = true;
        return true;
    }

    EfiMemoryDescriptor *rt_desc = efi_find_descriptor(
        map, bi->efi_mmap_size, desc_size, bi->efi_rt_services_phys);
    if (!rt_desc) {
        debug_printf("[EFI] cannot locate descriptor for rt_services 0x%lx\n",
                     (unsigned long)bi->efi_rt_services_phys);
        return false;
    }
    if (!(rt_desc->attribute & EFI_MEMORY_RUNTIME)) {
        debug_printf("[EFI] descriptor for rt_services 0x%lx lacks RUNTIME bit\n",
                     (unsigned long)bi->efi_rt_services_phys);
        return false;
    }
    uint64_t rt_new_va = rt_desc->virtual_start +
                         (bi->efi_rt_services_phys - rt_desc->physical_start);

    EfiRuntimeServices *rt_phys = (EfiRuntimeServices *)vmm_phys_to_virt(
        (uintptr_t)bi->efi_rt_services_phys);
    if (!rt_phys || !rt_phys->set_virtual_address_map) {
        kprintf("[EFI] RT services table at 0x%lx has no SVAM entry\n",
                (unsigned long)bi->efi_rt_services_phys);
        return false;
    }

    size_t   rt_bytes  = (size_t)g_rt_svam_descriptors * desc_size;
    size_t   rt_pages  = (rt_bytes + 0xFFFu) / 0x1000u;
    uintptr_t svam_phys = (uintptr_t)pmm_alloc(rt_pages);
    if (!svam_phys) {
        kprintf("[EFI] cannot allocate %lu page(s) for the SVAM map\n",
                (unsigned long)rt_pages);
        return false;
    }
    {
        vmm_map_result_t rs = vmm_map_pages(kctx, svam_phys, svam_phys,
                                            rt_pages, VMM_FLAGS_KERNEL_RW);
        if (!rs.success) {
            kprintf("[EFI] cannot identity-map the SVAM map at 0x%lx: %s\n",
                    (unsigned long)svam_phys,
                    rs.error_msg ? rs.error_msg : "(no msg)");
            pmm_free((void *)svam_phys, rt_pages);
            return false;
        }
    }

    uint8_t *svam_map = (uint8_t *)vmm_phys_to_virt(svam_phys);
    uint32_t svam_count = 0;
    for (uint8_t *q = map; q + desc_size <= end; q += desc_size) {
        EfiMemoryDescriptor *d = (EfiMemoryDescriptor *)q;
        if (!(d->attribute & EFI_MEMORY_RUNTIME)) continue;
        if (svam_count >= g_rt_svam_descriptors) break;
        memcpy(svam_map + (size_t)svam_count * desc_size, d, desc_size);
        svam_count++;
    }

    efi_boot_services_alias(map, bi->efi_mmap_size, desc_size, kctx, true);

    kprintf("[EFI] SetVirtualAddressMap: %u runtime region(s), %lu page(s) "
            "at base 0x%lx; rt_services VA=0x%lx\n",
            svam_count,
            (unsigned long)g_rt_runtime_pages_total,
            (unsigned long)EFI_RT_VA_BASE,
            (unsigned long)rt_new_va);

    uint64_t svam_rflags;
    efi_rt_lock(&svam_rflags);
    EfiStatus s = rt_phys->set_virtual_address_map(
        (EfiUintn)((EfiUintn)svam_count * desc_size),
        (EfiUintn)desc_size,
        bi->efi_mmap_desc_ver,
        (EfiMemoryDescriptor *)svam_phys);
    efi_rt_unlock(svam_rflags);

    vmm_unmap_pages(kctx, svam_phys, rt_pages);
    pmm_free((void *)svam_phys, rt_pages);

    efi_boot_services_alias(map, bi->efi_mmap_size, desc_size, kctx, false);
    PmmReleaseBootServicesMemory();

    if (!EFI_IS_ERROR(s))
        efi_apply_memory_attributes(kctx, map, bi->efi_mmap_size, desc_size);

    if (EFI_IS_ERROR(s)) {
        debug_printf("[EFI] SetVirtualAddressMap failed: 0x%lx\n",
                     (unsigned long)s);
        g_rt = rt_phys;
        if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
        g_rt_available = true;
        return true;
    }

    EfiRuntimeServices *rt_new = (EfiRuntimeServices *)(uintptr_t)rt_new_va;

    const uint64_t CANONICAL_KERNEL_MASK = 0xFFFF800000000000ULL;
    if (rt_new_va < EFI_RT_VA_BASE ||
        (rt_new_va & CANONICAL_KERNEL_MASK) != CANONICAL_KERNEL_MASK) {
        debug_printf("[EFI] rebased rt_services VA 0x%lx outside RT window — "
                     "keeping physical mode\n", (unsigned long)rt_new_va);
        g_rt = rt_phys;
    } else {
        if (efi_table_header_valid(rt_new)) {
            g_rt = rt_new;
            debug_printf("[EFI] runtime services online at VA 0x%lx "
                         "(post-SVAM, CRC OK)\n",
                         (unsigned long)rt_new_va);
        } else {
            debug_printf("[EFI] rt_services CRC32 mismatch at VA 0x%lx — "
                         "downgrading to physical mode\n",
                         (unsigned long)rt_new_va);
            g_rt = rt_phys;
        }
    }

    if (!g_rt_lock_init) { spinlock_init(&g_rt_lock); g_rt_lock_init = true; }
    g_rt_available = true;

    {
        EfiTime t;
        memset(&t, 0, sizeof(t));
        EfiStatus gs = efi_get_time(&t, NULL);
        if (!EFI_IS_ERROR(gs)) {
            kprintf("[EFI] runtime services answer: firmware clock reads "
                    "%04u-%02u-%02u %02u:%02u:%02u\n",
                    t.year, t.month, t.day, t.hour, t.minute, t.second);
        } else {
            kprintf("[EFI] runtime services answer: GetTime returned 0x%lx "
                    "(the call came back, which is the part that was in "
                    "question)\n", (unsigned long)gs);
        }
    }

    return true;
}

bool efi_runtime_available(void)
{
    return g_rt_available && g_rt != NULL;
}


EfiRuntimeServices *efi_rt_get(void)
{
    return g_rt_available ? g_rt : NULL;
}

bool efi_guid_equal(const EfiGuid *a, const EfiGuid *b)
{
    if (!a || !b) return false;
    return memcmp(a, b, sizeof(EfiGuid)) == 0;
}

#define EFI_RFLAGS_IF  (1ULL << 9)

static bool g_rt_if_violation_said = false;

void efi_rt_lock(uint64_t *saved_rflags)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    spin_lock(&g_rt_lock);
    *saved_rflags = rflags;
}

void efi_rt_unlock(uint64_t saved_rflags)
{
    uint64_t on_return;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(on_return));

    spin_unlock(&g_rt_lock);

    if ((on_return & EFI_RFLAGS_IF) && !(saved_rflags & EFI_RFLAGS_IF) &&
        !g_rt_if_violation_said) {
        g_rt_if_violation_said = true;
        kprintf("[EFI] firmware returned from a runtime call with interrupts "
                "ENABLED where this kernel had them off — UEFI 2.10 §8.1 says "
                "it must restore the flag. Cleared here, and after every call "
                "from now on.\n");
    }

    if (saved_rflags & EFI_RFLAGS_IF) __asm__ volatile("sti");
}

void efi_rt_watch_start(uint64_t *t0)
{
    *t0 = rdtsc();
}

void efi_rt_watch_end(const char *name, uint64_t t0)
{
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (!khz) return;
    uint64_t elapsed_ms = (rdtsc() - t0) / khz;
    if (elapsed_ms > EFI_RT_CALL_WARN_MS) {
        debug_printf("[EFI] WARNING: %s took %lu ms (firmware slow)\n",
                     name, (unsigned long)elapsed_ms);
    }
}


void efi_reset_system(EfiResetType type, EfiStatus status,
                      uint64_t data_size, void *data)
{
    if (!efi_runtime_available() || !g_rt->reset_system) {
        debug_printf("[EFI] reset_system unavailable\n");
        return;
    }

    __asm__ volatile("cli");
    spin_lock(&g_rt_lock);

    debug_printf("[EFI] ResetSystem(type=%u, status=0x%lx)\n",
                 type, (unsigned long)status);

    g_rt->reset_system(type, status, (EfiUintn)data_size, data);

    __asm__ volatile("cli");
    spin_unlock(&g_rt_lock);
    kprintf("[EFI] ResetSystem returned — the firmware refused the reset\n");
}

EfiStatus efi_get_time(EfiTime *time, EfiTimeCapabilities *cap)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->get_time) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);
    EfiStatus s = rt->get_time(time, cap);
    efi_rt_watch_end("GetTime", t0);
    efi_rt_unlock(rflags);
    return s;
}

EfiStatus efi_set_time(EfiTime *time)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->set_time) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);
    EfiStatus s = rt->set_time(time);
    efi_rt_watch_end("SetTime", t0);
    efi_rt_unlock(rflags);

    if (!EFI_IS_ERROR(s)) {
        debug_printf("[EFI] SetTime succeeded\n");
    }
    return s;
}


EfiConfigurationTable *efi_get_configuration_table(uint32_t *out_count)
{
    if (out_count) *out_count = 0;

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || bi->version != BOOT_INFO_VERSION4) {
        return NULL;
    }
    if (!bi->efi_cfg_table_phys || bi->efi_cfg_table_count == 0) {
        return NULL;
    }

    EfiConfigurationTable *ct = (EfiConfigurationTable *)
        vmm_phys_to_virt((uintptr_t)bi->efi_cfg_table_phys);
    if (!ct) return NULL;

    if (out_count) *out_count = bi->efi_cfg_table_count;
    return ct;
}

void *efi_find_configuration_table(const EfiGuid *target)
{
    if (!target) return NULL;

    uint32_t count = 0;
    EfiConfigurationTable *ct = efi_get_configuration_table(&count);
    if (!ct) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        if (efi_guid_equal(&ct[i].vendor_guid, target)) {
            return ct[i].vendor_table;
        }
    }
    return NULL;
}

void efi_runtime_print_info(void)
{
    if (!efi_runtime_available()) {
        debug_printf("[EFI] runtime services unavailable\n");
        return;
    }

    uint32_t cfg_count = 0;
    (void)efi_get_configuration_table(&cfg_count);

    debug_printf("[EFI] runtime: fw_rev=0x%x rt=%p desc_count=%u pages=%lu "
                 "cfg_entries=%u\n",
                 g_rt_fw_revision, (void *)g_rt,
                 g_rt_runtime_descriptors,
                 (unsigned long)g_rt_runtime_pages_total,
                 cfg_count);
}