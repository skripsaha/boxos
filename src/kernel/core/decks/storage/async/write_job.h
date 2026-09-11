#ifndef WRITE_JOB_H
#define WRITE_JOB_H

#include "ktypes.h"
#include "error.h"
#include "tagfs.h"
#include "boxos_crate.h"
#include "process.h"
#include "baton.h"


struct OpContext;
struct ManifestOp;

typedef enum {
    W_INIT = 0,
    W_TOKEN_WAIT,
    W_LOCATE,
    W_ALLOC,
    W_COW_BEFORE,
    W_COW_READ_OLD,
    W_DMA_FILL,
    W_AHCI_SUBMIT,
    W_AHCI_DONE,
    W_BEGIN_TXN,
    W_LOG_META,
    W_COW_AFTER,
    W_COMMIT_TXN,
    W_PUBLISH,
    W_RELEASE_TOKEN,
    W_DONE,
    W_ERROR,
} WriteJobState;

typedef struct WriteJob {
    process_t        *target;
    uint32_t          file_id;
    TagFSFileHandle  *handle;
    OpenFileEntry    *ofe;
    Crate            *out_crate;

    const uint8_t    *src_kp;
    void             *src_bounce;
    uint64_t          start_offset;
    uint64_t          total_bytes;
    uint64_t          bytes_done;
    uint32_t          flags;
    uint64_t          waybill;
    uint32_t          submit_cookie;

    uint32_t          if_disk_block;
    uint32_t          if_off_in_blk;
    uint32_t          if_chunk;
    bool              if_partial_rmw;
    bool              if_alloc_fresh;
    bool              if_cow_redirected;
    uint32_t          if_cow_old_block;
    uint16_t          if_extent_idx;
    uint32_t          if_blk_in_ext;
    error_t           if_status;

    uint32_t          alloc_block;
    uint16_t          alloc_count;
    bool              alloc_pending_meta;

    void             *dma_phys;
    void             *dma_virt;

    struct WriteJob  *next_pending;

    int               result_rc;

    _Atomic int       state;

    Baton cq_node;

    Crate            *crates_kbuf;
    uint64_t          crates_uaddr;
    uint16_t          crate_count;
} WriteJob;

int ObjWriteAsync(uint32_t           file_id,
                  uint64_t           offset,
                  uint32_t           flags,
                  const void        *src_kp,
                  uint32_t           size,
                  Crate             *out_crate,
                  const struct OpContext *ctx,
                  Crate             *crates_kbuf,
                  uint16_t           crate_count,
                  uint64_t           crates_uaddr,
                  uint64_t           waybill);

#endif