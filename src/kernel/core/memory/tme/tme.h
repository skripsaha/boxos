#ifndef TME_H
#define TME_H

#include "ktypes.h"
#include "error.h"

/*
 * TME / TME-MK — Total Memory Encryption with Multi-Key extension.
 *
 * Hardware reference:
 *   Intel SDM Vol 3D Chapter 16     — TME / TME-MK architecture
 *   Intel SDM Vol 2D PCONFIG        — key programming instruction
 *   Intel TME-MK Specification 1.4  — operand layout
 *
 * Real-HW model:
 *   - Firmware (UEFI / coreboot) sets IA32_TME_ACTIVATE.LOCK=1 before
 *     the OS boots. The OS sees the activated state and cannot change
 *     it post-lock; we only USE it.
 *   - KeyID 0 = platform default. TME baseline-encrypted with a
 *     platform key. Cannot be reprogrammed; always usable for any
 *     allocation that doesn't request a specific KeyID.
 *   - KeyIDs 1..max_keyid = OS-managed pool. Each programmed via
 *     PCONFIG with a CPU-generated random key (SET_KEY_RANDOM).
 *   - Effective MAXPHYADDR (vmm_maxphyaddr) ALREADY reflects the
 *     KeyID-bit reduction when TME-MK is locked active — CPUID
 *     reports the reduced value, not the raw silicon limit.
 *
 * BoxOS pool model (hybrid — pre-program + re-key-on-free):
 *
 *   tme_init_bsp:
 *     One-time. BSP programs KeyIDs 1..max_keyid with SET_KEY_RANDOM.
 *     Each PCONFIG ≈ 100 µs–1 ms on real silicon; for typical N=32
 *     this is 3 ms one-shot cost at boot. After this, the pool is
 *     "warm" — every KeyID has a CPU-generated random key.
 *
 *   tme_keyid_alloc:
 *     Hot path. Pure software bitmap operation. **Microseconds.**
 *     Returns the first free KeyID slot.
 *
 *   tme_keyid_free:
 *     Off-hot-path (called from Bay destroy etc.). Issues PCONFIG
 *     SET_KEY_RANDOM to re-key the slot BEFORE returning it to the
 *     pool. Security pivot: a freed KeyID never carries its old key
 *     forward to a future reuse — even an attacker who somehow
 *     extracted DRAM after a Bay's death can't decrypt with the new
 *     key.
 *
 * Behavior when TME-MK is inactive (QEMU TCG, BIOS without TME setup,
 * MK extension disabled but TME baseline only):
 *   tme_init_bsp returns OK but sets g_tme.mk_active = false.
 *   All allocation paths return ERR_UNSUPPORTED. Callers should
 *   gracefully fall back to non-encrypted allocations (KeyID 0 = TME
 *   platform-default encryption is still in effect for ALL memory,
 *   without per-region key separation).
 *
 * Touch tags published:
 *   tme:pool:ready             — on successful init
 *   tme:keyid:N (1..max)       — when KeyID N successfully programmed
 *   tme:keyid:N:allocated      — when a consumer reserves KeyID N
 *   tme:keyid:N:released       — when KeyID N returns to the pool
 *   tme:keyid:N:rekey:failed   — diagnostic (slot is removed from pool)
 *
 * Thread-safety:
 *   g_tme is initialized once on BSP and read-only after init. The
 *   KeyID bitmap + in-use counter are protected by g_tme_lock. PCONFIG
 *   is a CPL-0 single-instruction operation that affects only the
 *   targeted KeyID slot — no global serialization needed beyond the
 *   bitmap mutex (per Intel SDM Vol 2D, PCONFIG is atomic w.r.t. its
 *   target slot).
 */

#define TME_MAX_KEYIDS  256u

/* PCONFIG commands (KEY_CTRL bits [3:0]) — Intel SDM Vol 2D. */
#define TME_CMD_SET_KEY_DIRECT  0u
#define TME_CMD_SET_KEY_RANDOM  1u
#define TME_CMD_CLEAR_KEY       2u
#define TME_CMD_NO_ENCRYPT      3u

/* PCONFIG return codes. */
#define TME_PCONFIG_SUCCESS                0u
#define TME_PCONFIG_INVALID_PROG_CMD       1u
#define TME_PCONFIG_ENTROPY_ERROR          2u
#define TME_PCONFIG_INVALID_KEYID          3u
#define TME_PCONFIG_INVALID_ENC_ALG        4u
#define TME_PCONFIG_DEVICE_BUSY            5u

typedef struct {
    bool      mk_active;            /* TME-MK enabled and locked */
    bool      tme_active;           /* TME baseline enabled */
    uint8_t   num_keyid_bits;       /* size of KeyID field in PA */
    uint16_t  max_keyid;            /* highest valid KeyID slot in pool */
    uint16_t  reserved;
    uint8_t   reduced_maxphyaddr;   /* PA bit where KeyID field starts */
    uint8_t   activated_alg;        /* IA32_TME_ACTIVATE.ALGS_ENABLED */
    uint8_t   pconfig_alg;          /* PCONFIG-format alg bitmask */
    uint8_t   pad;
    uint32_t  pool_size;            /* number of usable KeyID slots */
    uint32_t  pool_programmed;      /* number successfully SET_KEY_RANDOM'd */
    uint32_t  in_use;               /* current reservation count */
} TmeState;

extern TmeState g_tme;

/* One-shot BSP init. Idempotent. Safe to call when TME is unsupported
 * (returns OK with mk_active=false). Must be called AFTER vmm_init
 * (depends on vmm_maxphyaddr) and BEFORE any consumer (Bay etc.) tries
 * tme_keyid_alloc. */
error_t tme_init_bsp(void);

/* Hot path. O(N) bitmap scan (N ≤ 256). Microseconds.
 *   ERR_UNSUPPORTED   — TME-MK inactive
 *   ERR_NO_MEMORY     — pool exhausted
 *   ERR_NOT_INITIALIZED — tme_init_bsp not yet completed */
error_t tme_keyid_alloc(uint16_t *out_keyid);

/* Off-hot-path. Issues PCONFIG to re-key the slot, then returns to
 * pool. If PCONFIG fails, the slot is REMOVED from the pool (lost) and
 * the caller's KeyID becomes permanently un-reusable — better to lose
 * a slot than to risk key reuse. */
error_t tme_keyid_free(uint16_t keyid);

/* Pure encoder. Wraps vmm_phys_with_keyid with state-derived params.
 * Returns the input unchanged when TME-MK is inactive. */
uint64_t tme_phys_with_keyid(uint64_t phys, uint16_t keyid);

/* Pure decoder. Strips KeyID bits to recover the raw physical address.
 * Identity when TME-MK is inactive. */
uint64_t tme_phys_strip_keyid(uint64_t phys_with_keyid);

/* Zero-fill phys pages via a temporary kernel mapping that bears the
 * supplied KeyID. Used by consumers (encrypted Bay etc.) so the user
 * mapping (also using KeyID) observes true zeros on first read —
 * without this, the user would see ciphertext_K0(zeros) decrypted
 * with their KeyID = defined-but-garbage bytes.
 *
 * Iterates page-by-page (4 KiB) or huge-page-by-huge-page (2 MiB)
 * through a single locked kernel VA slot. Slow path (one map+memset+
 * unmap per page) — called only at Bay create / similar lifecycle
 * boundaries, not on the hot path.
 *
 * Returns OK on success; ERR_UNSUPPORTED when TME-MK is inactive
 * (caller should fall back to plain pmm_alloc_zero / memset). */
error_t tme_zero_pages_with_keyid(uintptr_t phys_base, size_t pages,
                                   uint16_t keyid, bool huge_2m);

/* Diagnostic dump — boot log + `hw tme` shell command. */
void tme_dump_state(void);

/* ─── DMA + TME-MK interaction (Intel TME-MK Spec §5) ───────────────
 *
 * A device DMA bypasses the CPU memory-encryption engine: the device
 * sees the RAW ciphertext stored in DRAM, NOT the plaintext the CPU
 * sees through its KeyID-bearing PTE. This makes encrypted-Bay-backed
 * pages UNSAFE FOR DMA without IOMMU programming that:
 *   1. Carries the same KeyID in the IOMMU's translation tables, OR
 *   2. Provides a per-device decryption shim (Intel TDX has this for
 *      "shared" pages; OS-managed TME-MK does not).
 *
 * BoxOS currently has VT-d / AMD-Vi drivers but does NOT yet expose
 * a KeyID-aware DMA mapping path. Until that ships, callers MUST
 * treat encrypted Bays as CPU-only and refuse to hand them to device
 * drivers (NIC RX/TX, AHCI, NVMe, GPU framebuffers).
 *
 * The `tme_is_safe_for_dma(phys)` helper below is the gate: returns
 * true ONLY if (a) MK is inactive (raw memory; baseline encryption
 * is transparent to DMA), OR (b) the page has KeyID 0 (platform
 * default key, also transparent to DMA because the platform key is
 * shared with the IOMMU on TME-only hosts; UNSAFE under TDX which
 * partitions KeyID 0 too — TODO when we add TDX support).
 *
 * Drivers wiring up DMA for a user-supplied buffer MUST check this. */
bool tme_is_safe_for_dma(uintptr_t phys_with_keyid);

#endif /* TME_H */
