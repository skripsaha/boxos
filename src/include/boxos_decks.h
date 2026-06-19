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
 *  System deck opcodes — SINGLE SOURCE OF TRUTH for kernel handler tables
 *  and boxlib syscall wrappers.
 *
 *  NEVER mirror these defines in any *.c file. Add a new op here and include
 *  this header on both sides:
 *    kernel:    src/kernel/core/decks/system/system_deck.h includes it
 *    userspace: src/userspace/boxlib/src/X.c includes it
 *
 *  Sections are grouped by byte range so a new op is a one-line edit in the
 *  right block and can't collide silently with an existing range.
 * ============================================================================ */

/* ─── Process + info (0x01 – 0x0A) ─────────────────────────────────── */
#define SYSTEM_OP_PROC_SPAWN        0x01
#define SYSTEM_OP_PROC_KILL         0x02
#define SYSTEM_OP_PROC_INFO         0x03
#define SYSTEM_OP_CTX_USE           0x04
#define SYSTEM_OP_PROC_EXEC         0x06
#define SYSTEM_OP_INFO              0x07
#define SYSTEM_OP_TLS_FSBASE        0x0A  /* set caller's FS base (C++ TLS) */

/* ─── Buffer registry (0x10 – 0x14) ────────────────────────────────── */
#define SYSTEM_OP_BUF_ALLOC         0x10
#define SYSTEM_OP_BUF_FREE          0x11
#define SYSTEM_OP_BUF_RESIZE        0x12
/* Implicit huge-page prefault — boxlib memory.c calls this when growing
 * the user heap by >= 2 MB so the kernel pre-backs the new VA range with
 * 2 MB pages instead of waiting for 4 KB demand paging. The pre-fault
 * path falls back to 4 KB transparently if PMM cannot deliver a 2 MB
 * chunk. Params: [u64 va_base][u64 size]. */
#define SYSTEM_OP_HEAP_PREFAULT     0x14

/* ─── TagFS defrag (0x18 – 0x19) ───────────────────────────────────── */
#define SYSTEM_OP_DEFRAG_FILE       0x18
#define SYSTEM_OP_FRAG_SCORE        0x19

/* ─── Process tags (0x20 – 0x22) ───────────────────────────────────── */
#define SYSTEM_OP_TAG_ADD           0x20
#define SYSTEM_OP_TAG_REMOVE        0x21
#define SYSTEM_OP_TAG_CHECK         0x22

/* ─── Route / broadcast (0x40 – 0x41) ──────────────────────────────── */
#define SYSTEM_OP_ROUTE             0x40
#define SYSTEM_OP_ROUTE_TAG         0x41

/* ─── Diagnostics (0x50) ───────────────────────────────────────────── */
#define SYSTEM_OP_PERF_DUMP         0x50

/* ─── Touch handle-based ABI (0x51 – 0x58) ─────────────────────────── */
/* Userspace resolves a tag string ONCE via TOUCH_INTERN, caches the
 * uint16_t TouchTag locally, and passes it to all subsequent ops. The
 * hot syscall path takes the tag_id as a u16 in params — no string
 * parsing, no registry lookup. */
#define SYSTEM_OP_TOUCH_CLAIM       0x51
#define SYSTEM_OP_TOUCH_RELEASE     0x52
#define SYSTEM_OP_TOUCH_SEND        0x53
#define SYSTEM_OP_TOUCH_AWAIT       0x54
#define SYSTEM_OP_TOUCH_IRQ_RETURN  0x55
#define SYSTEM_OP_TOUCH_REGISTER    0x56
#define SYSTEM_OP_TOUCH_ACK         0x57
#define SYSTEM_OP_TOUCH_INTERN      0x58  /* tag string -> (full, bare) ids */

/* ─── EFI runtime introspection — read-only (0x60 – 0x62) ──────────── */
#define SYSTEM_OP_EFI_INFO          0x60
#define SYSTEM_OP_EFI_ESRT_GET      0x61
#define SYSTEM_OP_EFI_VERIFY_PE     0x62

/* ─── Bay — cross-cabin shared memory (0x70 – 0x72) ────────────────── */
/* open creates-or-attaches and returns a user-VA pointer; release drops
 * the per-cabin claim; size queries the region size. */
#define SYSTEM_OP_BAY_OPEN          0x70
#define SYSTEM_OP_BAY_RELEASE       0x71
#define SYSTEM_OP_BAY_SIZE          0x72

/* ─── Brook — SPSC ordered streaming (0x75 – 0x7A) ─────────────────── */
/* open attaches with role + maps header/slots; release drops the claim
 * (last release destroys backing); info snapshots live stats. */
#define SYSTEM_OP_BROOK_OPEN        0x75
#define SYSTEM_OP_BROOK_RELEASE     0x76
#define SYSTEM_OP_BROOK_INFO        0x7A

/* ─── Manifest compile-and-reuse (0x80 – 0x81) ─────────────────────── */
/* Prepared-statement pattern for hot syscall paths: build a Manifest
 * once, COMPILE returns an 8-byte handle, submit many times via
 * POCKET_FLAG_MANIFEST_HANDLE skipping full validation. Handles are
 * per-process; auto-released at process_destroy. */
#define SYSTEM_OP_MANIFEST_COMPILE  0x80
#define SYSTEM_OP_MANIFEST_RELEASE  0x81

/* ─── MemTag — RAM-region tagging (0xA0 – 0xAF) ────────────────────── */

/* Phase 1 — read-only surface. */
#define SYSTEM_OP_MEMTAG_QUERY      0xA0  /* required+any+excluded -> region_ids */
#define SYSTEM_OP_MEMTAG_INFO       0xA1  /* region_id -> {base, pages, tags...} */
#define SYSTEM_OP_MEMTAG_LOOKUP     0xA2  /* phys_addr -> region_id              */
#define SYSTEM_OP_MEMTAG_TAGS       0xA3  /* region_id -> list of tag strings    */
#define SYSTEM_OP_MEMTAG_STATS      0xA4  /* global MemTagStats snapshot         */

/* Phase 2A — enforcement infrastructure. SET_GUARD/GRANT/REVOKE gated on
 * the caller holding the "system" tag-bit (TagFS auth); CABIN_TAGS / CHECK
 * are unprivileged observation. */
#define SYSTEM_OP_MEMTAG_SET_GUARD  0xA5  /* tag_str + u8 on/off                */
#define SYSTEM_OP_MEMTAG_GRANT      0xA6  /* (u32 pid)(tag_str) -> grant cap    */
#define SYSTEM_OP_MEMTAG_REVOKE     0xA7  /* (u32 pid)(tag_str) -> revoke cap   */
#define SYSTEM_OP_MEMTAG_CABIN_TAGS 0xA8  /* u32 pid -> tag_str list            */
#define SYSTEM_OP_MEMTAG_CHECK      0xA9  /* (u32 pid)(u32 region_id) -> bool   */

/* Phase 2H+ — tag-driven PKU PTE stamping. APPLY_PKEY adds `pku:N` to the
 * region (replacing any prior pku:* tag), auto-stamping PTE bits 62:59
 * across every attach + cross-core TLB shootdown. Userspace WRPKRU then
 * gates per-key access. Unprivileged: callers may stamp any region they
 * know the region_id of — security comes from cabin's grant-set on the
 * region's other tags. */
#define SYSTEM_OP_MEMTAG_APPLY_PKEY 0xAA  /* (u32 region_id)(u8 pkey)           */
#define SYSTEM_OP_MEMTAG_LOOKUP_VIRT 0xAB /* (u64 virt) -> region_id (caller VM) */

/* ─── HW — real-HW per-process CPU state (0xB0 – 0xBF) ─────────────── */
/* Per-process state knobs that don't fit MemTag's region-centric model.
 * Each op acts on the CALLING process's VM context unless explicitly
 * params-targeted. Unprivileged: a process can read or set its OWN
 * state without elevation; cross-process mutation requires "system"
 * (not implemented in this opcode range yet). */
#define SYSTEM_OP_HW_LAM_GET        0xB0  /* () -> u8 lam_mode 0/1/2            */
#define SYSTEM_OP_HW_LAM_SET        0xB1  /* (u8 lam_mode) — 0=NONE,1=U48,2=U57 */
#define SYSTEM_OP_HW_TME_STATE      0xB2  /* () -> struct hw_tme_state          */

/* ─── Strand sync — park/wake on address (0xC0 – 0xC1) ─────────────── */
/* Substrate for std::atomic::wait / notify. Parked caller is resumed by
 * SysAddrWake or by the timeout armed via TouchQueueWakeAfter. */
#define SYSTEM_OP_ADDR_PARK         0xC0  /* (u64 va)(u64 expected)(u32 timeout_ms) */
#define SYSTEM_OP_ADDR_WAKE         0xC1  /* (u64 va)(u32 count — 0 = all)         */

#endif // BOXOS_DECKS_H
