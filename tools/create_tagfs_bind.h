/*
 * create_tagfs_bind.h — the book's binder: what create_tagfs_book.c hands it
 * and what it lays out (create_tagfs_bind.c).
 *
 * tools/volumeread/binder.py is the same binder said again in Python, by code
 * of its own, and the two lay the same bytes on the same files — one value,
 * one spelling, one rule of packing for every writer.
 */

#ifndef CREATE_TAGFS_BIND_H
#define CREATE_TAGFS_BIND_H

#include "create_tagfs.h"
#include "volume_tally.h"

/* A file as the book lays it: the cargo's account of it and its one run —
 * blocks counted in u64, for the ground has no width. An empty file has no
 * run: a header alone (§2.9). */
typedef struct {
    const char* path;
    const char* name;          /* its base name, the name it is listed by */
    uint64_t    fid;
    uint64_t    size;
    uint64_t    start;         /* in the volume's blocks */
    uint64_t    blocks;
    uint32_t*   tags;          /* rising */
    uint32_t    ntags;
    uint32_t    role;          /* TALLY_FILE_ROLE_*: what the system keeps it as, 0 for none */
    uint8_t     head[TALLY_HEAD_MOST];   /* its first bytes, head_len of them: min(size, TALLY_HEAD_MOST) */
    uint32_t    head_len;
    uint64_t    passport;      /* the signed program it is, 0 for none (PassportOf) */
} BindFile;

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

uint8_t* BindBody(BindLeaves* l, uint64_t bytes);
int      BindAdd(BindLeaves* l, uint32_t tree, TallyKey key, const uint8_t* body, uint32_t len,
                 uint64_t count);
int      BindParcels(BindLeaves* l, uint64_t fid, const TallyRun* runs, uint64_t n);
void     BindLeavesFree(BindLeaves* l);
int      QuireLay(const BindLeaves* l, uint64_t after, uint64_t data_end, BindPlan* plan);
void     BindPlanFree(BindPlan* plan);
int      BindTally(FILE* disk, const BindPlan* plan, const uint8_t uuid[16], uint64_t seq);
int      BindSpine(FILE* disk, const BindPlan* plan, uint64_t slot_block, uint64_t grain,
                   const uint8_t uuid[16], const SpineHead* head, const SpineCounts* counts,
                   uint64_t slot_seq);

#endif /* CREATE_TAGFS_BIND_H */
