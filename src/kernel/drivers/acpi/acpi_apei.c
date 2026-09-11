#include "acpi_internal.h"
#include "apei_ghes_runtime.h"
#include "klib.h"
#include "vmm.h"
#include "touch.h"

typedef struct {
    uint32_t block_status;
    uint32_t raw_data_offset;
    uint32_t raw_data_length;
    uint32_t data_length;
    uint32_t error_severity;
} __attribute__((packed)) acpi_bert_region_t;

typedef struct {
    char     signature[4];
    uint16_t revision;
    uint32_t signature_end;
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

static void cper_decode_region(volatile uint8_t* region,
                                uint32_t region_len,
                                uint32_t data_length) {
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


#define HEST_TYPE_IA32_MCE         0
#define HEST_TYPE_IA32_CMCI        1
#define HEST_TYPE_IA32_NMI         2
#define HEST_TYPE_AER_ROOT_PORT    6
#define HEST_TYPE_AER_ENDPOINT     7
#define HEST_TYPE_AER_BRIDGE       8
#define HEST_TYPE_GHES             9
#define HEST_TYPE_GHESV2           10
#define HEST_TYPE_IA32_DMC         11

typedef struct {
    uint8_t  type;
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

static void decode_ghes(uint8_t* entry, bool v2) {
    uint16_t source_id  = *(uint16_t*)(entry + 2);
    uint8_t  enabled    = entry[15];
    uint32_t records    = *(uint32_t*)(entry + 16);
    uint32_t max_sect   = *(uint32_t*)(entry + 20);
    ghes_notification_t* n = (ghes_notification_t*)(entry + 40);
    uint64_t gesb_addr = *(uint64_t*)(entry + 28 + 4);
    uint32_t gesb_len  = *(uint32_t*)(entry + 68);
    uint64_t ack_addr = 0, ack_preserve = 0, ack_write = 0;
    if (v2) {
        ack_addr     = *(uint64_t*)(entry + 72 + 4);
        ack_preserve = *(uint64_t*)(entry + 96);
        ack_write    = *(uint64_t*)(entry + 104);
    }
    debug_printf("[APEI]   GHES%s src=%u enabled=%u records=%u sections=%u "
                 "notify=%s vector=%u poll=%u gesb=0x%lx len=%u\n",
                 v2 ? "v2" : "", source_id, enabled, records, max_sect,
                 ghes_notify_name(n->type), n->vector, n->poll_interval,
                 (unsigned long)gesb_addr, gesb_len);

    if (enabled) {
        (void)apei_ghes_register_source(source_id, v2, n->type, n->vector,
                                         n->poll_interval, gesb_addr,
                                         gesb_len, ack_addr, ack_preserve,
                                         ack_write);
    }
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

static uint16_t hest_entry_len(uint16_t t) {
    switch (t) {
        case HEST_TYPE_IA32_MCE:      return 264;
        case HEST_TYPE_IA32_CMCI:     return 60;
        case HEST_TYPE_IA32_NMI:      return 24;
        case HEST_TYPE_AER_ROOT_PORT: return 80;
        case HEST_TYPE_AER_ENDPOINT:  return 80;
        case HEST_TYPE_AER_BRIDGE:    return 88;
        case HEST_TYPE_GHES:          return 92;
        case HEST_TYPE_GHESV2:        return 116;
        case HEST_TYPE_IA32_DMC:      return 60;
        default:                       return 0;
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

        uint8_t* ptr = (uint8_t*)hest + sizeof(acpi_hest_t);
        uint8_t* end = (uint8_t*)hest + hest->header.length;
        uint32_t walked = 0;
        while (ptr + 4 <= end && walked < hest->error_source_count) {
            uint16_t type = *(uint16_t*)ptr;
            uint16_t srcid = *(uint16_t*)(ptr + 2);
            uint16_t step = hest_entry_len(type);
            if (step == 0 || ptr + step > end) {
                debug_printf("[ACPI] HEST: unknown type %u at off %lu, stopping\n",
                             type, (unsigned long)(ptr - (uint8_t*)hest));
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
        uint8_t* base = (uint8_t*)erst + sizeof(acpi_erst_t);
        uint32_t entry_size = 32;
        for (uint32_t i = 0; i < erst->instruction_entry_count; i++) {
            uint8_t* e = base + i * entry_size;
            if (e + entry_size > (uint8_t*)erst + erst->header.length) break;
            uint8_t action = e[0];
            uint8_t instr  = e[1];
            uint8_t flags  = e[2];
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

    struct { uint32_t status; uint32_t severity; uint32_t data_length; } ev =
        { status, rg->error_severity, rg->data_length };
    TouchPublish("acpi:boot-error", &ev, sizeof(ev));

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