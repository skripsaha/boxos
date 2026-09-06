#ifndef BOXOS_POCKET_H
#define BOXOS_POCKET_H

/*
 * Shared kernel/userspace ABI header.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

#ifndef __packed
#define __packed __attribute__((packed))
#endif

/*
 * Pocket — kernel-bound syscall envelope.
 *
 * Every Pocket carries POCKET_FLAG_MANIFEST (single-shot Manifest dispatch);
 * the yield is not a pocket at all (GATE_YIELD below). A Manifest-mode
 * envelope describes its letter in one of three ways:
 *
 *   enclosed    POCKET_FLAG_ENCLOSED — the Manifest bytes travel INSIDE the
 *               envelope, in enclosure[]. The ring slot they sit in cannot be
 *               reused until the kernel moves head past it, so the bytes are
 *               alive for exactly as long as the kernel may still read them:
 *               the ring's own discipline is the lifetime guarantee, and the
 *               sender is free the moment the push returns. Every Manifest
 *               that fits (total_size <= POCKET_ENCLOSURE_MAX) travels this
 *               way — chosen automatically by encode_manifest_pocket, never
 *               by the caller.
 *
 *   by address  manifest_addr / manifest_size — the letter stays in cabin
 *               memory and the envelope only names it. The named memory MUST
 *               outlive the kernel's read. A synchronous submitter guarantees
 *               that by waiting for its Result; a no-wait submitter must put
 *               the bytes in memory it owns past the call (box::ferry keeps
 *               them in its station object). An address named by a frame
 *               that has already returned is the kernel executing a dead
 *               stack — the failure class the enclosure exists to close.
 *
 *   by handle   POCKET_FLAG_MANIFEST_HANDLE (set together with MANIFEST) —
 *               manifest_addr carries a ManifestHandle from
 *               SYSTEM_OP_MANIFEST_COMPILE; the compiled form already lives
 *               in kernel memory and manifest_size is ignored.
 *
 * Layout is 128 bytes: 40 bytes of envelope fields + the 88-byte enclosure.
 * The stride divides a 4 KiB page, so a slot never straddles one and a single
 * translation reaches the whole Pocket.
 */

/* The yield is not a pocket. A strand that gives its core away says so in
 * RDI at the syscall gate (idt.c); nothing enters its ring, nothing is owed
 * and nothing is answered. It used to be a pocket, and every yield made
 * while a real pocket waited at the head landed BEHIND it, where the gate
 * could not take it — a strand yielding for its Turn In to be taken filled
 * its ring to the brim in half a second and every submit after that was
 * refused (BIOS 16c, 2026-09-06). */
#define GATE_YIELD                   1u
#define POCKET_FLAG_MANIFEST         0x40
#define POCKET_FLAG_MANIFEST_HANDLE  0x20  /* manifest_addr is a ManifestHandle */
#define POCKET_FLAG_ENCLOSED         0x10  /* Manifest bytes ride in enclosure[] */

/* Envelope fields take 40 bytes; the enclosure is the rest of the 128-byte
 * slot. A Manifest of total_size <= POCKET_ENCLOSURE_MAX rides enclosed. */
#define POCKET_ENCLOSURE_MAX         88u

typedef struct __packed {
    uint32_t pid;             /* kernel overwrites from process_t (security)  */
    uint32_t target_pid;      /* 0 = self, != 0 = IPC route                   */
    uint32_t error_code;      /* deck handlers write errors here              */
    uint8_t  flags;           /* POCKET_FLAG_*                                */
    /* The submit's cloakroom token, little-endian 24-bit, never zero for a
     * synchronous submit. The kernel stamps it into the high 24 bits of the
     * reply Result's `context` (see boxos_kctx.h), and the waiter accepts
     * ONLY the Result carrying its own token — a coat is handed over by
     * token, never "the next one off the rack". Before this, pairing was
     * implicit by ring order, and one late reply (a timed-out caller's, a
     * fire-and-forget's) shifted every later wait onto the wrong answer
     * while the kernel's crate commit-out scribbled a dead stack frame. */
    uint8_t  cookie24[3];
    uint32_t manifest_size;   /* bytes at manifest_addr, or in enclosure[]    */
    uint16_t crate_count;     /* number of entries in Crate[]                 */
    uint16_t pier_id;         /* urgency lane                                 */
    uint64_t manifest_addr;   /* user vaddr of raw Manifest (0 when enclosed) */
    uint64_t crates_addr;     /* user vaddr of Crate[]                        */
    uint8_t  enclosure[POCKET_ENCLOSURE_MAX];  /* the letter itself, when it fits */
} Pocket;

/* static_assert spelling: keyword in C23 and C++; shim for C11/C17. */
#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L))
#define static_assert _Static_assert
#endif

static_assert(sizeof(Pocket) == 128, "Pocket must be 128 bytes (one ring slot)");

/* Read/write the 24-bit submit token (little-endian bytes). */
static inline uint32_t PocketCookie24(const Pocket *p)
{
    return (uint32_t)p->cookie24[0]
         | ((uint32_t)p->cookie24[1] << 8)
         | ((uint32_t)p->cookie24[2] << 16);
}

static inline void PocketSetCookie24(Pocket *p, uint32_t ck)
{
    p->cookie24[0] = (uint8_t)(ck & 0xFFu);
    p->cookie24[1] = (uint8_t)((ck >> 8) & 0xFFu);
    p->cookie24[2] = (uint8_t)((ck >> 16) & 0xFFu);
}
static_assert(__builtin_offsetof(Pocket, enclosure) == 40,
              "Pocket envelope fields must stay 40 bytes so the enclosure fills the slot");

#endif /* BOXOS_POCKET_H */
