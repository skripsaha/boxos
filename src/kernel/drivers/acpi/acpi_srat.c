#include "acpi_internal.h"
#include "klib.h"

/*
 * SRAT (Static Resource Affinity Table) parser — ACPI 6.5 §5.2.16.
 *
 * SRAT maps CPUs and physical RAM ranges to NUMA proximity domains. The OS
 * uses it to keep per-thread memory and per-IO buffers on the same NUMA
 * node as the CPU using them. BoxOS does not yet have a NUMA-aware
 * allocator; this pass parses the table and exports the topology via
 * acpi_get_numa(), so the future memory subsystem audit can consume it
 * without reparsing the firmware buffer.
 *
 * x86-only entry types are handled (Type 0 LAPIC, Type 1 Memory, Type 2
 * x2APIC). ARM-only types (GICC/GIC-ITS) are skipped silently.
 */

static void numa_record_domain(uint32_t domain) {
    for (uint8_t i = 0; i < g_acpi.numa.domain_count; i++) {
        if (g_acpi.numa.domains[i] == domain) return;
    }
    if (g_acpi.numa.domain_count >= ACPI_NUMA_MAX_DOMAINS) return;
    g_acpi.numa.domains[g_acpi.numa.domain_count++] = domain;
}

static void numa_add_cpu(uint32_t apic_id, uint32_t domain, bool enabled) {
    if (g_acpi.numa.cpu_count >= ACPI_NUMA_MAX_CPUS) return;
    acpi_numa_cpu_t* slot = &g_acpi.numa.cpus[g_acpi.numa.cpu_count++];
    slot->apic_id = apic_id;
    slot->domain  = domain;
    slot->enabled = enabled;
    if (enabled) numa_record_domain(domain);
}

static void numa_add_mem(uint64_t base, uint64_t length,
                          uint32_t domain, uint32_t flags) {
    if (g_acpi.numa.mem_count >= ACPI_NUMA_MAX_MEM_RANGES) return;
    acpi_numa_mem_t* slot = &g_acpi.numa.mem[g_acpi.numa.mem_count++];
    slot->base   = base;
    slot->length = length;
    slot->domain = domain;
    slot->flags  = flags;
    if (flags & SRAT_FLAG_ENABLED) numa_record_domain(domain);
}

void acpi_parse_srat(void) {
    memset(&g_acpi.numa, 0, sizeof(g_acpi.numa));

    acpi_srat_t* srat = (acpi_srat_t*)acpi_find_table("SRAT");
    if (!srat) return;

    debug_printf("[ACPI] SRAT len=%u — parsing NUMA topology\n",
                 srat->header.length);

    uint8_t* ptr = (uint8_t*)srat + sizeof(acpi_srat_t);
    uint8_t* end = (uint8_t*)srat + srat->header.length;

    while (ptr + sizeof(srat_entry_header_t) <= end) {
        srat_entry_header_t* eh = (srat_entry_header_t*)ptr;
        if (eh->length < sizeof(srat_entry_header_t)) break;
        if (ptr + eh->length > end) break;

        switch (eh->type) {
            case SRAT_TYPE_LOCAL_APIC: {
                if (eh->length < sizeof(srat_local_apic_t)) break;
                srat_local_apic_t* e = (srat_local_apic_t*)eh;
                uint32_t domain = (uint32_t)e->lo_domain
                                | ((uint32_t)e->hi_domain[0] <<  8)
                                | ((uint32_t)e->hi_domain[1] << 16)
                                | ((uint32_t)e->hi_domain[2] << 24);
                bool enabled = (e->flags & SRAT_FLAG_ENABLED) != 0;
                numa_add_cpu(e->apic_id, domain, enabled);
                debug_printf("[ACPI] SRAT LAPIC apic=%u domain=%u %s\n",
                             e->apic_id, domain,
                             enabled ? "enabled" : "disabled");
                break;
            }
            case SRAT_TYPE_MEMORY: {
                if (eh->length < sizeof(srat_memory_t)) break;
                srat_memory_t* e = (srat_memory_t*)eh;
                bool enabled = (e->flags & SRAT_FLAG_ENABLED) != 0;
                numa_add_mem(e->base_address, e->length,
                              e->domain, e->flags);
                debug_printf("[ACPI] SRAT MEM base=0x%lx len=0x%lx domain=%u "
                             "flags=0x%x %s%s%s\n",
                             (unsigned long)e->base_address,
                             (unsigned long)e->length, e->domain, e->flags,
                             enabled ? "[enabled] " : "[disabled] ",
                             (e->flags & SRAT_MEM_FLAG_HOTPLUG) ? "[hotplug] " : "",
                             (e->flags & SRAT_MEM_FLAG_NONVOL)  ? "[nv] " : "");
                break;
            }
            case SRAT_TYPE_LOCAL_X2APIC: {
                if (eh->length < sizeof(srat_local_x2apic_t)) break;
                srat_local_x2apic_t* e = (srat_local_x2apic_t*)eh;
                bool enabled = (e->flags & SRAT_FLAG_ENABLED) != 0;
                numa_add_cpu(e->x2apic_id, e->domain, enabled);
                debug_printf("[ACPI] SRAT x2APIC id=%u domain=%u %s\n",
                             e->x2apic_id, e->domain,
                             enabled ? "enabled" : "disabled");
                break;
            }
            default:
                /* GICC, GIC-ITS, Generic Initiator — not applicable to x86. */
                break;
        }
        ptr += eh->length;
    }

    g_acpi.numa.present = (g_acpi.numa.cpu_count > 0 ||
                           g_acpi.numa.mem_count > 0);
    debug_printf("[ACPI] SRAT parsed: %u CPUs, %u mem ranges, %u domains\n",
                 g_acpi.numa.cpu_count, g_acpi.numa.mem_count,
                 g_acpi.numa.domain_count);
}

const acpi_numa_info_t *acpi_get_numa(void) {
    return g_acpi.numa.present ? &g_acpi.numa : NULL;
}

uint32_t acpi_numa_domain_for_phys(uint64_t phys) {
    if (!g_acpi.numa.present) return ACPI_NUMA_DOMAIN_UNKNOWN;
    for (uint8_t i = 0; i < g_acpi.numa.mem_count; i++) {
        const acpi_numa_mem_t* r = &g_acpi.numa.mem[i];
        if (!(r->flags & SRAT_FLAG_ENABLED)) continue;
        if (phys >= r->base && phys < (r->base + r->length)) {
            return r->domain;
        }
    }
    return ACPI_NUMA_DOMAIN_UNKNOWN;
}

uint32_t acpi_numa_domain_for_apic(uint32_t apic_id) {
    if (!g_acpi.numa.present) return ACPI_NUMA_DOMAIN_UNKNOWN;
    for (uint16_t i = 0; i < g_acpi.numa.cpu_count; i++) {
        const acpi_numa_cpu_t *c = &g_acpi.numa.cpus[i];
        if (!c->enabled) continue;
        if (c->apic_id == apic_id) return c->domain;
    }
    return ACPI_NUMA_DOMAIN_UNKNOWN;
}
