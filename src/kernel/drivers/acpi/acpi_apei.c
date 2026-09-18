#include "acpi_internal.h"
#include "apei_ghes_runtime.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"
#include "touch.h"

/* ACPI 6.5 §18.3.2 — Boot Error Region.
 * 32 byte header followed by CPER records. Block Status flags the kind
 * of error captured at the previous boot. */
typedef struct {
    uint32_t block_status;          /* bits: 0 uncorr, 1 corr, 2 multi-uncorr, 3 multi-corr */
    uint32_t raw_data_offset;
    uint32_t raw_data_length;
    uint32_t data_length;
    uint32_t error_severity;        /* 0=recoverable, 1=fatal, 2=corrected, 3=none */
} __attribute__((packed)) acpi_bert_region_t;

/* UEFI Spec Appendix N — Common Platform Error Record.
 * Header is fixed 128 bytes followed by N section descriptors (72 each)
 * and the section data they point to.
 */
typedef struct {
    char     signature[4];          /* "CPER" */
    uint16_t revision;
    uint32_t signature_end;         /* 0xFFFFFFFF */
    uint16_t section_count;
    uint32_t error_severity;
    uint32_t validation_bits;
    uint32_t record_length;
    uint64_t timestamp;
    uint8_t  platform_id[16];
    uint8_t  partition_id[16];
    uint8_t  creator_id[16];
    uint8_t  notification_type[16];
    uint64_t record_id;
    uint32_t flags;
    uint64_t persistence_info;
    uint8_t  reserved[12];
} __attribute__((packed)) cper_record_header_t;
_Static_assert(sizeof(cper_record_header_t) == 128, "CPER header = 128 bytes");

typedef struct {
    uint32_t section_offset;
    uint32_t section_length;
    uint16_t revision;
    uint8_t  validation_bits;
    uint8_t  reserved;
    uint32_t flags;
    uint8_t  section_type[16];
    uint8_t  fru_id[16];
    uint32_t section_severity;
    char     fru_text[20];
} __attribute__((packed)) cper_section_desc_t;
_Static_assert(sizeof(cper_section_desc_t) == 72, "CPER section desc = 72 bytes");

/* Section Type GUIDs (UEFI Spec Appendix N). Stored little-endian wire
 * format (data1..data4 LE, the byte tail as-is). */
static const uint8_t SECT_GUID_PROCESSOR[16] = {
    0xB0,0xA0,0x3E,0xDC, 0x44,0xA1, 0x97,0x47,
    0xB9,0x5B, 0x53,0xFA,0x24,0x2B,0x6E,0x1D
};
static const uint8_t SECT_GUID_MEMORY[16] = {
    0x14,0x11,0xBC,0xA5, 0x64,0x6F, 0xDE,0x4E,
    0xB8,0x63, 0x3E,0x83,0xED,0x7C,0x83,0xB1
};
static const uint8_t SECT_GUID_PCIE[16] = {
    0x54,0xE9,0x95,0xD9, 0xC1,0xBB, 0x0F,0x43,
    0xAD,0x91, 0xB4,0x4D,0xCB,0x3C,0x6F,0x35
};
static const uint8_t SECT_GUID_GENERIC[16] = {
    0xAD,0xCC,0x76,0x98, 0xB4,0x47, 0xDB,0x4B,
    0xB6,0x5E, 0x16,0xF1,0x93,0xC4,0xF3,0xDB
};

static const char* section_type_name(const uint8_t* guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != SECT_GUID_PROCESSOR[i]) goto try_mem;
    }
    return "Processor";
try_mem:
    for (int i = 0; i < 16; i++) {
        if (guid[i] != SECT_GUID_MEMORY[i]) goto try_pcie;
    }
    return "Memory";
try_pcie:
    for (int i = 0; i < 16; i++) {
        if (guid[i] != SECT_GUID_PCIE[i]) goto try_generic;
    }
    return "PCIe";
try_generic:
    for (int i = 0; i < 16; i++) {
        if (guid[i] != SECT_GUID_GENERIC[i]) return "Unknown";
    }
    return "Generic";
}

static const char* cper_severity_text(uint32_t sev) {
    switch (sev) {
        case 0: return "recoverable";
        case 1: return "fatal";
        case 2: return "corrected";
        case 3: return "informational";
        default: return "unknown";
    }
}

/* Decode the CPER record(s) that follow the BERT header. Logs the
 * record header + a one-line summary of every section descriptor.
 * `region` is the full BERT region MMIO map; `region_len` is its
 * declared length. */
static void cper_decode_region(volatile uint8_t* region,
                                uint32_t region_len,
                                uint32_t data_length) {
    /* The first CPER record sits immediately after the 32-byte BERT
     * header. data_length tells us how many CPER bytes follow. */
    if (data_length < sizeof(cper_record_header_t)) {
        debug_printf("[APEI] BERT data_length %u below CPER header size\n",
                     data_length);
        return;
    }
    if (sizeof(acpi_bert_region_t) + data_length > region_len) {
        debug_printf("[APEI] BERT data_length %u exceeds region (%u)\n",
                     data_length, region_len);
        return;
    }

    volatile cper_record_header_t* rec =
        (volatile cper_record_header_t*)(region + sizeof(acpi_bert_region_t));

    if (rec->signature[0] != 'C' || rec->signature[1] != 'P' ||
        rec->signature[2] != 'E' || rec->signature[3] != 'R') {
        debug_printf("[APEI] BERT record missing CPER signature (got %.4s)\n",
                     (const char*)rec->signature);
        return;
    }
    if (rec->signature_end != 0xFFFFFFFFu) {
        debug_printf("[APEI] BERT CPER signature_end mismatch (0x%x)\n",
                     rec->signature_end);
        return;
    }

    debug_printf("[APEI]   CPER rev=0x%x sections=%u severity=%s flags=0x%x len=%u\n",
                 rec->revision, rec->section_count,
                 cper_severity_text(rec->error_severity),
                 rec->flags, rec->record_length);

    static const char* sect_tag[] = {
        [0] = "acpi:cper:processor",
        [1] = "acpi:cper:memory",
        [2] = "acpi:cper:pcie",
        [3] = "acpi:cper:generic",
    };
    volatile cper_section_desc_t* desc =
        (volatile cper_section_desc_t*)(rec + 1);
    for (uint16_t i = 0; i < rec->section_count; i++) {
        /* Bound the descriptor walk to record_length so a bad firmware
         * cannot drive us off the end of the region. */
        uint32_t off = (uint32_t)((uintptr_t)(desc + i + 1)
                                 - (uintptr_t)rec);
        if (off > rec->record_length) {
            debug_printf("[APEI]     section desc %u beyond record len\n", i);
            break;
        }
        uint8_t guid_buf[16];
        for (int j = 0; j < 16; j++) guid_buf[j] = desc[i].section_type[j];
        const char* tname = section_type_name(guid_buf);
        debug_printf("[APEI]   section[%u] type=%s sev=%s len=%u off=%u\n",
                     i, tname,
                     cper_severity_text(desc[i].section_severity),
                     desc[i].section_length, desc[i].section_offset);
        /* Per-section tag broadcast: subscribers can scope their
         * interest (e.g. only memory or only PCIe). */
        int kind = (tname[0] == 'P' && tname[1] == 'r') ? 0
                 : (tname[0] == 'M') ? 1
                 : (tname[0] == 'P') ? 2
                 : (tname[0] == 'G') ? 3 : -1;
        if (kind >= 0) {
            struct { uint32_t sev; uint32_t len; uint32_t off; } sev_ev =
                { desc[i].section_severity, desc[i].section_length,
                  desc[i].section_offset };
            TouchPublish(sect_tag[kind], &sev_ev, sizeof(sev_ev));
        }
    }
}

/*
 * APEI (ACPI Platform Error Interfaces) — ACPI 6.5 §18.
 *
 * Three tables, all optional, all detected here:
 *   HEST — Hardware Error Source Table (catalogue of error sources)
 *   BERT — Boot Error Record Table (one-shot record carried across reboot)
 *   ERST — Error Record Serialization Table (persistent error log API)
 *
 * BoxOS does not yet act as a full WHEA-style RAS consumer. This pass
 * only records presence + handful of top-level fields so a future RAS
 * subsystem can find the descriptors without re-walking the firmware
 * tables. Server boards that expose machine-check details, IPMI alerts,
 * or persistent log regions surface them through these tables.
 */

/* HEST error source type codes (ACPI 6.5 §18.3.2, Table 18.2). Every
 * type begins with the same 4-byte header: type(2), source_id(2). A HEST
 * entry carries no length field of its own — there is nothing
 * runtime-declared to step by — so the walker derives each entry's length
 * from the layout the spec gives that type, and for the three IA-32 types
 * from the bank count stored inside the entry. See hest_entry_len. */
#define HEST_TYPE_IA32_MCE         0
#define HEST_TYPE_IA32_CMCI        1
#define HEST_TYPE_IA32_NMI         2
#define HEST_TYPE_AER_ROOT_PORT    6
#define HEST_TYPE_AER_ENDPOINT     7
#define HEST_TYPE_AER_BRIDGE       8
#define HEST_TYPE_GHES             9
#define HEST_TYPE_GHESV2           10
#define HEST_TYPE_IA32_DMC         11

/* Hardware Error Notification Structure — ACPI 6.5 §18.3.2.9, Table
 * 18.14. 28 bytes, embedded in a GHES entry at byte offset 32 (0x20) per
 * Table 18.10, and in the IA-32 Corrected/Deferred Machine Check
 * structures at offset 16. The Type field tells the OS how the platform
 * signals an error to us. */
typedef struct {
    uint8_t  type;           /* 0=Polled, 1=External-IRQ, 2=Local-IRQ,
                                3=SCI, 4=NMI, 5=CMCI, 6=MCE, 7=GPIO,
                                8=SEA, 9=SEI, 10=ExtInt (GSIV) */
    uint8_t  length;
    uint16_t cfg_write_enable;
    uint32_t poll_interval;
    uint32_t vector;
    uint32_t switch_to_polling_value;
    uint32_t switch_to_polling_window;
    uint32_t error_threshold_value;
    uint32_t error_threshold_window;
} __attribute__((packed)) ghes_notification_t;
_Static_assert(sizeof(ghes_notification_t) == 28, "GHES notification = 28 bytes");

/* Generic Hardware Error Source — ACPI 6.5 §18.3.2.7, Table 18.10.
 * The byte offsets are the spec's own: Type@0, Source Id@2, Related
 * Source Id@4, Flags@6, Enabled@7, Records To Pre-allocate@8, Max
 * Sections Per Record@12, Max Raw Data Length@16, Error Status
 * Address@20 (GAS, 12 B), Notification Structure@32 (28 B), Error Status
 * Block Length@60 — 64 bytes in total. The static assert below is the
 * whole point of spelling the layout out as a structure: every offset in
 * this file now comes from one place the compiler checks. */
typedef struct {
    uint16_t            type;
    uint16_t            source_id;
    uint16_t            related_source_id;
    uint8_t             reserved;
    uint8_t             enabled;
    uint32_t            records_to_preallocate;
    uint32_t            max_sections_per_record;
    uint32_t            max_raw_data_length;
    acpi_gas_t          error_status_address;
    ghes_notification_t notify;
    uint32_t            error_status_block_length;
} __attribute__((packed)) HestGenericSource;
_Static_assert(sizeof(HestGenericSource) == 64,
               "GHES structure = 64 bytes (ACPI 6.5 Table 18.10)");

/* GHESv2 — ACPI 6.5 §18.3.2.8, Table 18.13: the GHES structure followed
 * by Read Ack Register@64 (GAS, 12 B), Read Ack Preserve@76 and Read Ack
 * Write@84 — 92 bytes in total. */
typedef struct {
    HestGenericSource generic;
    acpi_gas_t        read_ack_register;
    uint64_t          read_ack_preserve;
    uint64_t          read_ack_write;
} __attribute__((packed)) HestGenericSourceV2;
_Static_assert(sizeof(HestGenericSourceV2) == 92,
               "GHESv2 structure = 92 bytes (ACPI 6.5 Table 18.13)");

/* GAS address space identifiers used by this file (ACPI 6.5 §5.2.3.2). */
#define GAS_AS_SYSTEM_MEMORY  0
#define GAS_AS_SYSTEM_IO      1

static const char* ghes_notify_name(uint8_t t) {
    switch (t) {
        case 0:  return "Polled";
        case 1:  return "External-IRQ";
        case 2:  return "Local-IRQ";
        case 3:  return "SCI";
        case 4:  return "NMI";
        case 5:  return "CMCI";
        case 6:  return "MCE";
        case 7:  return "GPIO";
        case 8:  return "SEA";
        case 9:  return "SEI";
        case 10: return "ExtInt(GSIV)";
        case 11: return "SDEI";
        default: return "?";
    }
}

/* Read the register named by a GHES Error Status Address and return the
 * physical address of the Generic Error Status Block it holds.
 *
 * ACPI 6.5 §18.3.2.7, Table 18.10, Error Status Address: "This field
 * specifies the location of a register that contains the physical address
 * of a block of memory that holds the error status data for this error
 * source", and after the table: "the Error Status Address field specifies
 * the location of an 8-byte memory-mapped register that holds the
 * physical address of the error status block". The GAS address is
 * therefore NOT the status block — there is one level of indirection, and
 * a kernel that skips it maps the pointer register as if it were the
 * block and, on acknowledgement, writes zero over the firmware's own
 * pointer. Linux performs the same read in
 * drivers/acpi/apei/ghes.c::__ghes_peek_estatus().
 *
 * The register is read once, here, while the HEST is being walked, and
 * only the value is carried into the runtime. A firmware that republishes
 * a different block address later in the boot would not be followed; that
 * re-read belongs to the runtime path (apei_ghes_runtime.c), which owns
 * the per-source descriptor and the polling, and it needs the register
 * itself — not just its value — to get there.
 *
 * Returns false and says so out loud when the register cannot be read;
 * *out_phys is 0 in that case. A register that reads back 0 is not an
 * error here: it means the firmware has published no status block for
 * this source. */
static bool ghes_read_status_block_pointer(const acpi_gas_t* gas,
                                           uint16_t source_id,
                                           uint64_t* out_phys) {
    *out_phys = 0;
    if (gas->address == 0) {
        kprintf("[APEI] GHES src=%u: error status address register is null "
                "in HEST\n", source_id);
        return false;
    }

    /* The register is 8 bytes by definition. Firmware that publishes a bit
     * width of 0 — or any width that is not an access size the CPU has —
     * is honoured as the spec's 8 bytes rather than refused, the same
     * fixup Linux applies in apei_check_gar(). */
    uint8_t width = gas->bit_width;
    if (width != 8 && width != 16 && width != 32 && width != 64) width = 64;

    if (gas->address_space == GAS_AS_SYSTEM_MEMORY) {
        /* Mapped read-only: this register belongs to the firmware and the
         * OS has no business writing it, ever. Unmapped again immediately
         * — one boot-time read per source, not a permanent window. */
        volatile void* reg = vmm_map_mmio((uintptr_t)gas->address, 8,
                                          VMM_FLAGS_KERNEL_RO);
        if (!reg) {
            kprintf("[APEI] GHES src=%u: cannot map error status address "
                    "register 0x%lx\n",
                    source_id, (unsigned long)gas->address);
            return false;
        }
        uint64_t value;
        switch (width) {
            case 8:  value = *(volatile uint8_t*)reg;  break;
            case 16: value = *(volatile uint16_t*)reg; break;
            case 32: value = *(volatile uint32_t*)reg; break;
            default: value = *(volatile uint64_t*)reg; break;
        }
        vmm_unmap_mmio(reg, 8);
        *out_phys = value;
        return true;
    }

    if (gas->address_space == GAS_AS_SYSTEM_IO) {
        uint16_t port = (uint16_t)gas->address;
        switch (width) {
            case 8:  *out_phys = inb(port); return true;
            case 16: *out_phys = inw(port); return true;
            case 32: *out_phys = inl(port); return true;
            default: break;
        }
        /* No 64-bit port access exists on x86; a table asking for one is
         * describing a register the CPU cannot read. */
        kprintf("[APEI] GHES src=%u: error status address register is a "
                "%u-bit I/O port (0x%x) — unreadable on this CPU\n",
                source_id, width, port);
        return false;
    }

    kprintf("[APEI] GHES src=%u: error status address register lives in "
            "address space %u, which this kernel cannot read\n",
            source_id, gas->address_space);
    return false;
}

/* Decode one GHES / GHESv2 HEST entry (ACPI 6.5 Tables 18.10 / 18.13)
 * and hand the source to the runtime. */
static void decode_ghes(uint8_t* entry, bool v2) {
    const HestGenericSource* g = (const HestGenericSource*)entry;
    const ghes_notification_t* n = &g->notify;

    uint64_t ack_addr = 0, ack_preserve = 0, ack_write = 0;
    if (v2) {
        const HestGenericSourceV2* g2 = (const HestGenericSourceV2*)entry;
        ack_addr     = g2->read_ack_register.address;
        ack_preserve = g2->read_ack_preserve;
        ack_write    = g2->read_ack_write;
    }

    debug_printf("[APEI]   GHES%s src=%u enabled=%u records=%u sections=%u "
                 "notify=%s vector=%u poll=%u status-reg=0x%lx len=%u\n",
                 v2 ? "v2" : "", g->source_id, g->enabled,
                 g->records_to_preallocate, g->max_sections_per_record,
                 ghes_notify_name(n->type), n->vector, n->poll_interval,
                 (unsigned long)g->error_status_address.address,
                 g->error_status_block_length);

    if (!g->enabled) return;

    uint64_t gesb_phys = 0;
    if (!ghes_read_status_block_pointer(&g->error_status_address,
                                        g->source_id, &gesb_phys) ||
        gesb_phys == 0) {
        /* kprintf, not debug_printf: an enabled source the kernel cannot
         * follow is a class of hardware error this machine will never
         * report, and that sentence has to survive into a shipped build. */
        kprintf("[APEI] GHES src=%u is enabled, but its error status address "
                "register (0x%lx) names no status block — this source cannot "
                "report a hardware error\n",
                g->source_id, (unsigned long)g->error_status_address.address);
        return;
    }

    debug_printf("[APEI]   GHES src=%u status block at 0x%lx\n",
                 g->source_id, (unsigned long)gesb_phys);

    (void)apei_ghes_register_source(g->source_id, v2, n->type, n->vector,
                                     n->poll_interval, gesb_phys,
                                     g->error_status_block_length,
                                     ack_addr, ack_preserve, ack_write);
}

static const char* hest_type_name(uint16_t t) {
    switch (t) {
        case HEST_TYPE_IA32_MCE:       return "IA32 Machine Check";
        case HEST_TYPE_IA32_CMCI:      return "IA32 CMCI";
        case HEST_TYPE_IA32_NMI:       return "IA32 NMI";
        case HEST_TYPE_AER_ROOT_PORT:  return "PCIe AER (Root Port)";
        case HEST_TYPE_AER_ENDPOINT:   return "PCIe AER (Endpoint)";
        case HEST_TYPE_AER_BRIDGE:     return "PCIe AER (Bridge)";
        case HEST_TYPE_GHES:           return "GHES";
        case HEST_TYPE_GHESV2:         return "GHESv2";
        case HEST_TYPE_IA32_DMC:       return "IA32 DMC";
        default:                        return "Unknown";
    }
}

/* Machine Check Bank Structure — ACPI 6.5 §18.3.2.1.1, Table 18.4. */
#define HEST_MC_BANK_LEN         28
/* IA-32 Machine Check Exception — Table 18.3: 40 fixed bytes, then one
 * bank structure per Number Of Hardware Banks, which sits at offset 32. */
#define HEST_IA32_MCE_FIXED      40
#define HEST_IA32_MCE_BANKS_OFF  32
/* IA-32 Corrected Machine Check (Table 18.5) and IA-32 Deferred Machine
 * Check (§18.3.2.10, Table 18.15) share a layout: 48 fixed bytes, bank
 * count at offset 44, then one bank structure each. */
#define HEST_IA32_CMC_FIXED      48
#define HEST_IA32_CMC_BANKS_OFF  44

/* Byte length of one HEST error-source entry.
 *
 * A HEST entry does not carry its own length, so the length comes from
 * the layout ACPI 6.5 gives its type — and for the three IA-32 types,
 * which end in an array of machine-check bank structures, from the bank
 * count inside the entry itself. Fixed sizes: IA-32 NMI 20 (Table 18.6),
 * AER root port 48 / endpoint 44 / bridge 56 (Tables 18.7, 18.8, 18.9),
 * GHES 64 and GHESv2 92 (Tables 18.10, 18.13, taken from the structures
 * above so there is one source of truth). Linux computes exactly the same
 * lengths in drivers/acpi/apei/hest.c::hest_esrc_len().
 *
 * `avail` is how many bytes of the table remain at `entry`; 0 is returned
 * when the type is unknown or when the entry's own fixed part does not
 * fit in what is left, and the caller stops walking. */
static uint32_t hest_entry_len(uint16_t t, const uint8_t* entry, size_t avail) {
    switch (t) {
        case HEST_TYPE_IA32_MCE:
            if (avail < HEST_IA32_MCE_FIXED) return 0;
            return HEST_IA32_MCE_FIXED +
                   (uint32_t)entry[HEST_IA32_MCE_BANKS_OFF] * HEST_MC_BANK_LEN;
        case HEST_TYPE_IA32_CMCI:
        case HEST_TYPE_IA32_DMC:
            if (avail < HEST_IA32_CMC_FIXED) return 0;
            return HEST_IA32_CMC_FIXED +
                   (uint32_t)entry[HEST_IA32_CMC_BANKS_OFF] * HEST_MC_BANK_LEN;
        case HEST_TYPE_IA32_NMI:      return 20;
        case HEST_TYPE_AER_ROOT_PORT: return 48;
        case HEST_TYPE_AER_ENDPOINT:  return 44;
        case HEST_TYPE_AER_BRIDGE:    return 56;
        case HEST_TYPE_GHES:          return (uint32_t)sizeof(HestGenericSource);
        case HEST_TYPE_GHESV2:        return (uint32_t)sizeof(HestGenericSourceV2);
        default:                      return 0;
    }
}

void acpi_parse_apei(void) {
    memset(&g_acpi.apei, 0, sizeof(g_acpi.apei));

    acpi_hest_t* hest = (acpi_hest_t*)acpi_find_table("HEST");
    if (hest) {
        g_acpi.apei.hest_present          = true;
        g_acpi.apei.hest_error_source_count = hest->error_source_count;
        debug_printf("[ACPI] HEST: %u error source(s), len=%u\n",
                     hest->error_source_count, hest->header.length);

        /* Walk every error source, classifying by type code at offset 0. */
        uint8_t* ptr = (uint8_t*)hest + sizeof(acpi_hest_t);
        uint8_t* end = (uint8_t*)hest + hest->header.length;
        uint32_t walked = 0;
        while (ptr + 4 <= end && walked < hest->error_source_count) {
            uint16_t type = *(uint16_t*)ptr;
            uint16_t srcid = *(uint16_t*)(ptr + 2);
            size_t   avail = (size_t)(end - ptr);
            uint32_t step  = hest_entry_len(type, ptr, avail);
            if (step == 0 || step > avail) {
                /* kprintf, not debug_printf: the walk has lost its place in
                 * the table, and every error source behind this point is a
                 * class of hardware failure this machine will never report.
                 * That sentence has to survive into a shipped build. */
                kprintf("[APEI] HEST: entry %u at offset %lu declares type %u "
                        "the kernel cannot size (step=%u, %lu bytes left) — "
                        "%u of %u error sources abandoned\n",
                        walked, (unsigned long)(ptr - (uint8_t*)hest), type,
                        step, (unsigned long)avail,
                        hest->error_source_count - walked,
                        hest->error_source_count);
                break;
            }
            debug_printf("[ACPI]   HEST[%u]: %s (type=%u, src=%u)\n",
                         walked, hest_type_name(type), type, srcid);
            if (type == HEST_TYPE_GHES)   decode_ghes(ptr, false);
            if (type == HEST_TYPE_GHESV2) decode_ghes(ptr, true);
            ptr += step;
            walked++;
        }
    }

    acpi_bert_t* bert = (acpi_bert_t*)acpi_find_table("BERT");
    if (bert) {
        g_acpi.apei.bert_present        = true;
        g_acpi.apei.bert_region_address = bert->region_address;
        g_acpi.apei.bert_region_length  = bert->region_length;
        debug_printf("[ACPI] BERT: region 0x%lx len=%u\n",
                     (unsigned long)bert->region_address,
                     bert->region_length);
    }

    acpi_erst_t* erst = (acpi_erst_t*)acpi_find_table("ERST");
    if (erst) {
        g_acpi.apei.erst_present         = true;
        g_acpi.apei.erst_instruction_count = erst->instruction_entry_count;
        acpi_erst_bind(erst);
        debug_printf("[ACPI] ERST: %u instruction entries\n",
                     erst->instruction_entry_count);
        /* Each instruction entry is 32 bytes:
         *   Action(1) Instruction(1) Flags(1) Reserved(1)
         *   RegisterRegion(GAS, 12)
         *   Value(8) Mask(8). */
        uint8_t* base = (uint8_t*)erst + sizeof(acpi_erst_t);
        uint32_t entry_size = 32;
        for (uint32_t i = 0; i < erst->instruction_entry_count; i++) {
            uint8_t* e = base + i * entry_size;
            if (e + entry_size > (uint8_t*)erst + erst->header.length) break;
            uint8_t action = e[0];
            uint8_t instr  = e[1];
            uint8_t flags  = e[2];
            /* +4 .. +15 : Register Region (Generic Address Structure).
             * +16 .. +23 : Value
             * +24 .. +31 : Mask */
            acpi_gas_t* reg = (acpi_gas_t*)(e + 4);
            uint64_t value = *(uint64_t*)(e + 16);
            uint64_t mask  = *(uint64_t*)(e + 24);
            debug_printf("[ACPI]   ERST[%u]: action=0x%02x instr=0x%02x flags=0x%02x "
                         "reg=as%u/bw%u@0x%lx val=0x%lx mask=0x%lx\n",
                         i, action, instr, flags,
                         reg->address_space, reg->bit_width,
                         (unsigned long)reg->address,
                         (unsigned long)value, (unsigned long)mask);
        }
    }
}

const acpi_apei_info_t *acpi_get_apei(void) {
    if (g_acpi.apei.hest_present || g_acpi.apei.bert_present ||
        g_acpi.apei.erst_present) {
        return &g_acpi.apei;
    }
    return NULL;
}

static const char* severity_text(uint32_t sev) {
    switch (sev) {
        case 0: return "recoverable";
        case 1: return "fatal";
        case 2: return "corrected";
        case 3: return "none";
        default: return "unknown";
    }
}

/* Read the BERT region (if present) and print a one-line summary so a
 * prior-boot uncorrectable error is visible in serial. The actual CPER
 * decode + dispatch into a runtime RAS pipeline belongs to a future
 * APEI/WHEA driver — at this layer we just surface the fact. */
void acpi_apei_consume(void) {
    if (!g_acpi.apei.bert_present) return;
    if (g_acpi.apei.bert_region_length < sizeof(acpi_bert_region_t)) {
        debug_printf("[APEI] BERT region length %u below header size\n",
                     g_acpi.apei.bert_region_length);
        return;
    }
    volatile acpi_bert_region_t* rg = (volatile acpi_bert_region_t*)
        vmm_map_mmio((uintptr_t)g_acpi.apei.bert_region_address,
                      g_acpi.apei.bert_region_length,
                      VMM_FLAGS_KERNEL_RW);
    if (!rg) {
        debug_printf("[APEI] BERT region map failed (addr=0x%lx len=%u)\n",
                     (unsigned long)g_acpi.apei.bert_region_address,
                     g_acpi.apei.bert_region_length);
        return;
    }

    uint32_t status = rg->block_status;
    if (status == 0) {
        debug_printf("[APEI] BERT: no error recorded from previous boot\n");
        return;
    }
    debug_printf("[APEI] BERT: %s previous-boot error, severity=%s, data_len=%u\n",
                 (status & 0x1) ? "uncorrectable" :
                 (status & 0x2) ? "correctable"   :
                 (status & 0x4) ? "multi-uncorr"  :
                 (status & 0x8) ? "multi-corr"    : "unknown",
                 severity_text(rg->error_severity),
                 rg->data_length);

    /* Broadcast the prior-boot error so a userspace logger / telemetry
     * daemon (anyone subscribing to `acpi:boot-error`) can persist it
     * before the next crash. */
    struct { uint32_t status; uint32_t severity; uint32_t data_length; } ev =
        { status, rg->error_severity, rg->data_length };
    TouchPublish("acpi:boot-error", &ev, sizeof(ev));

    /* Decode the embedded CPER record + each section. */
    cper_decode_region((volatile uint8_t*)rg,
                       g_acpi.apei.bert_region_length,
                       rg->data_length);

    if (g_acpi.apei.hest_present) {
        debug_printf("[APEI] HEST exposes %u error sources for runtime dispatch\n",
                     g_acpi.apei.hest_error_source_count);
    }
    if (g_acpi.apei.erst_present) {
        debug_printf("[APEI] ERST exposes %u serialization instructions\n",
                     g_acpi.apei.erst_instruction_count);
    }
}
