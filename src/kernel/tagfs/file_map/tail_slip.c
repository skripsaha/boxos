#include "tail_slip.h"

/*
 * TailSlip (tail_slip.h): the bytes the Slips lay over a file's last block,
 * and which of the block's bytes they cover — one bit each.
 */

struct TailSlip {
    volatile uint32_t refs;
    uint8_t           covered[TAGFS_BLOCK_SIZE / 8u];
    uint8_t           bytes[TAGFS_BLOCK_SIZE];
};

TailSlip* TailSlipLaid(const TailSlip* under, uint32_t off, const uint8_t* bytes, uint32_t len)
{
    TailSlip* s = kmalloc(sizeof(TailSlip));
    if (!s)
        return NULL;
    if (under)
        memcpy(s, under, sizeof(TailSlip));
    else
        memset(s, 0, sizeof(TailSlip));
    s->refs = 1;
    for (uint32_t i = off; i < off + len && i < TAGFS_BLOCK_SIZE; i++) {
        s->bytes[i]           = bytes[i - off];
        s->covered[i / 8u] |= (uint8_t)(1u << (i % 8u));
    }
    return s;
}

const TailSlip* TailSlipHold(const TailSlip* slip)
{
    if (slip)
        __atomic_add_fetch(&((TailSlip*)slip)->refs, 1, __ATOMIC_ACQ_REL);
    return slip;
}

void TailSlipLetGo(const TailSlip* slip)
{
    if (slip && __atomic_sub_fetch(&((TailSlip*)slip)->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree((TailSlip*)slip);
}

bool TailSlipOn(const FileRecord* rec, uint64_t k)
{
    return rec && rec->slip && rec->size && k == (rec->size - 1u) / TAGFS_BLOCK_SIZE;
}

bool TailSlipOver(const FileRecord* rec, uint64_t k, uint8_t* block)
{
    if (!TailSlipOn(rec, k))
        return false;
    const TailSlip* s = rec->slip;
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++)
        if (s->covered[i / 8u] & (1u << (i % 8u)))
            block[i] = s->bytes[i];
    return true;
}
