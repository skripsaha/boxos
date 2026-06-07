#ifndef BOXOS_DECKS_H
#define BOXOS_DECKS_H

/* ============================================================================
 *  Deck identifiers — top byte of every (deck, op) syscall code.
 * ============================================================================ */
#define DECK_EXECUTION     0x00
#define DECK_OPERATIONS    0x01
#define DECK_STORAGE       0x02
#define DECK_HARDWARE      0x03
#define DECK_NETWORK       0x04
#define DECK_SYSTEM        0xFF

/* ============================================================================
 *  System deck opcodes — shared between kernel handler tables and boxlib
 *  syscall wrappers. SINGLE SOURCE OF TRUTH; never mirror these defines in
 *  any *.c file. Add new ops here and include this header on both sides.
 *
 *  The kernel side picks up these defines through
 *    src/kernel/core/decks/system/system_deck.h
 *      -> include "boxos_decks.h"
 *  and the userspace side through
 *    src/userspace/boxlib/src/X.c
 *      -> include "boxos_decks.h".
 *
 *  Each section starts with the byte range it occupies so adding a new op
 *  is a one-line edit that can't collide with an existing range.
 * ============================================================================ */

/* ─── MemTag (0xA0 – 0xAF) ─────────────────────────────────────────── */

/* Phase 1 — read-only surface. */
#define SYSTEM_OP_MEMTAG_QUERY      0xA0  /* required+any+excluded → region_ids */
#define SYSTEM_OP_MEMTAG_INFO       0xA1  /* region_id → {base, pages, tags...} */
#define SYSTEM_OP_MEMTAG_LOOKUP     0xA2  /* phys_addr → region_id              */
#define SYSTEM_OP_MEMTAG_TAGS       0xA3  /* region_id → list of tag strings    */
#define SYSTEM_OP_MEMTAG_STATS      0xA4  /* global MemTagStats snapshot        */

/* Phase 2A — enforcement infrastructure. SET_GUARD/GRANT/REVOKE gated on
 * the caller holding the "system" tag-bit (TagFS auth); CABIN_TAGS / CHECK
 * are unprivileged observation. */
#define SYSTEM_OP_MEMTAG_SET_GUARD  0xA5  /* tag_str + u8 on/off                */
#define SYSTEM_OP_MEMTAG_GRANT      0xA6  /* (u32 pid)(tag_str) → grant cap     */
#define SYSTEM_OP_MEMTAG_REVOKE     0xA7  /* (u32 pid)(tag_str) → revoke cap    */
#define SYSTEM_OP_MEMTAG_CABIN_TAGS 0xA8  /* u32 pid → tag_str list             */
#define SYSTEM_OP_MEMTAG_CHECK      0xA9  /* (u32 pid)(u32 region_id) → bool    */

/* Phase 2H+ — tag-driven PKU PTE stamping. APPLY_PKEY adds `pku:N` to the
 * region (replacing any prior pku:* tag), auto-stamping PTE bits 62:59
 * across every attach + cross-core TLB shootdown. Userspace WRPKRU then
 * gates per-key access. Unprivileged: callers may stamp any region they
 * know the region_id of — security comes from cabin's grant-set on the
 * region's other tags. */
#define SYSTEM_OP_MEMTAG_APPLY_PKEY 0xAA  /* (u32 region_id)(u8 pkey)           */

#endif // BOXOS_DECKS_H
