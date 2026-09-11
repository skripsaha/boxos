
#include "efi.h"
#include "efi_esrt.h"
#include "boot_info.h"
#include "vmm.h"
#include "klib.h"
#include "touch.h"

static const EfiSystemResourceTable *g_esrt_table = NULL;
static const EfiSystemResourceEntry *g_esrt_entries = NULL;
static uint32_t g_esrt_count = 0;
static bool     g_esrt_ready = false;

static void guid_to_string(const EfiGuid *g, char out[37])
{
    static const char hex[] = "0123456789abcdef";
    if (!g || !out) {
        if (out) out[0] = 0;
        return;
    }

    uint32_t d1 = g->data1;
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[d1 & 0xF];
        d1 >>= 4;
    }
    out[8] = '-';

    uint16_t d2 = g->data2;
    for (int i = 12; i >= 9; i--) {
        out[i] = hex[d2 & 0xF];
        d2 >>= 4;
    }
    out[13] = '-';

    uint16_t d3 = g->data3;
    for (int i = 17; i >= 14; i--) {
        out[i] = hex[d3 & 0xF];
        d3 >>= 4;
    }
    out[18] = '-';

    out[19] = hex[(g->data4[0] >> 4) & 0xF];
    out[20] = hex[ g->data4[0]       & 0xF];
    out[21] = hex[(g->data4[1] >> 4) & 0xF];
    out[22] = hex[ g->data4[1]       & 0xF];
    out[23] = '-';

    for (int i = 2; i < 8; i++) {
        out[24 + (i - 2) * 2]     = hex[(g->data4[i] >> 4) & 0xF];
        out[24 + (i - 2) * 2 + 1] = hex[ g->data4[i]       & 0xF];
    }
    out[36] = 0;
}

bool efi_esrt_init(void)
{
    if (g_esrt_ready) return true;

    boot_info_t *bi = boot_info_get();
    if (!bi || !boot_info_valid(bi) || bi->version != BOOT_INFO_VERSION4) {
        return false;
    }
    if (bi->efi_esrt_copy_phys == 0 || bi->efi_esrt_copy_size == 0) {
        debug_printf("[ESRT] no ESRT published by firmware\n");
        return false;
    }
    if (bi->efi_esrt_copy_size < sizeof(EfiSystemResourceTable)) {
        debug_printf("[ESRT] copy size %u smaller than header\n",
                     bi->efi_esrt_copy_size);
        return false;
    }

    const EfiSystemResourceTable *t = (const EfiSystemResourceTable *)
        vmm_phys_to_virt((uintptr_t)bi->efi_esrt_copy_phys);
    if (!t) {
        debug_printf("[ESRT] phys_to_virt failed for 0x%lx\n",
                     (unsigned long)bi->efi_esrt_copy_phys);
        return false;
    }

    if (t->fw_resource_version != ESRT_FW_RESOURCE_VERSION_V1) {
        debug_printf("[ESRT] unknown FwResourceVersion=%lu\n",
                     (unsigned long)t->fw_resource_version);
        return false;
    }

    uint64_t entries_bytes = (uint64_t)t->fw_resource_count *
                              sizeof(EfiSystemResourceEntry);
    if (entries_bytes + sizeof(EfiSystemResourceTable) > bi->efi_esrt_copy_size) {
        debug_printf("[ESRT] count %u exceeds copy size %u\n",
                     t->fw_resource_count, bi->efi_esrt_copy_size);
        return false;
    }

    g_esrt_table   = t;
    g_esrt_entries = (const EfiSystemResourceEntry *)(t + 1);
    g_esrt_count   = t->fw_resource_count;
    g_esrt_ready   = true;

    debug_printf("[ESRT] ready: %u entries (max=%u, version=1)\n",
                 g_esrt_count, t->fw_resource_count_max);
    return true;
}

bool efi_esrt_available(void) { return g_esrt_ready; }
uint32_t efi_esrt_count(void) { return g_esrt_ready ? g_esrt_count : 0; }

const EfiSystemResourceEntry *efi_esrt_get(uint32_t idx)
{
    if (!g_esrt_ready || idx >= g_esrt_count) return NULL;
    return &g_esrt_entries[idx];
}

const EfiSystemResourceEntry *efi_esrt_find(const EfiGuid *target)
{
    if (!g_esrt_ready || !target) return NULL;
    for (uint32_t i = 0; i < g_esrt_count; i++) {
        EfiGuid fc;
        memcpy(&fc, &g_esrt_entries[i].fw_class, sizeof(fc));
        if (efi_guid_equal(&fc, target)) {
            return &g_esrt_entries[i];
        }
    }
    return NULL;
}

static const char *fw_type_name(uint32_t t)
{
    switch (t) {
        case ESRT_FW_TYPE_SYSTEM_FIRMWARE: return "system";
        case ESRT_FW_TYPE_DEVICE_FIRMWARE: return "device";
        case ESRT_FW_TYPE_UEFI_DRIVER:     return "uefi-driver";
        default:                            return "unknown";
    }
}

void efi_esrt_print(void)
{
    if (!g_esrt_ready) {
        debug_printf("[ESRT] not available\n");
        return;
    }
    debug_printf("[ESRT] %u firmware resource(s):\n", g_esrt_count);
    for (uint32_t i = 0; i < g_esrt_count; i++) {
        const EfiSystemResourceEntry *e = &g_esrt_entries[i];
        EfiGuid fc;
        memcpy(&fc, &e->fw_class, sizeof(fc));
        char gstr[37];
        guid_to_string(&fc, gstr);
        debug_printf("  [%u] %s class=%s fwver=0x%x lowest=0x%x "
                     "caps=0x%x last_ver=0x%x last_status=%u\n",
                     i, fw_type_name(e->fw_type), gstr,
                     e->fw_version, e->lowest_supported_fw_version,
                     e->capsule_flags, e->last_attempt_version,
                     e->last_attempt_status);
    }
}

typedef struct {
    uint32_t                index;
    uint32_t                _pad;
    EfiSystemResourceEntry  entry;
} __attribute__((packed)) EsrtTouchPayload;

void efi_esrt_publish_touch(void)
{
    if (!g_esrt_ready) return;

    struct { uint32_t count; uint32_t max; uint64_t version; } ready_ev = {
        .count   = g_esrt_count,
        .max     = g_esrt_table->fw_resource_count_max,
        .version = g_esrt_table->fw_resource_version,
    };
    TouchPublish("esrt:ready", &ready_ev, sizeof(ready_ev));

    for (uint32_t i = 0; i < g_esrt_count; i++) {
        const EfiSystemResourceEntry *e = &g_esrt_entries[i];
        EfiGuid fc;
        memcpy(&fc, &e->fw_class, sizeof(fc));
        char gstr[37];
        guid_to_string(&fc, gstr);

        char tag[80];
        ksnprintf(tag, sizeof(tag), "esrt:fwclass:%s:0x%x",
                  gstr, (unsigned)e->fw_version);

        EsrtTouchPayload p = {
            .index = i,
            ._pad  = 0,
            .entry = *e,
        };
        TouchPublish(tag, &p, sizeof(p));
    }
}