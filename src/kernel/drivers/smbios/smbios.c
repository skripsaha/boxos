#include "smbios.h"
#include "boxos_smbios.h"
#include "boot_info.h"
#include "vmm.h"
#include "klib.h"

static MachineInfo g_machine;

/* SMBIOS structure types (DSP0134 §7) and the offsets read of each. */
#define SMBIOS_SYSTEM          1u
#define SMBIOS_BASEBOARD       2u
#define SMBIOS_ENCLOSURE       3u
#define SMBIOS_END             127u
#define SMBIOS_HEADER_BYTES    4u

/*
 * The values vendors leave in a field they never filled — systemd's
 * hwdb.d/20-dmi-id.hwdb, Linux's ASoC list and linux-hardware.org's hw-probe
 * together — compared with the case folded and the spaces taken out, as
 * systemd's modalias compares them. None names a machine.
 */
static const char* const g_placeholders[] = {
    "defaultstring", "n/a", "o.e.m.", "oem", "systemmanufacturer", "systemproductname", "systemname",
    "systemversion", "tobefilledbyo.e.m.", "tobefilledbyoem", "tbdbyoem", "boardmanufacturer",
    "boardvendorname", "boardproductname", "baseboardmanufacturer", "chassismanufacturer", "chassisversion",
    "notspecified", "notapplicable", "notavailable", "none", "unknown", "invalid", "null", "default",
    "x.x", "0123456789", "1234567890", "systemskunumber", "type1productconfigid",
};

/* Whether `s` is a placeholder: one of the values above, or made of nothing
 * but zeros, O's, X's, dashes, dots and underscores. */
static bool SmbiosPlaceholder(const char* s)
{
    char   folded[MACHINE_TEXT_MOST];
    size_t n = 0;
    bool   filler = true;
    for (; *s && n + 1 < sizeof(folded); s++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        filler = filler && strchr("0ox-._", c) != NULL;
        if (c != ' ')
            folded[n++] = c;
    }
    folded[n] = 0;
    if (!n || filler)
        return true;
    for (size_t i = 0; i < sizeof(g_placeholders) / sizeof(g_placeholders[0]); i++)
        if (strcmp(folded, g_placeholders[i]) == 0)
            return true;
    return false;
}

/*
 * String `index` (1-based; 0 is none, §6.1.3) of the structure whose strings
 * begin at `strings` and end before `end`, into `out`: no byte below a space
 * or DEL, the spaces round it trimmed, cut on a whole UTF-8 character — and
 * empty for one past the set, or a vendor's placeholder.
 */
static void SmbiosString(const uint8_t* strings, const uint8_t* end, uint8_t index, char out[MACHINE_TEXT_MOST])
{
    out[0] = 0;
    if (!index)
        return;
    const uint8_t* s = strings;
    for (uint8_t i = 1; i < index && s < end && *s; i++) {
        while (s < end && *s)
            s++;
        s++;
    }
    if (s >= end || !*s)
        return;
    size_t n = 0;
    for (; s < end && *s && n + 1 < MACHINE_TEXT_MOST; s++)
        if (*s >= 0x20 && *s != 0x7F && (n || *s != ' '))
            out[n++] = (char)*s;
    if (s < end && (*s & 0xC0u) == 0x80u) {
        while (n && ((uint8_t)out[n - 1] & 0xC0u) == 0x80u)
            n--;
        if (n)
            n--;    /* the lead byte of the character cut */
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    if (SmbiosPlaceholder(out))
        out[0] = 0;
}

/* The table's structures walked to its end or the End-of-Table (§6.1.2):
 * the first of types 1, 2 and 3 read into g_machine. A structure whose length
 * is short of its header, or whose strings run past the table, ends the walk. */
static void SmbiosWalk(const uint8_t* t, uint32_t bytes)
{
    bool     system = false, board = false, enclosure = false;
    uint32_t at     = 0;
    while (at + SMBIOS_HEADER_BYTES <= bytes) {
        uint8_t        type = t[at], len = t[at + 1];
        const uint8_t* s    = t + at + len;
        const uint8_t* end  = t + bytes;
        if (len < SMBIOS_HEADER_BYTES || at + len > bytes || type == SMBIOS_END)
            break;
        const uint8_t* next = s;
        while (next + 1 < end && (next[0] || next[1]))
            next++;
        if (next + 1 >= end)
            break;
        const uint8_t* f = t + at;
        if (type == SMBIOS_SYSTEM && !system && len >= 8u) {
            SmbiosString(s, next + 1, f[4], g_machine.maker);
            SmbiosString(s, next + 1, f[5], g_machine.product);
            SmbiosString(s, next + 1, f[6], g_machine.version);
            if (len > 0x1Au)
                SmbiosString(s, next + 1, f[0x1A], g_machine.family);
            system = true;
        } else if (type == SMBIOS_BASEBOARD && !board && len >= 6u) {
            SmbiosString(s, next + 1, f[4], g_machine.board_maker);
            SmbiosString(s, next + 1, f[5], g_machine.board);
            board = true;
        } else if (type == SMBIOS_ENCLOSURE && !enclosure && len > 5u) {
            g_machine.chassis = f[5] & 0x7Fu;
            enclosure         = true;
        }
        at = (uint32_t)(next + 2 - t);
    }
}

/* A BIOS's entry point, searched for on the F-segment's paragraphs — the
 * 64-bit one throughout first, then the 32-bit one. */
static bool SmbiosEntryOfBios(SmbiosEntry* e)
{
    const uint8_t* f = (const uint8_t*)vmm_map_mmio(SMBIOS_SCAN_FIRST, SMBIOS_SCAN_END - SMBIOS_SCAN_FIRST,
                                                    VMM_FLAGS_KERNEL_RW);
    if (!f)
        return false;
    uint32_t span = SMBIOS_SCAN_END - SMBIOS_SCAN_FIRST;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t at = 0; at + 32u <= span; at += SMBIOS_SCAN_STEP)
            if (f[at] == '_' && f[at + 3] == (pass == 0 ? '3' : '_') && SmbiosEntryRead(f + at, span - at, e))
                return true;
    return false;
}

void SmbiosInit(void)
{
    boot_info_t* bi = boot_info_get();
    SmbiosEntry  e;
    memset(&e, 0, sizeof(e));
    bool found = false;
    if (boot_info_valid(bi) && bi->boot_method == BOOT_METHOD_UEFI) {
        found   = bi->smbios_copy_phys && bi->smbios_copy_size;
        e.table = bi->smbios_copy_phys;
        e.bytes = bi->smbios_copy_size;
        e.major = bi->smbios_major;
        e.minor = bi->smbios_minor;
        e.from  = bi->smbios_from;
    } else {
        found = SmbiosEntryOfBios(&e);
    }
    if (!found || e.bytes > SMBIOS_TABLE_MOST) {
        kprintf("[SMBIOS] the firmware publishes no SMBIOS table — the machine does not say what it is\n");
        return;
    }
    const uint8_t* t = (const uint8_t*)vmm_map_mmio((uintptr_t)e.table, e.bytes, VMM_FLAGS_KERNEL_RW);
    if (!t) {
        kprintf("[SMBIOS] the structure table at 0x%llx would not be mapped\n", (unsigned long long)e.table);
        return;
    }
    SmbiosWalk(t, e.bytes);
    g_machine.from  = e.from;
    g_machine.major = e.major;
    g_machine.minor = e.minor;
    kprintf("[SMBIOS] %u.%u: the machine calls itself \"%s\" \"%s\" (version \"%s\", family \"%s\"), its board "
            "\"%s\" \"%s\", its enclosure of type %u\n", e.major, e.minor, g_machine.maker, g_machine.product,
            g_machine.version, g_machine.family, g_machine.board_maker, g_machine.board, g_machine.chassis);
}

void SmbiosMachine(MachineInfo* out)
{
    *out = g_machine;
}
