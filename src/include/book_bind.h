#ifndef BOOK_BIND_H
#define BOOK_BIND_H

/*
 * The book bound: the first Tally of a volume in both its Tally-Quires, slot 0
 * of its Spine naming it, and the ribbon's two Quires blank for the first
 * stitch a kernel will sew (src/lib/book/book_bind.c).
 *
 * ‼ ONE BINDER FOR EVERY MAKER OF A VOLUME. The host's volume maker
 * (tools/create_tagfs_book.c) binds a volume's book over the files it packs,
 * and the kernel's (tagfs/volume/volume_lay.c) over none; both hand their
 * leaves to this and write what it lays through a sink of their own — the
 * host into an image, the kernel onto a seat. tools/volumeread/binder.py is
 * the same rule said again in Python, by code of its own, so that the bytes
 * of either can be held to it (binder.Circle). A running kernel's Tallies
 * after the first are its own binder's (tagfs/settle/binder*.c), from the
 * state it holds.
 *
 * Integer types come from the includer, as volume_tally.h's do — <stdint.h>
 * on the host, ktypes.h in the kernel — and so do memory and the medium:
 * book_bind.c takes the one or the other by __STDC_HOSTED__, and writes
 * through the sink it is handed.
 */

#include "volume_tally.h"

/* One element of a node: its key, its body and what it counts toward its
 * parent (§2.9: a leaf element 1 — but a FILES Parcel 0, for FILES counts
 * files — and an inner element its child's count). */
typedef struct {
    TallyKey       key;
    const uint8_t* body;
    uint32_t       len;
    uint64_t       count;
} BindItem;

/* What the binder is handed: each tree's leaf elements in key order, every
 * body they point at (freed with them), and the ground's grain, the atom
 * every node begins on (both in blocks) and the Tally's seq. */
typedef struct {
    BindItem* items[TALLY_SHARED + 1u];
    uint32_t  count[TALLY_SHARED + 1u];
    uint32_t  room[TALLY_SHARED + 1u];
    uint8_t** bodies;
    uint32_t  nbodies;
    uint32_t  bodies_room;
    uint64_t  grain;
    uint64_t  grid;
    uint64_t  seq;
} BindLeaves;

/* A node laid out: its tree and level, the count its parent carries for it,
 * its first key (the separator its parent names it by), its place — `off`
 * blocks into each Tally-Quire, `sheets` long — and its stream, with where
 * its records begin (the leads of its sectors). */
typedef struct {
    uint32_t  tree;
    uint32_t  level;
    uint64_t  count;
    uint64_t  sheets;
    uint64_t  off;
    TallyKey  first;
    uint8_t*  stream;
    uint32_t  len;
    uint32_t* starts;
    uint32_t  nstarts;
} BindNode;

/* Where the book goes (§9.3, §16.3 п.5): Tally-Quire A at block `a` and B at
 * `b`, `k` grains each; the ribbon's Quire and its next_quire a grain each
 * after B; the nodes in the order they are written, and each tree's root by
 * its index among them — -1 for a tree that is empty and not written. */
typedef struct {
    uint64_t  a;
    uint64_t  b;
    uint64_t  k;
    uint64_t  ribbon;
    uint64_t  next;
    BindNode* nodes;
    uint32_t  nnodes;
    uint32_t  room;
    int32_t   root[TALLY_SHARED + 1u];
} BindPlan;

/* Where the binder's blocks go: `write` lays `bytes` at block `block` of the
 * volume — its own blocks, VOLUME_BLOCK_BYTES each, counted from its ground —
 * and answers 0, or -1 when the medium did not take them. */
typedef struct {
    int  (*write)(void* ctx, uint64_t block, const void* data, uint64_t bytes);
    void* ctx;
} BindSink;

/* What a binding answers: done, out of memory, a book the ground does not
 * hold (QuireLay), or a write the sink refused (BindTally, BindSpine — at the
 * block they name). */
typedef enum {
    BIND_OK        = 0,
    BIND_NO_MEMORY = -1,
    BIND_NO_ROOM   = -2,
    BIND_REFUSED   = -3,
} BindVerdict;

uint8_t* BindBody(BindLeaves* l, uint64_t bytes);
int      BindAdd(BindLeaves* l, uint32_t tree, TallyKey key, const uint8_t* body, uint32_t len,
                 uint64_t count);
int      BindParcels(BindLeaves* l, uint64_t fid, const TallyRun* runs, uint64_t n);
void     BindLeavesFree(BindLeaves* l);

/* The plan of the book for the leaves `l`, its Tally-Quires from the first
 * grain at or past `after`: BIND_OK; BIND_NO_MEMORY; or BIND_NO_ROOM when its
 * next_quire runs past `data_end` — the far Deed's block — with `plan->a` and
 * `plan->next` left saying where it would have run, and nothing else held. */
int      QuireLay(const BindLeaves* l, uint64_t after, uint64_t data_end, BindPlan* plan);
void     BindPlanFree(BindPlan* plan);

/* Every node of `plan` sealed at `seq` and written in both copies; slot 0 at
 * `slot_block` naming them, and the ribbon's Quires blank. BIND_OK,
 * BIND_NO_MEMORY, or BIND_REFUSED with *failed the block the sink refused. */
int      BindTally(const BindSink* sink, const BindPlan* plan, const uint8_t uuid[16], uint64_t seq,
                   uint64_t* failed);
int      BindSpine(const BindSink* sink, const BindPlan* plan, uint64_t slot_block, uint64_t grain,
                   const uint8_t uuid[16], const SpineHead* head, const SpineCounts* counts,
                   uint64_t slot_seq, uint64_t* failed);

#endif /* BOOK_BIND_H */
