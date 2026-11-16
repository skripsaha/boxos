#include "acpi_internal.h"
#include "apei_ghes_runtime.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"
#include "touch.h"

/* ACPI 6.5 §18.3.2.7.1, Table 18.11 — Generic Error Status Block.
 *
 * This is also the layout of the Boot Error Region: §18.3.1 says "the
 * format of the Boot Error Region follows that of an Error Status Block, as
 * defined in the Generic Hardware Error Source Structure". Twenty bytes of
 * header, then Data Length bytes of Generic Error Data Entries — there is
 * no CPER record header and no "CPER" signature anywhere in it. */
typedef struct {
    uint32_t block_status;          /* bit 0 uncorrectable valid, 1 correctable
                                     * valid, 2 multiple uncorrectable, 3
                                     * multiple correctable, bits 13:4 entry
                                     * count */
    uint32_t raw_data_offset;
    uint32_t raw_data_length;
    uint32_t data_length;
    uint32_t error_severity;        /* 0=recoverable, 1=fatal, 2=corrected,
                                     * 3=none (Table 18.11; UEFI CPER calls
                                     * the same code informational) */
} __attribute__((packed)) GenericErrorStatusBlock;
_Static_assert(sizeof(GenericErrorStatusBlock) == 20,
               "Generic Error Status Block header = 20 bytes (Table 18.11)");

/* ACPI 6.5 §18.3.2.7.1, Table 18.12 — Generic Error Data Entry.
 *
 * The header is followed by Error Data Length bytes of section body, and
 * "it is valid to have a Data Length of zero". The Timestamp at offset 64
 * exists only from Revision 0x300 onwards, so the header is 72 bytes at
 * revision >= 0x300 and 64 before it — Linux draws the same line in
 * acpi_hest_get_size(). */
typedef struct {
    uint8_t  section_type[16];
    uint32_t error_severity;
    uint16_t revision;
    uint8_t  validation_bits;
    uint8_t  flags;
    uint32_t error_data_length;
    uint8_t  fru_id[16];
    char     fru_text[20];
    uint8_t  timestamp[8];
} __attribute__((packed)) GenericErrorDataEntry;
_Static_assert(sizeof(GenericErrorDataEntry) == 72,
               "Generic Error Data Entry = 72 bytes at revision 0x300");
#define GEDATA_HEADER_PRE_V300  64u

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

/* The four section kinds this kernel names, and the Touch tag each one is
 * published under. -1 from section_type_kind is a GUID we do not know. */
static const char* const SECTION_KIND_NAME[4] = {
    "Processor", "Memory", "PCIe", "Generic"
};
static const char* const SECTION_KIND_TAG[4] = {
    "acpi:cper:processor", "acpi:cper:memory",
    "acpi:cper:pcie",      "acpi:cper:generic"
};

static bool guid_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return false;
    return true;
}

static int section_type_kind(const uint8_t* guid) {
    if (guid_eq(guid, SECT_GUID_PROCESSOR)) return 0;
    if (guid_eq(guid, SECT_GUID_MEMORY))    return 1;
    if (guid_eq(guid, SECT_GUID_PCIE))      return 2;
    if (guid_eq(guid, SECT_GUID_GENERIC))   return 3;
    return -1;
}

/* ACPI 6.5 Table 18.11 / Table 18.12 Error Severity. Code 3 is "None" in
 * the ACPI tables and "Informational" in the UEFI CPER wording; it is the
 * same code and this kernel says "none". */
static const char* severity_text(uint32_t sev) {
    switch (sev) {
        case 0: return "recoverable";
        case 1: return "fatal";
        case 2: return "corrected";
        case 3: return "none";
        default: return "unknown";
    }
}

/* Byte length of one Generic Error Data Entry header (Table 18.12). */
static uint32_t gedata_header_len(uint16_t revision) {
    /* Revision is packed major.minor; the Timestamp arrived at 0x300.
     * Linux compares the same way in acpi_hest_get_version(). */
    return ((revision >> 8) >= 3) ? (uint32_t)sizeof(GenericErrorDataEntry)
                                  : GEDATA_HEADER_PRE_V300;
}

/* Decode ONE Generic Error Status Block of the Boot Error Region and say
 * what it holds: a one-line summary of the block, one line per Generic
 * Error Data Entry, and a Touch publish for each so a logger can persist
 * the prior-boot failure before the next one.
 *
 * `blk` is the block; `avail` is how many bytes of the region remain at
 * that point. Returns the block's total length so the caller can step to
 * the next block, or 0 when there is no further record (Block Status 0) or
 * the block does not fit what is left.
 *
 * What stood here read `blk + 20` as a 128-byte CPER record header and
 * demanded a "CPER" signature. No such header exists in the Boot Error
 * Region: the bytes at +20 are the first entry's Section Type GUID (for a
 * memory error, 14 11 BC A5 ...), so the signature test failed on every
 * real machine and not one section was ever decoded — silently, because
 * every print on that path compiles out of a shipped build. */
static uint32_t bert_decode_block(volatile uint8_t* blk, uint32_t avail) {
    if (avail < sizeof(GenericErrorStatusBlock)) return 0;

    volatile GenericErrorStatusBlock* hdr = (volatile GenericErrorStatusBlock*)blk;
    uint32_t status = hdr->block_status;
    if (status == 0) return 0;           /* no more records in the region */

    uint32_t data_length = hdr->data_length;
    uint32_t raw_offset  = hdr->raw_data_offset;
    uint32_t raw_length  = hdr->raw_data_length;

    /* A block ends after its raw data when it carries any, and after its
     * entries otherwise — the same length Linux computes in
     * cper_estatus_len(). */
    uint64_t block_len = raw_length
        ? (uint64_t)raw_offset + raw_length
        : (uint64_t)sizeof(GenericErrorStatusBlock) + data_length;

    if (block_len < sizeof(GenericErrorStatusBlock) || block_len > avail ||
        (uint64_t)sizeof(GenericErrorStatusBlock) + data_length > avail) {
        /* kprintf, not debug_printf: the machine is holding a record of the
         * failure that ended its previous boot and cannot read it. That
         * sentence has to survive into a shipped build. */
        kprintf("[APEI] BERT: a status block claims %lu bytes (%u of entries, "
                "%u of raw data at +%u) with %u left in the region — the "
                "prior-boot record cannot be read\n",
                (unsigned long)block_len, data_length, raw_length,
                raw_offset, avail);
        return 0;
    }

    kprintf("[APEI] BERT: %s error from the previous boot, severity=%s, "
            "%u byte(s) of error data\n",
            (status & 0x1) ? "uncorrectable" :
            (status & 0x2) ? "correctable"   :
            (status & 0x4) ? "multiple uncorrectable" :
            (status & 0x8) ? "multiple correctable"   : "unclassified",
            severity_text(hdr->error_severity), data_length);

    /* Broadcast the block so a userspace logger / telemetry daemon (anyone
     * subscribing to `acpi:boot-error`) can persist it before the next
     * crash. */
    struct { uint32_t status; uint32_t severity; uint32_t data_length; } ev =
        { status, hdr->error_severity, data_length };
    TouchPublish("acpi:boot-error", &ev, sizeof(ev));

    /* Generic Error Data Entries: header (64 or 72 bytes by revision) then
     * Error Data Length bytes of section body, repeated until Data Length
     * is spent. A zero-length body is legal and still advances by a
     * header. */
    volatile uint8_t* p   = blk + sizeof(GenericErrorStatusBlock);
    uint32_t remaining    = data_length;
    uint32_t index        = 0;
    while (remaining >= GEDATA_HEADER_PRE_V300) {
        volatile GenericErrorDataEntry* ge = (volatile GenericErrorDataEntry*)p;
        uint32_t hlen = gedata_header_len(ge->revision);
        uint32_t dlen = ge->error_data_length;
        if (hlen > remaining || (uint64_t)hlen + dlen > remaining) {
            kprintf("[APEI] BERT: error data entry %u claims %u+%u bytes with "
                    "%u left — the rest of this record is unreadable\n",
                    index, hlen, dlen, remaining);
            break;
        }

        uint8_t guid[16];
        for (int j = 0; j < 16; j++) guid[j] = ge->section_type[j];
        int kind = section_type_kind(guid);
        uint32_t sev = ge->error_severity;
        uint32_t body_off = (uint32_t)((uintptr_t)(p + hlen) - (uintptr_t)blk);

        kprintf("[APEI] BERT:   section[%u] %s, severity=%s, %u byte(s) at "
                "+%u of the block\n",
                index, (kind >= 0) ? SECTION_KIND_NAME[kind] : "unrecognised",
                severity_text(sev), dlen, body_off);

        /* Per-section tag broadcast: subscribers can scope their interest
         * (e.g. only memory or only PCIe). */
        if (kind >= 0) {
            struct { uint32_t sev; uint32_t len; uint32_t off; } sect_ev =
                { sev, dlen, body_off };
            TouchPublish(SECTION_KIND_TAG[kind], &sect_ev, sizeof(sect_ev));
        }

        p         += hlen + dlen;
        remaining -= hlen + dlen;
        index++;
    }

    /* The Boot Error Region is a ONE-TIME source: what it holds describes
     * the boot before this one. Clearing Block Status is how OSPM says the
     * record has been consumed — left set, the same prior-boot error is
     * re-announced at every boot from here on and the firmware has no room
     * to record the next one. Linux clears the same field in
     * bert_print_all() for the same reason. */
    hdr->block_status = 0;

    return (uint32_t)block_len;
}

/*
 * APEI (ACPI Platform Error Interfaces) — ACPI 6.5 §18.
 *
 * Three tables, all optional, all detected here:
 *   HEST — Hardware Error Source Table (catalogue of error sources)
 *   BERT — Boot Error Record Table (one-shot record carried across reboot)
 *   ERST — Error Record Serialization Table (persistent error log API)
 *
 * What each one costs this pass is no longer the same, and the comment
 * that stood here — "only records presence + handful of top-level fields
 * so a future RAS subsystem can find the descriptors" — has not been true
 * of two of the three for some time:
 *
 *   HEST is walked entry by entry, and every enabled GHES / GHESv2 source
 *        is handed to the runtime (decode_ghes -> apei_ghes_register_source),
 *        which is what makes an SCI-, NMI- or polled-notification hardware
 *        error reach this kernel at all.
 *   BERT is read, decoded down to its Generic Error Data Entries,
 *        published on Touch and acknowledged (acpi_apei_consume).
 *   ERST is bound only: acpi_erst_bind records where the table is, and no
 *        serialization instruction of it is executed on any boot today.
 *
 * Server boards that expose machine-check details, IPMI alerts, or
 * persistent log regions surface them through these tables.
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
 * This read is the boot-time LOOK — it says where the block is while the
 * HEST is being walked. The register itself, not its value, is what the
 * runtime is handed (apei_ghes_register_source below), and the runtime
 * re-reads it before every walk, so a firmware that republishes the block
 * elsewhere is followed. Linux performs the same read at the same place in
 * __ghes_peek_estatus().
 *
 * Returns false and says so out loud when the register cannot be read;
 * *out_phys is 0 in that case. A register that reads back 0 is not an
 * error here: it means the firmware has published no status block for
 * this source yet. */
/* The register is 8 bytes by definition. Firmware that publishes a bit
 * width of 0 — or any width that is not an access size the CPU has — is
 * honoured as the spec's 8 bytes rather than refused, the same fixup Linux
 * applies in apei_check_gar(). The runtime is handed this width, not the
 * raw one, so both ends read the register the same way. */
static uint8_t ghes_status_register_width(const acpi_gas_t* gas) {
    uint8_t w = gas->bit_width;
    return (w == 8 || w == 16 || w == 32 || w == 64) ? w : 64;
}

static bool ghes_read_status_block_pointer(const acpi_gas_t* gas,
                                           uint16_t source_id,
                                           uint64_t* out_phys) {
    *out_phys = 0;
    if (gas->address == 0) {
        kprintf("[APEI] GHES src=%u: error status address register is null "
                "in HEST\n", source_id);
        return false;
    }

    uint8_t width = ghes_status_register_width(gas);

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
                                        g->source_id, &gesb_phys)) {
        /* kprintf, not debug_printf: an enabled source whose register the
         * kernel cannot read at all is a class of hardware error this
         * machine will never report, and that sentence has to survive into
         * a shipped build. */
        kprintf("[APEI] GHES src=%u is enabled, but its error status address "
                "register (0x%lx) cannot be read — this source cannot report "
                "a hardware error\n",
                g->source_id, (unsigned long)g->error_status_address.address);
        return;
    }

    /* A register that reads 0 right now is NOT a source to throw away: it
     * means the firmware has published no status block yet. The runtime
     * reads the register again before every walk and follows it then. */
    debug_printf("[APEI]   GHES src=%u status block at 0x%lx\n",
                 g->source_id, (unsigned long)gesb_phys);

    (void)apei_ghes_register_source(g->source_id, v2, n->type, n->vector,
                                     n->poll_interval,
                                     g->error_status_address.address,
                                     g->error_status_address.address_space,
                                     ghes_status_register_width(
                                         &g->error_status_address),
                                     gesb_phys,
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

    /* ACPI 6.5 §18.6.1 — the Error Injection Table. Asked for, never walked:
     * injecting an error means writing to registers a GAS names, and this
     * kernel writes to none of them. What is worth knowing is whether the
     * firmware published the table at all, because three comments in this
     * tree name EINJ as the integration test that would prove the MCE and
     * GHES paths, and on a board that does not publish it that test cannot
     * be run by anyone. */
    g_acpi.apei.einj_present = (acpi_find_table("EINJ") != NULL);

    /* ‼ THE SENTENCE THAT SAYS WHETHER THIS MACHINE CAN REPORT A HARDWARE
     * ERROR AT ALL. kprintf, not debug_printf, because the answer is a
     * property of the board and not of this build — and because until now
     * every carrier of it was mute: the runtime's own line was a
     * debug_printf, acpi_print_info's summary is a debug_printf standing
     * behind CONFIG_ACPI_DEBUG, and the capability word published on
     * "acpi:ready" has carried HEST, BERT and ERST in bits 6..8 since the
     * day it was written with no subscriber that ever prints them. The fact
     * was machine-readable and unsaid. */
    if (!g_acpi.apei.hest_present && !g_acpi.apei.bert_present &&
        !g_acpi.apei.erst_present && !g_acpi.apei.einj_present) {
        kprintf("[APEI] %[W]this firmware describes no hardware error source "
                "at all%[D] — no HEST, no BERT, no ERST, no EINJ. Nothing "
                "here arms the platform's error reporting: a corrected "
                "memory error on this machine is handed to nobody, whatever "
                "the previous boot died of was not kept for this one to "
                "read, and the first error that does arrive arrives as a "
                "machine check, after the fact and with no warning ahead of "
                "it\n");
        return;
    }

    kprintf("[APEI] what this firmware says about reporting its own errors: ");
    if (g_acpi.apei.hest_present)
        kprintf("HEST names %u error source(s) this kernel can watch; ",
                g_acpi.apei.hest_error_source_count);
    else
        kprintf("no HEST, so not one error source is described and nothing "
                "will be reported while the machine runs; ");
    if (g_acpi.apei.bert_present)
        kprintf("BERT hands over %u byte(s) holding what the previous boot "
                "died of; ", g_acpi.apei.bert_region_length);
    else
        kprintf("no BERT, so whatever killed the previous boot was not kept "
                "for this one; ");
    if (g_acpi.apei.erst_present)
        kprintf("ERST offers %u instruction(s) for writing an error record "
                "into firmware storage; ",
                g_acpi.apei.erst_instruction_count);
    else
        kprintf("no ERST, so an error seen here cannot be persisted where "
                "the next boot would find it; ");
    if (g_acpi.apei.einj_present)
        kprintf("EINJ is published, so the error injection this kernel names "
                "as its integration test can actually be run on this board\n");
    else
        kprintf("no EINJ, so the error injection this kernel names as its "
                "integration test cannot be run on this board\n");
}

const acpi_apei_info_t *acpi_get_apei(void) {
    if (g_acpi.apei.hest_present || g_acpi.apei.bert_present ||
        g_acpi.apei.erst_present) {
        return &g_acpi.apei;
    }
    return NULL;
}

/* Read the Boot Error Region (if present) and say what the previous boot
 * left in it. The region holds a sequence of Generic Error Status Blocks
 * (ACPI 6.5 §18.3.1) and ends at the first block whose Block Status is
 * zero; each block is decoded, published and then acknowledged by clearing
 * that field. */
void acpi_apei_consume(void) {
    if (!g_acpi.apei.bert_present) return;
    if (g_acpi.apei.bert_region_length < sizeof(GenericErrorStatusBlock)) {
        debug_printf("[APEI] BERT region length %u below header size\n",
                     g_acpi.apei.bert_region_length);
        return;
    }
    volatile uint8_t* region = (volatile uint8_t*)
        vmm_map_mmio((uintptr_t)g_acpi.apei.bert_region_address,
                      g_acpi.apei.bert_region_length,
                      VMM_FLAGS_KERNEL_RW);
    if (!region) {
        kprintf("[APEI] BERT: region 0x%lx (%u bytes) cannot be mapped — "
                "whatever ended the previous boot cannot be read\n",
                (unsigned long)g_acpi.apei.bert_region_address,
                g_acpi.apei.bert_region_length);
        return;
    }

    volatile uint8_t* p = region;
    uint32_t remaining  = g_acpi.apei.bert_region_length;
    uint32_t blocks     = 0;
    while (remaining >= sizeof(GenericErrorStatusBlock)) {
        uint32_t len = bert_decode_block(p, remaining);
        if (len == 0) break;
        p         += len;
        remaining -= len;
        blocks++;
    }
    if (blocks == 0) {
        debug_printf("[APEI] BERT: no error recorded from previous boot\n");
    }

    /* The Boot Error Region is read ONCE, here, and never again: ACPI 6.5
     * §18.3 has the firmware fill it before OSPM runs, and the blocks are
     * acknowledged above by clearing their Block Status. Keeping the window
     * open for the life of the kernel — which is what this used to do —
     * spends kernel virtual address space and the page-table pages under it
     * on a region nothing will ever look at again, and leaves a writable
     * mapping of firmware memory standing for no reason. */
    vmm_unmap_mmio(region, g_acpi.apei.bert_region_length);

    if (g_acpi.apei.hest_present) {
        debug_printf("[APEI] HEST exposes %u error sources for runtime dispatch\n",
                     g_acpi.apei.hest_error_source_count);
    }
    if (g_acpi.apei.erst_present) {
        debug_printf("[APEI] ERST exposes %u serialization instructions\n",
                     g_acpi.apei.erst_instruction_count);
    }
}
