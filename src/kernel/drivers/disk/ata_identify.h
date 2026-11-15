#ifndef ATA_IDENTIFY_H
#define ATA_IDENTIFY_H

#include "ktypes.h"

/*
 * Where an ATA drive's physical blocks begin, out of IDENTIFY DEVICE — the
 * one reading the AHCI and the ATA drivers share (ahci.c, ata.c), so the two
 * cannot come to say it two ways.
 *
 * Word 209 (ACS-3 §7.12.7.67, "Logical sector offset"): bits 15:14 = 01b mark
 * it valid, and bits 13:0 are how far into the FIRST physical block logical
 * sector 0 lies. That is not the first aligned sector, and taking it for one
 * gets the grid backwards: a drive whose sector 0 lies one sector into its
 * first 4096-byte block (the old "XP jumper", word 209 = 0x4001) begins its
 * next block at sector 8 - 1 = 7. So it is turned here, as Linux turns it
 * (ata_id_logical_sector_offset): with P/L logical sectors to a physical
 * block, the first sector that begins one is (P/L - offset) when the offset is
 * not 0, and 0 when it is — the L0 a volume's ground is held to:
 * `(ground - L0) mod (atom / 512) == 0`.
 *
 * `logical` and `physical` are the sizes word 106 already gave, in bytes.
 * Answered in logical sectors, which on these two drivers are the room's
 * 512-byte ones (both refuse any other, ahci.c and ata.c). 0 when the word is
 * not valid, when the drive is built of blocks no larger than its sectors,
 * and when the offset it states lies outside the block it says it is made of
 * — a drive that does not say where its blocks begin is taken to begin them
 * at sector 0, which is what the standard makes of a word left unset.
 */
static inline uint32_t AtaIdentifyFirstAligned(const uint16_t *id, uint32_t logical,
                                               uint32_t physical)
{
    uint32_t per = (logical && physical > logical) ? physical / logical : 1u;
    uint16_t word = id[209];
    if ((word & 0xC000u) != 0x4000u || per <= 1u)
        return 0;
    uint32_t offset = word & 0x3FFFu;
    if (offset == 0 || offset >= per)
        return 0;
    return per - offset;
}

#endif /* ATA_IDENTIFY_H */
