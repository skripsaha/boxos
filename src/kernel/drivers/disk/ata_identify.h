#ifndef ATA_IDENTIFY_H
#define ATA_IDENTIFY_H

#include "ktypes.h"

/*
 * What an ATA drive is addressed in and built from, and where its physical
 * blocks begin, out of IDENTIFY DEVICE — the one reading the AHCI and the ATA
 * drivers share (ahci.c, ata.c), so the two cannot come to say it two ways.
 */

/* Word 106 says something (ATA8-ACS §7.16.7.74): bit 14 set, bit 15 clear. */
static inline bool AtaIdentifySizesValid(const uint16_t *id)
{
    return (id[106] & (1u << 14)) && !(id[106] & (1u << 15));
}

/* The logical block, in bytes: words 117-118 give it in 16-bit words when
 * word 106 bit 12 says it is larger than 256 of them (§7.16.7.75); 512
 * otherwise — 4096 on a 4Kn drive. A value no drive could mean, fewer than
 * 256 words, is taken for the 512 the standard falls back to. */
static inline uint32_t AtaIdentifyLogicalBytes(const uint16_t *id)
{
    uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
    return AtaIdentifySizesValid(id) && (id[106] & (1u << 12)) && words >= 256u && words <= 0x7FFFFFFFu
               ? words * 2u : 512u;
}

/* The physical block, in bytes: word 106 bit 13 marks more than one logical
 * block a physical one, and bits 3:0 the exponent — 2^N of them. This is how
 * a 512e drive says "addressed in 512 and built from 4096". */
static inline uint32_t AtaIdentifyPhysicalBytes(const uint16_t *id, uint32_t logical)
{
    uint32_t exponent = id[106] & 0x0Fu;
    if (!AtaIdentifySizesValid(id) || !(id[106] & (1u << 13)) || exponent >= 16 ||
        logical > (0xFFFFFFFFu >> exponent))
        return logical;
    return logical << exponent;
}

/*
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
 * Answered in the room's 512-byte sectors, whatever the drive's logical
 * block. 0 when the word is not valid, when the drive is built of blocks no
 * larger than its logical ones, and when the offset it states lies outside
 * the block it says it is made of — a drive that does not say where its
 * blocks begin is taken to begin them at sector 0, which is what the
 * standard makes of a word left unset.
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
    return (per - offset) * (logical / 512u);
}

#endif /* ATA_IDENTIFY_H */
