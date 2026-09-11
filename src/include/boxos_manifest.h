#ifndef BOXOS_MANIFEST_H
#define BOXOS_MANIFEST_H


#ifndef __packed
#define __packed __attribute__((packed))
#endif

#include "boxos_decks.h"


#define MANIFEST_MAGIC 0x534F4E4Du

#define CRATE_INDEX_NONE 0xFFFFu

#define OP_KIND(deck, op)     (((uint32_t)(deck) << 16) | ((uint32_t)(op) & 0xFFFFu))
#define OP_KIND_DECK(kind)    ((uint16_t)((kind) >> 16))
#define OP_KIND_OPCODE(kind)  ((uint16_t)((kind) & 0xFFFFu))

#define MANIFEST_FLAG_TRANSACTIONAL (1u << 0)
#define MANIFEST_FLAG_PINNED        (1u << 1)
#define MANIFEST_FLAG_PARALLEL_OK   (1u << 2)

#define OP_FLAG_SKIP_ON_ERROR    (1u << 0)
#define OP_FLAG_RETRY            (1u << 1)
#define OP_FLAG_PARALLEL_BARRIER (1u << 2)
#define OP_FLAG_OPTIONAL         (1u << 3)
#define OP_FLAG_TERMINAL         (1u << 4)

typedef struct __packed {
    uint32_t op_kind;
    uint16_t flags;
    uint16_t in_crate;
    uint16_t out_crate;
    uint16_t param_size;
    uint8_t  params[];
} ManifestOp;

#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L))
#define static_assert _Static_assert
#endif

static_assert(sizeof(ManifestOp) == 12, "ManifestOp header must be 12 bytes");

typedef struct __packed {
    uint32_t magic;
    uint16_t version;
    uint16_t op_count;
    uint32_t flags;
    uint32_t total_size;
} Manifest;

static_assert(sizeof(Manifest) == 16, "Manifest header must be 16 bytes");

#define MANIFEST_VERSION 1u


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

#endif