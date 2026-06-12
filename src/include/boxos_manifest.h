#ifndef BOXOS_MANIFEST_H
#define BOXOS_MANIFEST_H

/*
 * Shared kernel/userspace ABI header.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/defs.h"
 */

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#include "boxos_decks.h"

/*
 * Manifest — variable-length, unbounded chain of operations.
 *
 * Replaces the fixed prefixes[32] array inside legacy Pocket. A Manifest is a
 * self-describing byte stream living in cabin user heap (or, after compilation,
 * in kernel heap). Each op carries:
 *   - op_kind  : (deck<<16 | opcode), 16/16-bit split, freely extensible
 *   - flags    : per-op control (skip-on-error, retry, parallel-barrier, ...)
 *   - in_crate : index into Pocket.crates[] for input data (or CRATE_INDEX_NONE)
 *   - out_crate: index into Pocket.crates[] for output data (or CRATE_INDEX_NONE)
 *   - params[] : inline operation parameters of arbitrary size
 *
 * Op records are packed contiguously after the Manifest header. Iterate with
 * ManifestOpFirst / ManifestOpNext.
 *
 * Compilation:
 *   - User builds Manifest in cabin heap (cheap, no kernel involvement).
 *   - User calls manifest_compile(addr, size) → returns ManifestHandle (u64).
 *   - Kernel copies, validates, runs security_gate once per op, resolves handler
 *     pointers, stores compiled form in kernel heap.
 *   - Subsequent submissions referencing the handle skip all validation —
 *     pure execute.
 *
 * Reuse:
 *   - One compiled Manifest serves many Pockets with different Crate sets.
 *   - Useful for hot paths: shell pipeline, scripted ETL, periodic syscalls.
 *
 * Hard ABI break: there is no compatibility with legacy prefix-chain Pockets.
 */

#define MANIFEST_MAGIC 0x534F4E4Du  /* 'MNOS' — Manifest, BoxOS */

/* Sentinel for in_crate / out_crate when an op needs no buffer of that kind. */
#define CRATE_INDEX_NONE 0xFFFFu

/*
 * Deck IDs come from boxos_decks.h (single source of truth).
 * OP_KIND packs deck (high 16 bits) and opcode (low 16 bits) into a 32-bit
 * key. Deck IDs are 8-bit today but the encoding leaves room for 16-bit
 * extension without ABI churn.
 */
#define OP_KIND(deck, op)     (((uint32_t)(deck) << 16) | ((uint32_t)(op) & 0xFFFFu))
#define OP_KIND_DECK(kind)    ((uint16_t)((kind) >> 16))
#define OP_KIND_OPCODE(kind)  ((uint16_t)((kind) & 0xFFFFu))

/* Manifest-wide flags */
#define MANIFEST_FLAG_TRANSACTIONAL (1u << 0)  /* on failure, all mutations rolled back */
#define MANIFEST_FLAG_PINNED        (1u << 1)  /* kernel keeps compiled form resident */
#define MANIFEST_FLAG_PARALLEL_OK   (1u << 2)  /* ops without dependencies may run concurrently */

/* Per-op flags */
#define OP_FLAG_SKIP_ON_ERROR    (1u << 0)  /* if previous op failed, skip this one */
#define OP_FLAG_RETRY            (1u << 1)  /* on transient error, retry up to N times */
#define OP_FLAG_PARALLEL_BARRIER (1u << 2)  /* synchronization point in PARALLEL_OK manifests */
#define OP_FLAG_OPTIONAL         (1u << 3)  /* failure does not abort manifest */
#define OP_FLAG_TERMINAL         (1u << 4)  /* explicit terminator (e.g., execution deck) */

/*
 * One operation. Variable length: header followed by param_size bytes of
 * inline parameters specific to op_kind.
 */
typedef struct __packed {
    uint32_t op_kind;     /* OP_KIND(deck, opcode) */
    uint16_t flags;       /* OP_FLAG_* */
    uint16_t in_crate;    /* index into Pocket.crates[], or CRATE_INDEX_NONE */
    uint16_t out_crate;   /* index into Pocket.crates[], or CRATE_INDEX_NONE */
    uint16_t param_size;  /* bytes of inline params following this header */
    uint8_t  params[];    /* variable-length inline parameters */
} ManifestOp;

/* static_assert spelling: keyword in C23 and C++; shim for C11/C17. */
#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L))
#define static_assert _Static_assert
#endif

static_assert(sizeof(ManifestOp) == 12, "ManifestOp header must be 12 bytes");

/*
 * Manifest header. Followed inline by op_count ManifestOp records (each
 * variable-length). total_size = sizeof(Manifest) + sum(sizeof(op_i)).
 */
typedef struct __packed {
    uint32_t magic;       /* MANIFEST_MAGIC */
    uint16_t version;     /* layout version (1) */
    uint16_t op_count;    /* number of ops following */
    uint32_t flags;       /* MANIFEST_FLAG_* */
    uint32_t total_size;  /* total bytes (header + all ops) */
} Manifest;

static_assert(sizeof(Manifest) == 16, "Manifest header must be 16 bytes");

#define MANIFEST_VERSION 1u

/* ---------------------------------------------------------------------------
 * Iteration helpers — work on raw (not yet compiled) Manifest streams.
 *
 * These do bounds-checked stepping. Caller must verify total_size matches the
 * actual buffer size before iterating, otherwise out-of-bounds reads occur.
 * ManifestValidate() in manifest.c does that check authoritatively.
 * ---------------------------------------------------------------------------
 */

static inline ManifestOp *ManifestOpFirst(Manifest *m)
{
    if (!m || m->op_count == 0) return NULL;
    return (ManifestOp *)((uint8_t *)m + sizeof(Manifest));
}

static inline ManifestOp *ManifestOpNext(ManifestOp *op)
{
    if (!op) return NULL;
    return (ManifestOp *)((uint8_t *)op + sizeof(ManifestOp) + op->param_size);
}

static inline const ManifestOp *ManifestOpFirstConst(const Manifest *m)
{
    if (!m || m->op_count == 0) return NULL;
    return (const ManifestOp *)((const uint8_t *)m + sizeof(Manifest));
}

static inline const ManifestOp *ManifestOpNextConst(const ManifestOp *op)
{
    if (!op) return NULL;
    return (const ManifestOp *)((const uint8_t *)op + sizeof(ManifestOp) + op->param_size);
}

#endif /* BOXOS_MANIFEST_H */
