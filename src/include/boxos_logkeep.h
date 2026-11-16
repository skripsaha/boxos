#ifndef BOXOS_LOGKEEP_H
#define BOXOS_LOGKEEP_H

/*
 * boxos_logkeep.h — the carry-over window: where the kernel keeps what it
 * says so that the start after a reset can read it, and how it is laid out.
 *
 * The kernel writes it (klib_logring.c, LogKeepInit and on — what it survives
 * and what it does not is said there); the utility lastsaid reads the bank of
 * the start before through the kernel; Gangway's panel reads it straight from
 * memory before the kernel is placed (gangway_panel_log.c), with nothing
 * written. One layout, the kernel's and the loader's both.
 *
 * A header page, then two banks. The start that is running writes bank
 * `current`, and the header the next start finds still names it: that bank is
 * the previous start's, `bytes[current]` said into it, the last
 * LOGKEEP_BANK_BYTES of them kept, byte `p` at `p % LOGKEEP_BANK_BYTES`.
 * `size` is the header's own length and `capacity` a bank's — a header left
 * by a kernel of another layout says others, and is not taken.
 *
 * Includer must provide uint32_t and uint64_t BEFORE including this header.
 */

/* 64 MiB. Above anything either loader places (the kernel may be 32 MiB and
 * its page tables and boot stack sit just past it, ~33 MiB at the very most),
 * below the first address any machine BoxOS targets fails to have. Fixed
 * rather than allocated because the whole point is that the NEXT start finds
 * it, and the next start's allocator has not run yet. */
#define LOGKEEP_PHYS        0x04000000ULL
#define LOGKEEP_HEADER      4096ULL
#define LOGKEEP_BANKS       2u
#define LOGKEEP_BANK_BYTES  (1024u * 1024u)
#define LOGKEEP_BYTES       (LOGKEEP_HEADER + (uint64_t)LOGKEEP_BANKS * LOGKEEP_BANK_BYTES)

#define LOGKEEP_MAGIC       0x4449415353584F42ULL   /* "BOXSAID" + NUL, little-end */

typedef struct {
    uint64_t magic;
    uint32_t size;         /* sizeof(LogKeepHeader) */
    uint32_t capacity;     /* per bank: LOGKEEP_BANK_BYTES */
    uint32_t current;      /* the bank the start that wrote it wrote */
    uint32_t boot;         /* counts up; names which start a bank came from */
    uint64_t bytes[LOGKEEP_BANKS];
} LogKeepHeader;

_Static_assert(sizeof(LogKeepHeader) <= LOGKEEP_HEADER, "the carry-over header must fit its page");

#endif /* BOXOS_LOGKEEP_H */
