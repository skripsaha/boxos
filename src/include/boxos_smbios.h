#ifndef BOXOS_SMBIOS_H
#define BOXOS_SMBIOS_H

/*
 * SMBIOS's entry points (DMTF DSP0134 3.8.0 §5.2), read by one owner for the
 * two sides that meet them: Gangway, which copies the structure table out of
 * memory the firmware may give back once its boot services end (UEFI 2.10
 * §2.3 lets the table lie in EfiBootServicesData), and the kernel, which on a
 * BIOS start finds the entry point in the F-segment itself (§5.2.1, §5.2.2).
 * Integer types come from the includer, as fat.h's do; nothing here asks for
 * a library.
 *
 * The 64-bit entry point ("_SM3_") is preferred where both are published: the
 * 32-bit one's table is a consistent subset of it (§5.2).
 */

#define SMBIOS_SCAN_FIRST   0xF0000u      /* a BIOS's entry point lies on a paragraph in here */
#define SMBIOS_SCAN_END     0x100000u
#define SMBIOS_SCAN_STEP    16u
#define SMBIOS_TABLE_MOST   (1u << 20)    /* a structure table longer is no machine's */
#define SMBIOS_FROM_2       1u            /* boxos_system_info.h MACHINE_FROM_SMBIOS2 */
#define SMBIOS_FROM_3       2u            /* MACHINE_FROM_SMBIOS3 */

typedef struct {
    uint64_t table;     /* the structure table's physical address */
    uint32_t bytes;     /* its length (32-bit) or most (64-bit: "less or equal") */
    uint8_t  major;
    uint8_t  minor;
    uint8_t  from;      /* SMBIOS_FROM_* */
} SmbiosEntry;

static inline uint32_t SmbiosLe(const uint8_t* p, uint32_t bytes)
{
    uint32_t v = 0;
    for (uint32_t i = bytes; i-- > 0;)
        v = v << 8 | p[i];
    return v;
}

static inline int SmbiosSums(const uint8_t* p, uint32_t bytes)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < bytes; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Whether `room` bytes at `p` begin an entry point that holds — its anchor,
 * its length, its checksums — and what it says in *e. The versions firmware
 * is known to misreport are put right as Linux puts them (dmi_scan.c): 2.31
 * and 2.33 are 2.3, 2.51 is 2.6. */
static inline int SmbiosEntryRead(const uint8_t* p, uint32_t room, SmbiosEntry* e)
{
    if (room >= 24u && p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '3' && p[4] == '_') {
        uint32_t len = p[6];
        if (len < 24u || len > 32u || len > room || !SmbiosSums(p, len))
            return 0;
        e->major = p[7];
        e->minor = p[8];
        e->bytes = SmbiosLe(p + 0x0C, 4);
        e->table = (uint64_t)SmbiosLe(p + 0x10, 4) | (uint64_t)SmbiosLe(p + 0x14, 4) << 32;
        e->from  = SMBIOS_FROM_3;
        return e->table && e->bytes;
    }
    if (room >= 31u && p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_') {
        uint32_t len = p[5];
        if (len < 0x1Eu || len > 0x20u || len > room || !SmbiosSums(p, len))
            return 0;
        if (p[0x10] != '_' || p[0x11] != 'D' || p[0x12] != 'M' || p[0x13] != 'I' || p[0x14] != '_' ||
            !SmbiosSums(p + 0x10, 0x0F))
            return 0;
        e->major = p[6];
        e->minor = p[7];
        if (e->major == 2 && (e->minor == 0x1F || e->minor == 0x21))
            e->minor = 3;
        else if (e->major == 2 && e->minor == 0x33)
            e->minor = 6;
        e->bytes = SmbiosLe(p + 0x16, 2);
        e->table = SmbiosLe(p + 0x18, 4);
        e->from  = SMBIOS_FROM_2;
        return e->table && e->bytes;
    }
    return 0;
}

#endif /* BOXOS_SMBIOS_H */
