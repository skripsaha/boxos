#include "xhci_msd.h"
#include "boardroom.h"
#include "xhci_endpoint.h"
#include "xhci_enumeration.h"
#include "xhci_port.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_interrupt.h"
#include "xhci_trb.h"
#include "usb_common.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "baton.h"


#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_length;
    uint8_t  cb[16];
} __attribute__((packed)) MsdCbw;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} __attribute__((packed)) MsdCsw;

_Static_assert(sizeof(MsdCbw) == 31, "a command wrapper is 31 bytes");
_Static_assert(sizeof(MsdCsw) == 13, "a status wrapper is 13 bytes");

#define CSW_PASSED      0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

#define MSD_REQ_GET_MAX_LUN 0xFE
#define MSD_REQ_BOT_RESET   0xFF


#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A
#define SCSI_SYNC_CACHE_10    0x35

#define SCSI_SERVICE_ACTION_IN_16 0x9E
#define SCSI_SAI_READ_CAPACITY_16 0x10
#define SCSI_READ_16              0x88
#define SCSI_WRITE_16             0x8A
#define SCSI_SYNC_CACHE_16        0x91

#define MSD_COMMAND_PATIENCE_MS 30000

#define MSD_SLOWEST_BYTES_PER_SEC 128u

static uint32_t msd_patience_ms(uint32_t bytes)
{
    uint64_t ms = ((uint64_t)bytes * 1000u) / MSD_SLOWEST_BYTES_PER_SEC;
    if (ms < MSD_COMMAND_PATIENCE_MS) {
        ms = MSD_COMMAND_PATIENCE_MS;
    }
    if (ms > 0xFFFFFFFFull) {
        ms = 0xFFFFFFFFull;
    }
    return (uint32_t)ms;
}

#define MSD_CTRL_TIMEOUT_MS   1000

#define MSD_BOUNCE_BYTES      (64u * 1024u)

#define MSD_READY_PATIENCE_MS 30000
#define MSD_READY_WAIT_MS     50

#define SENSE_NOT_READY       0x02
#define SENSE_UNIT_ATTENTION  0x06

#define ASC_NOT_READY         0x04
#define ASCQ_BECOMING_READY   0x01
#define ASCQ_START_NEEDED     0x02
#define ASC_NO_MEDIUM         0x3A

struct MsdAsyncReq;
struct MsdJob;

typedef struct XhciMsdUnit {
    struct XhciMsdUnit* next;

    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;

    uint32_t epoch;

    uint8_t  number;
    uint8_t  lun;
    bool     ready;

    uint64_t blocks;
    uint64_t sectors;
    uint32_t block_bytes;
    uint32_t per_sector;

    uint32_t phys_block_bytes;
    uint32_t lowest_aligned_lba;

    uint32_t tag;

    bool     said_gone;

    uint32_t prev_bytes;

    void*    cmd_virt;   uint64_t cmd_phys;
    void*    bounce_virt;uint64_t bounce_phys;

    volatile uint32_t busy;

    struct MsdJob*    watched;
    volatile uint64_t watched_since;

    struct MsdAsyncReq* q_head;
    struct MsdAsyncReq* q_tail;
    spinlock_t          q_lock;

    char     name[41];
} XhciMsdUnit;

static XhciMsdUnit* g_units = NULL;
static spinlock_t   g_units_lock;
static bool         g_units_lock_ready = false;

static void msd_units_lock_init(void)
{
    if (!g_units_lock_ready) {
        spinlock_init(&g_units_lock);
        g_units_lock_ready = true;
    }
}


static void be32_put(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void be64_put(uint8_t* p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
}

static uint64_t be64_get(const uint8_t* p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

static uint32_t be32_get(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void msd_pause_ms(uint32_t ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while ((int64_t)(rdtsc() - deadline) < 0) {
        cpu_pause();
    }
}

static XhciMsdUnit* msd_find_locked(uint8_t number)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->number == number) {
            return u;
        }
    }
    return NULL;
}

static XhciMsdUnit* msd_take(uint8_t number)
{
    if (!g_units_lock_ready) {
        return NULL;
    }

    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(number);
    if (u && (!u->ready || !xhci_slot_enter(u->slot))) {
        u = NULL;
    }
    spin_unlock(&g_units_lock);
    return u;
}

static void msd_give_back(XhciMsdUnit* u)
{
    if (u) {
        xhci_slot_leave(u->slot);
    }
}

#define MSD_GATE_COMPLAIN_MS 2000

static void msd_gate_enter(XhciMsdUnit* u)
{
    if (__atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    uint64_t complain_at = rdtsc() + cpu_ms_to_tsc(MSD_GATE_COMPLAIN_MS);
    bool     complained  = false;

    for (;;) {
        xhci_process_events();
        cpu_pause();

        if (__atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0) {
            return;
        }

        if (!complained && (int64_t)(rdtsc() - complain_at) >= 0) {
            complained = true;
            kprintf("[USB disk %u] a second request has been waiting %u ms for "
                    "its turn on this device\n", u->number, MSD_GATE_COMPLAIN_MS);
        }
    }
}

static void msd_watch_arm(XhciMsdUnit* u, struct MsdJob* j)
{
    __atomic_store_n(&u->watched_since, rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&u->watched, j, __ATOMIC_RELEASE);
}

static void msd_watch_disarm(XhciMsdUnit* u)
{
    __atomic_store_n(&u->watched, NULL, __ATOMIC_RELEASE);
}

static bool msd_gate_try_enter(XhciMsdUnit* u)
{
    return __atomic_exchange_n(&u->busy, 1u, __ATOMIC_ACQUIRE) == 0;
}

static void msd_start_queued(XhciMsdUnit* u);

static void msd_gate_leave(XhciMsdUnit* u)
{
    __atomic_store_n(&u->busy, 0u, __ATOMIC_RELEASE);
    msd_start_queued(u);
}

static uint8_t msd_next_number(void)
{
    for (uint8_t n = 0; n < 255; n++) {
        if (!msd_find_locked(n)) {
            return n;
        }
    }
    return 255;
}


static bool msd_device_is_there(const XhciMsdUnit* u)
{
    if (!u || !u->slot) {
        return false;
    }
    if (!xhci_slot_still_is(u->slot, u->epoch)) {
        return false;
    }
    uint8_t port = u->slot->port_num;
    if (port != 0 && xhci_port_says_gone(u->ctrl, port)) {
        return false;
    }
    return true;
}

static void msd_note_gone(XhciMsdUnit* u, const char* what)
{
    if (u->said_gone) {
        return;
    }
    u->said_gone = true;
    kprintf("[USB disk %u] %s — the device has left, so it is not being "
            "asked for anything more\n", u->number, what);
}


static void msd_clear_halt(XhciMsdUnit* u, uint8_t dci)
{
    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "a halted pipe was to be cleared");
        return;
    }

    xhci_endpoint_t* ep = &u->slot->endpoints[dci];

    usb_setup_packet_t setup = {
        .bmRequestType = 0x02,
        .bRequest = USB_REQ_CLEAR_FEATURE,
        .wValue = USB_FEATURE_ENDPOINT_HALT,
        .wIndex = ep->addr,
        .wLength = 0
    };
    xhci_control_transfer_sync(u->ctrl, u->slot, &setup, 0, 0, false,
                               MSD_CTRL_TIMEOUT_MS);

    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "the pipe would not clear");
        return;
    }

    xhci_ep_recover(u->ctrl, u->slot, dci);
    xhci_command_wait_idle(u->ctrl, MSD_CTRL_TIMEOUT_MS);
}

static void msd_bot_reset(XhciMsdUnit* u)
{
    if (!msd_device_is_there(u)) {
        msd_note_gone(u, "the transport was to be reset");
        return;
    }

    kprintf("[USB disk %u] resetting the transport\n", u->number);

    usb_setup_packet_t setup = {
        .bmRequestType = 0x21,
        .bRequest = MSD_REQ_BOT_RESET,
        .wValue = 0,
        .wIndex = u->slot->interface_num,
        .wLength = 0
    };
    xhci_control_transfer_sync(u->ctrl, u->slot, &setup, 0, 0, false,
                               MSD_CTRL_TIMEOUT_MS);

    msd_clear_halt(u, u->slot->ep_bulk_in);
    msd_clear_halt(u, u->slot->ep_bulk_out);
}



#define MSD_PHASE_IDLE 0
#define MSD_PHASE_CBW  1
#define MSD_PHASE_DATA 2
#define MSD_PHASE_CSW  3
#define MSD_PHASE_DONE 4

#define MSD_CSW_TRIES 2

typedef struct MsdJob {
    Baton node;

    XhciMsdUnit* u;

    uint8_t   cdb[16];
    uint8_t   cdb_len;
    uint64_t  data_phys;
    uint32_t  data_len;
    bool      data_in;

    uint32_t  tag;
    uint8_t   phase;
    uint8_t   dci;
    uint8_t   csw_tries;
    bool      hand_to_kcore;

    uint32_t  behind;
    uint32_t  transferred;
    int       status;
    volatile uint8_t finished;

    void (*done)(void* ctx, int status, uint32_t transferred);
    void*  done_ctx;
} MsdJob;

static void msd_job_step(void* ctx);

static int msd_job_submit(MsdJob* j, uint8_t dci, uint64_t phys, uint32_t len)
{
    j->dci = dci;
    return xhci_ep_submit_async(j->u->ctrl, j->u->slot, dci, phys, len,
                                j->hand_to_kcore ? &j->node : NULL);
}

static void msd_job_finish(MsdJob* j, int status)
{
    j->phase  = MSD_PHASE_DONE;
    j->status = status;
    __atomic_store_n(&j->finished, 1u, __ATOMIC_RELEASE);
    if (j->done) {
        j->done(j->done_ctx, status, j->transferred);
    }
}

static void msd_job_begin(MsdJob* j)
{
    XhciMsdUnit* u = j->u;
    MsdCbw* cbw = (MsdCbw*)u->cmd_virt;

    j->tag = ++u->tag;

    j->behind      = u->prev_bytes;
    u->prev_bytes  = j->data_len;

    j->node.run = msd_job_step;
    j->node.ctx = j;

    memset(cbw, 0, sizeof(*cbw));
    cbw->signature   = CBW_SIGNATURE;
    cbw->tag         = j->tag;
    cbw->data_length = j->data_len;
    cbw->flags       = j->data_in ? 0x80 : 0x00;
    cbw->lun         = u->lun;
    cbw->cb_length   = j->cdb_len;
    memcpy(cbw->cb, j->cdb, j->cdb_len);

    j->phase = MSD_PHASE_CBW;

    if (msd_job_submit(j, u->slot->ep_bulk_out, u->cmd_phys,
                       sizeof(MsdCbw)) != 0) {
        msd_job_finish(j, -1);
    }
}

static void msd_job_step(void* ctx)
{
    MsdJob* j = (MsdJob*)ctx;
    XhciMsdUnit* u = j->u;

    if (j->phase == MSD_PHASE_IDLE || j->phase == MSD_PHASE_DONE) {
        return;
    }

    uint8_t  code     = 0;
    uint32_t residual = 0;
    if (!xhci_ep_take_result(u->slot, j->dci, &code, &residual)) {
        return;
    }

    bool ok    = (code == TRB_COMPLETION_SUCCESS);
    bool short_ok = ok || (code == TRB_COMPLETION_SHORT_PKT);

    switch (j->phase) {

    case MSD_PHASE_CBW:
        if (!ok) {
            if (code == TRB_COMPLETION_STALL) {
                msd_clear_halt(u, u->slot->ep_bulk_out);
            }
            msd_job_finish(j, -1);
            return;
        }

        if (j->data_len > 0) {
            j->phase = MSD_PHASE_DATA;
            uint8_t dci = j->data_in ? u->slot->ep_bulk_in : u->slot->ep_bulk_out;
            if (msd_job_submit(j, dci, j->data_phys, j->data_len) != 0) {
                msd_job_finish(j, -1);
            }
            return;
        }
        j->phase = MSD_PHASE_CSW;
        memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
        if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                           sizeof(MsdCsw)) != 0) {
            msd_job_finish(j, -1);
        }
        return;

    case MSD_PHASE_DATA:
        if (code == TRB_COMPLETION_STALL) {
            msd_clear_halt(u, j->dci);
        } else if (!short_ok) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        } else {
            j->transferred = (residual <= j->data_len)
                           ? (j->data_len - residual) : 0;
        }

        j->phase = MSD_PHASE_CSW;
        memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
        if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                           sizeof(MsdCsw)) != 0) {
            msd_job_finish(j, -1);
        }
        return;

    case MSD_PHASE_CSW: {
        if (code == TRB_COMPLETION_STALL && j->csw_tries < MSD_CSW_TRIES) {
            j->csw_tries++;
            msd_clear_halt(u, u->slot->ep_bulk_in);
            memset((uint8_t*)u->cmd_virt + 64, 0, sizeof(MsdCsw));
            if (msd_job_submit(j, u->slot->ep_bulk_in, u->cmd_phys + 64,
                               sizeof(MsdCsw)) != 0) {
                msd_job_finish(j, -1);
            }
            return;
        }
        if (!short_ok) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }

        const MsdCsw* csw = (const MsdCsw*)((uint8_t*)u->cmd_virt + 64);
        if (csw->signature != CSW_SIGNATURE || csw->tag != j->tag) {
            kprintf("[USB disk %u] status wrapper does not match the command "
                    "(signature 0x%08x, tag %u for %u)\n",
                    u->number, csw->signature, csw->tag, j->tag);
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }
        if (csw->status == CSW_PHASE_ERROR) {
            msd_bot_reset(u);
            msd_job_finish(j, -1);
            return;
        }

        msd_job_finish(j, (csw->status == CSW_PASSED) ? 0 : 1);
        return;
    }

    default:
        return;
    }
}

static void msd_job_run(MsdJob* j)
{
    msd_job_begin(j);

    const uint32_t patience = msd_patience_ms(j->data_len + j->behind);
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(patience);

    while (!__atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
        xhci_process_events();
        msd_job_step(j);

        if (__atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
            break;
        }

        if (!msd_device_is_there(j->u)) {
            msd_note_gone(j->u, "an answer was owed at this stage");
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_job_finish(j, -1);
            break;
        }

        uint8_t ep_state = xhci_ep_context_state(j->u->ctrl, j->u->slot, j->dci);
        if (ep_state == XHCI_EP_STATE_ERROR ||
            ep_state == XHCI_EP_STATE_DISABLED) {
            kprintf("[USB disk %u] the pipe this command is on is %s — not "
                    "waiting for an answer that cannot come\n",
                    j->u->number, xhci_ep_state_name(ep_state));
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_job_finish(j, -1);
            break;
        }

        if (j->u->ctrl->error_state) {
            kprintf("[USB disk %u] the controller has stopped — this command "
                    "has nobody to answer it\n", j->u->number);
            msd_job_finish(j, -1);
            break;
        }

        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            kprintf("[USB disk %u] the device has been asking for more time "
                    "for %u ms at stage %u — giving up on the command\n",
                    j->u->number, patience, j->phase);
            xhci_ep_abandon(j->u->ctrl, j->u->slot, j->dci);
            msd_bot_reset(j->u);
            msd_job_finish(j, -1);
            break;
        }
        cpu_pause();
    }
}

static int msd_command(XhciMsdUnit* u, const uint8_t* cdb, uint8_t cdb_len,
                       uint64_t data_phys, uint32_t data_len, bool data_in,
                       uint32_t* out_transferred)
{
    if (cdb_len == 0 || cdb_len > 16) {
        return -1;
    }

    if (xhci_drain_is_mine(u->ctrl)) {
        kprintf("[USB disk %u] a command was waited for from inside the event "
                "drain — its answer cannot arrive until this returns\n",
                u->number);
        return -1;
    }

    MsdJob job;
    memset(&job, 0, sizeof(job));
    job.u         = u;
    job.cdb_len   = cdb_len;
    job.data_phys = data_phys;
    job.data_len  = data_len;
    job.data_in   = data_in;
    memcpy(job.cdb, cdb, cdb_len);

    if (out_transferred) {
        *out_transferred = 0;
    }

    msd_job_run(&job);

    if (out_transferred) {
        *out_transferred = job.transferred;
    }
    return job.status;
}
static int msd_request_sense(XhciMsdUnit* u, uint8_t* out_key, uint8_t* out_asc,
                             uint8_t* out_ascq)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 18);
    int rc = msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 18, true, &got);
    if (rc != 0 || got < 14) {
        return -1;
    }

    const uint8_t* s = (const uint8_t*)u->bounce_virt;
    if (out_key)  *out_key  = s[2] & 0x0F;
    if (out_asc)  *out_asc  = s[12];
    if (out_ascq) *out_ascq = (got > 13) ? s[13] : 0;
    return 0;
}


static int msd_read_capacity(XhciMsdUnit* u)
{
    uint8_t cdb10[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 8);
    int rc = msd_command(u, cdb10, sizeof(cdb10), u->bounce_phys, 8, true, &got);
    if (rc != 0 || got < 8) {
        return -1;
    }

    const uint8_t* c = (const uint8_t*)u->bounce_virt;
    uint32_t last_lba = be32_get(c);
    uint32_t block    = be32_get(c + 4);

    if (block == 0) {
        return -1;
    }

    bool ten_sufficed = (last_lba != 0xFFFFFFFFu);
    if (ten_sufficed) {
        u->block_bytes = block;
        u->blocks      = (uint64_t)last_lba + 1;
    }

    uint8_t cdb16[16] = {0};
    cdb16[0]  = SCSI_SERVICE_ACTION_IN_16;
    cdb16[1]  = SCSI_SAI_READ_CAPACITY_16;
    be32_put(&cdb16[10], 32);

    memset(u->bounce_virt, 0, 32);
    got = 0;
    rc = msd_command(u, cdb16, sizeof(cdb16), u->bounce_phys, 32, true, &got);

    if (rc != 0 || got < 12) {
        if (!ten_sufficed) {
            kprintf("[USB disk %u] says it is larger than a ten-byte capacity "
                    "can state and then would not answer the sixteen-byte "
                    "one\n", u->number);
            return -1;
        }
        return 0;
    }

    uint64_t last64 = be64_get(c);
    uint32_t blk64  = be32_get(c + 8);
    if (blk64 == 0) {
        return ten_sufficed ? 0 : -1;
    }

    if (!ten_sufficed) {
        u->block_bytes = blk64;
        u->blocks      = last64 + 1;
    } else if (blk64 != u->block_bytes) {
        kprintf("[USB disk %u] answers %u-byte blocks to one capacity command "
                "and %u to the other; using %u\n",
                u->number, u->block_bytes, blk64, u->block_bytes);
    }

    if (got >= 16) {
        uint32_t exponent = c[13] & 0x0Fu;
        if (u->block_bytes <= (0xFFFFFFFFu >> exponent)) {
            u->phys_block_bytes = u->block_bytes << exponent;
        }
        u->lowest_aligned_lba = ((uint32_t)(c[14] & 0x3Fu) << 8) | c[15];
    }
    return 0;
}

static int msd_wait_ready(XhciMsdUnit* u)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(MSD_READY_PATIENCE_MS);
    bool said_coming_up = false;

    for (;;) {
        int rc = msd_command(u, cdb, sizeof(cdb), 0, 0, false, NULL);
        if (rc == 0) {
            if (said_coming_up) {
                kprintf("[USB disk %u] ready\n", u->number);
            }
            return 0;
        }
        if (rc < 0) {
            return -1;
        }

        uint8_t key = 0, asc = 0, ascq = 0;
        if (msd_request_sense(u, &key, &asc, &ascq) != 0) {
            kprintf("[USB disk %u] refused a command and would not say why\n",
                    u->number);
            return -1;
        }

        if (key == SENSE_UNIT_ATTENTION) {
            continue;
        }

        if (key != SENSE_NOT_READY) {
            kprintf("[USB disk %u] is not ready and does not say it is coming "
                    "up (sense key 0x%x, code 0x%02x/0x%02x)\n",
                    u->number, key, asc, ascq);
            return -1;
        }

        if (asc == ASC_NO_MEDIUM) {
            kprintf("[USB disk %u] no medium\n", u->number);
            return -1;
        }

        if (asc != ASC_NOT_READY || ascq == ASCQ_START_NEEDED) {
            kprintf("[USB disk %u] is not ready and waiting will not change it "
                    "(code 0x%02x/0x%02x%s)\n", u->number, asc, ascq,
                    ascq == ASCQ_START_NEEDED ? " — it wants a START UNIT" : "");
            return -1;
        }

        if (!said_coming_up) {
            said_coming_up = true;
            kprintf("[USB disk %u] says it is still coming up — waiting for "
                    "it to say otherwise\n", u->number);
        }

        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            kprintf("[USB disk %u] has been coming up for %u ms and has not "
                    "finished — not using it\n", u->number,
                    MSD_READY_PATIENCE_MS);
            return -1;
        }

        msd_pause_ms(MSD_READY_WAIT_MS);
    }
}

static void msd_read_identity(XhciMsdUnit* u)
{
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 36);
    if (msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 36, true, &got) != 0 ||
        got < 36) {
        ksnprintf(u->name, sizeof(u->name), "usb%u", u->number);
        return;
    }

    const char* id = (const char*)u->bounce_virt;
    char vendor[9], product[17];
    memcpy(vendor, id + 8, 8);   vendor[8] = '\0';
    memcpy(product, id + 16, 16); product[16] = '\0';
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--)  vendor[i] = '\0';
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = '\0';

    ksnprintf(u->name, sizeof(u->name), "usb%u %s %s", u->number,
              vendor, product);
}

static int msd_attach_held(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    msd_units_lock_init();

    XhciMsdUnit* u = (XhciMsdUnit*)kmalloc(sizeof(XhciMsdUnit));
    if (!u) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    spinlock_init(&u->q_lock);
    u->ctrl  = ctrl;
    u->slot  = slot;
    u->epoch = xhci_slot_epoch(slot);

    void* cmd = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    void* bounce = pmm_alloc_zero(vmm_size_to_pages(MSD_BOUNCE_BYTES),
                                  PHYS_TAG_DMA32);
    if (!cmd || !bounce) {
        if (cmd)    pmm_free(cmd, 1);
        if (bounce) pmm_free(bounce, vmm_size_to_pages(MSD_BOUNCE_BYTES));
        kfree(u);
        return -1;
    }
    u->cmd_phys    = (uint64_t)cmd;
    u->cmd_virt    = vmm_phys_to_virt((uintptr_t)cmd);
    u->bounce_phys = (uint64_t)bounce;
    u->bounce_virt = vmm_phys_to_virt((uintptr_t)bounce);

    spin_lock(&g_units_lock);
    for (XhciMsdUnit* other = g_units; other; other = other->next) {
        if (other->slot == slot) {
            spin_unlock(&g_units_lock);
            pmm_free(cmd, 1);
            pmm_free(bounce, vmm_size_to_pages(MSD_BOUNCE_BYTES));
            kfree(u);
            return 0;
        }
    }
    u->number = msd_next_number();
    u->next   = g_units;
    g_units   = u;
    spin_unlock(&g_units_lock);

    xhci_ctrl_giveup_proof(ctrl, slot);

    uint8_t* lun_buf = (uint8_t*)u->bounce_virt;
    *lun_buf = 0;
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA1,
        .bRequest = MSD_REQ_GET_MAX_LUN,
        .wValue = 0,
        .wIndex = slot->interface_num,
        .wLength = 1
    };
    int rc = xhci_control_transfer_sync(ctrl, slot, &setup, u->bounce_phys, 1,
                                        true, MSD_CTRL_TIMEOUT_MS);
    if (rc == TRB_COMPLETION_STALL) {
        msd_clear_halt(u, 1);
    }
    u->lun = 0;

    if (msd_wait_ready(u) != 0) {
        kprintf("[USB disk %u] never became ready\n", u->number);
        xhci_msd_release(slot);
        return -1;
    }

    if (msd_read_capacity(u) != 0) {
        kprintf("[USB disk %u] would not say how large it is\n", u->number);
        xhci_msd_release(slot);
        return -1;
    }

    msd_read_identity(u);

    if (u->block_bytes < XHCI_MSD_SECTOR_BYTES ||
        (u->block_bytes % XHCI_MSD_SECTOR_BYTES) != 0 ||
        u->block_bytes > MSD_BOUNCE_BYTES) {
        kprintf("[USB disk %u] %s: %u-byte blocks, which this kernel cannot "
                "express in %u-byte sectors — not using it\n",
                u->number, u->name, u->block_bytes, XHCI_MSD_SECTOR_BYTES);
        xhci_msd_release(slot);
        return -1;
    }

    u->per_sector = u->block_bytes / XHCI_MSD_SECTOR_BYTES;
    u->sectors    = u->blocks * u->per_sector;
    u->ready      = true;

    uint64_t mib = (u->sectors * XHCI_MSD_SECTOR_BYTES) / (1024u * 1024u);
    kprintf("[USB disk %u] %s: %llu sectors, %llu MiB\n",
            u->number, u->name,
            (unsigned long long)u->sectors, (unsigned long long)mib);
    if (u->per_sector != 1) {
        kprintf("[USB disk %u] addressed in %u-byte blocks (%llu of them)\n",
                u->number, u->block_bytes, (unsigned long long)u->blocks);
    }

    if (u->phys_block_bytes != 0) {
        kprintf("[USB disk %u] built from %u-byte physical blocks%s\n",
                u->number, u->phys_block_bytes,
                u->lowest_aligned_lba ? ", offset from sector zero" : "");
        if (u->lowest_aligned_lba != 0) {
            kprintf("[USB disk %u] its first aligned block is logical %u — "
                    "anything laid out from zero is skewed on it\n",
                    u->number, u->lowest_aligned_lba);
        }
    } else {
        kprintf("[USB disk %u] would not say what it is built from; assuming "
                "nothing about alignment\n", u->number);
    }
    return 0;
}

int xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints ||
        !slot->ep_bulk_in || !slot->ep_bulk_out) {
        return -1;
    }

    if (!xhci_slot_enter(slot)) {
        return -1;
    }
    int rc = msd_attach_held(ctrl, slot);
    xhci_slot_leave(slot);
    return rc;
}

bool xhci_msd_slot_attached(xhci_device_slot_t* slot)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->slot == slot) {
            return true;
        }
    }
    return false;
}

void xhci_msd_release(xhci_device_slot_t* slot)
{
    if (!slot || !g_units_lock_ready) {
        return;
    }

    spin_lock(&g_units_lock);
    XhciMsdUnit** link = &g_units;
    XhciMsdUnit*  u    = NULL;
    while (*link) {
        if ((*link)->slot == slot) {
            u = *link;
            *link = u->next;
            break;
        }
        link = &(*link)->next;
    }
    spin_unlock(&g_units_lock);

    if (!u) {
        return;
    }

    if (u->ready) {
        kprintf("[USB disk %u] %s is gone\n", u->number, u->name);
    }

    uint8_t number = u->number;

    if (u->cmd_phys)    pmm_free((void*)u->cmd_phys, 1);
    if (u->bounce_phys) pmm_free((void*)u->bounce_phys,
                                 vmm_size_to_pages(MSD_BOUNCE_BYTES));
    kfree(u);

    BoardroomNoteDeparture(BOARD_USB, number);
}


uint8_t xhci_msd_unit_count(void)
{
    if (!g_units_lock_ready) return 0;
    uint8_t n = 0;
    spin_lock(&g_units_lock);
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->ready) n++;
    }
    spin_unlock(&g_units_lock);
    return n;
}

bool xhci_msd_unit_present(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return false;
    }
    msd_give_back(u);
    return true;
}

uint32_t xhci_msd_unit_max_run(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return MSD_BOUNCE_BYTES / XHCI_MSD_SECTOR_BYTES;
    }
    uint32_t blocks = MSD_BOUNCE_BYTES / u->block_bytes;
    uint32_t run    = blocks * u->per_sector;
    msd_give_back(u);
    return run ? run : (MSD_BOUNCE_BYTES / XHCI_MSD_SECTOR_BYTES);
}

uint64_t xhci_msd_unit_sectors(uint8_t unit)
{
    if (!g_units_lock_ready) return 0;
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    uint64_t sectors = (u && u->ready) ? u->sectors : 0;
    spin_unlock(&g_units_lock);
    return sectors;
}

uint32_t xhci_msd_unit_physical_bytes(uint8_t unit)
{
    if (!g_units_lock_ready) return 0;
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    uint32_t bytes = (u && u->ready) ? u->phys_block_bytes : 0;
    spin_unlock(&g_units_lock);
    return bytes;
}

const char* xhci_msd_unit_name(uint8_t unit)
{
    if (!g_units_lock_ready) return "";
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    const char* name = (u && u->ready) ? u->name : "";
    spin_unlock(&g_units_lock);
    return name;
}

static int msd_run_blocks(XhciMsdUnit* u, uint64_t block, uint32_t nblocks,
                          uint32_t bytes, bool write)
{
    uint8_t cdb[16] = {0};
    uint8_t cdb_len;

    if (block + nblocks > 0x100000000ULL) {
        cdb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
        be64_put(&cdb[2], block);
        be32_put(&cdb[10], nblocks);
        cdb_len = 16;
    } else {
        cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        be32_put(&cdb[2], (uint32_t)block);
        cdb[7] = (uint8_t)(nblocks >> 8);
        cdb[8] = (uint8_t)nblocks;
        cdb_len = 10;
    }

    uint32_t moved = 0;
    int rc = msd_command(u, cdb, cdb_len, u->bounce_phys, bytes, !write, &moved);
    if (rc != 0 || moved != bytes) {
        uint8_t key = 0, asc = 0, ascq = 0;
        msd_request_sense(u, &key, &asc, &ascq);
        kprintf("[USB disk %u] %s of %u block(s) at %llu failed "
                "(sense key 0x%x, code 0x%02x/0x%02x)\n",
                u->number, write ? "write" : "read", nblocks,
                (unsigned long long)block, key, asc, ascq);
        return -1;
    }
    return 0;
}

static int msd_rw(uint8_t unit, uint64_t lba, uint32_t count,
                  void* buffer, bool write)
{
    if (!buffer || count == 0) {
        return -1;
    }

    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return -1;
    }
    if (lba + count > u->sectors) {
        kprintf("[USB disk %u] request for sectors %llu..%llu, and it has "
                "%llu\n", unit, (unsigned long long)lba,
                (unsigned long long)(lba + count - 1),
                (unsigned long long)u->sectors);
        msd_give_back(u);
        return -1;
    }

    const uint32_t per_sector  = u->per_sector;
    const uint32_t block_bytes = u->block_bytes;
    const uint32_t blocks_per_pass = MSD_BOUNCE_BYTES / block_bytes;

    uint8_t* caller = (uint8_t*)buffer;
    int result = 0;

    msd_gate_enter(u);

    while (count > 0) {
        uint64_t block     = lba / per_sector;
        uint32_t head      = (uint32_t)(lba % per_sector);
        uint32_t room      = (blocks_per_pass * per_sector) - head;
        uint32_t chunk     = (count > room) ? room : count;
        uint32_t nblocks   = (head + chunk + per_sector - 1) / per_sector;
        uint32_t bytes     = nblocks * block_bytes;
        uint32_t head_bytes = head * XHCI_MSD_SECTOR_BYTES;
        uint32_t chunk_bytes = chunk * XHCI_MSD_SECTOR_BYTES;

        bool whole = (head == 0) && ((chunk % per_sector) == 0);

        if (!write || !whole) {
            if (msd_run_blocks(u, block, nblocks, bytes, false) != 0) {
                result = -1;
                break;
            }
        }

        if (write) {
            memcpy((uint8_t*)u->bounce_virt + head_bytes, caller, chunk_bytes);
            if (msd_run_blocks(u, block, nblocks, bytes, true) != 0) {
                result = -1;
                break;
            }
        } else {
            memcpy(caller, (uint8_t*)u->bounce_virt + head_bytes, chunk_bytes);
        }

        caller += chunk_bytes;
        lba    += chunk;
        count  -= chunk;
    }

    msd_gate_leave(u);
    msd_give_back(u);
    return result;
}



typedef struct MsdAsyncReq {
    struct MsdAsyncReq* next;
    MsdJob         job;
    XhciMsdUnit*   u;
    XhciMsdAsyncCb cb;
    void*          ctx;
    uint8_t        unit_number;
} MsdAsyncReq;

static void msd_async_done(void* ctx, int status, uint32_t transferred);

static void msd_start_queued(XhciMsdUnit* u)
{
    for (;;) {
        spin_lock(&u->q_lock);
        MsdAsyncReq* r = u->q_head;
        if (!r) {
            spin_unlock(&u->q_lock);
            return;
        }
        spin_unlock(&u->q_lock);

        if (!msd_gate_try_enter(u)) {
            return;
        }

        spin_lock(&u->q_lock);
        r = u->q_head;
        if (!r) {
            spin_unlock(&u->q_lock);
            __atomic_store_n(&u->busy, 0u, __ATOMIC_RELEASE);
            return;
        }
        u->q_head = r->next;
        if (!u->q_head) {
            u->q_tail = NULL;
        }
        spin_unlock(&u->q_lock);

        r->next = NULL;
        msd_watch_arm(u, &r->job);
        msd_job_begin(&r->job);

        if (!__atomic_load_n(&r->job.finished, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

static void msd_async_done(void* ctx, int status, uint32_t transferred)
{
    MsdAsyncReq* r = (MsdAsyncReq*)ctx;
    XhciMsdUnit* u = r->u;

    msd_watch_disarm(u);

    XhciMsdAsyncCb cb   = r->cb;
    void*          cctx = r->ctx;
    uint8_t        unit = r->unit_number;
    uint32_t       want = r->job.data_len;

    error_t st = (status == 0 && transferred == want) ? OK : ERR_IO;

    kfree(r);

    msd_gate_leave(u);
    xhci_slot_leave(u->slot);

    if (cb) {
        cb(unit, 0, st, cctx);
    }
}

void xhci_msd_watchdog(void)
{
    if (!g_units_lock_ready) {
        return;
    }

    MsdJob*      late_job  = NULL;
    XhciMsdUnit* late_unit = NULL;

    bool gone = false;

    spin_lock(&g_units_lock);
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        MsdJob* j = __atomic_load_n(&u->watched, __ATOMIC_ACQUIRE);
        if (!j || __atomic_load_n(&j->finished, __ATOMIC_ACQUIRE)) {
            continue;
        }

        bool here = msd_device_is_there(u);
        uint64_t since = __atomic_load_n(&u->watched_since, __ATOMIC_RELAXED);
        if (here && (int64_t)(rdtsc() - since) <
                        (int64_t)cpu_ms_to_tsc(msd_patience_ms(j->data_len + j->behind))) {
            continue;
        }
        if (__atomic_exchange_n(&u->watched, NULL, __ATOMIC_ACQ_REL) != j) {
            continue;
        }
        late_job  = j;
        late_unit = u;
        gone      = !here;
        break;
    }
    spin_unlock(&g_units_lock);

    if (!late_job) {
        return;
    }

    if (gone) {
        msd_note_gone(late_unit, "a read nobody was waiting on was outstanding");
        xhci_ep_abandon(late_unit->ctrl, late_unit->slot, late_job->dci);
        msd_job_finish(late_job, -1);
        return;
    }

    kprintf("[USB disk %u] a read nobody was waiting on has been asking for "
            "more time for %u ms at stage %u — giving up on the command\n",
            late_unit->number,
            msd_patience_ms(late_job->data_len + late_job->behind),
            late_job->phase);
    xhci_ep_abandon(late_unit->ctrl, late_unit->slot, late_job->dci);
    msd_bot_reset(late_unit);
    msd_job_finish(late_job, -1);
}

bool xhci_msd_unit_can_read_async(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return false;
    }
    msd_give_back(u);
    return true;
}

error_t xhci_msd_read_async(uint8_t unit, uint64_t lba, uint32_t count,
                            void* dma_phys, XhciMsdAsyncCb cb, void* ctx)
{
    if (!dma_phys || count == 0) {
        return ERR_INVALID_ARGUMENT;
    }

    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return ERR_DEVICE_NOT_READY;
    }

    if (lba + count > u->sectors) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }

    const uint32_t per_sector = u->per_sector;
    if ((lba % per_sector) != 0 || (count % per_sector) != 0) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }
    uint32_t nblocks = count / per_sector;
    uint32_t bytes   = nblocks * u->block_bytes;
    if (bytes > MSD_BOUNCE_BYTES) {
        msd_give_back(u);
        return ERR_INVALID_ARGUMENT;
    }

    MsdAsyncReq* r = (MsdAsyncReq*)kmalloc(sizeof(MsdAsyncReq));
    if (!r) {
        msd_give_back(u);
        return ERR_NO_MEMORY;
    }
    memset(r, 0, sizeof(*r));
    r->u           = u;
    r->cb          = cb;
    r->ctx         = ctx;
    r->unit_number = unit;

    MsdJob* j = &r->job;
    j->u             = u;
    j->hand_to_kcore = true;
    j->data_phys     = (uint64_t)dma_phys;
    j->data_len      = bytes;
    j->data_in       = true;
    j->done          = msd_async_done;
    j->done_ctx      = r;

    uint64_t block = lba / per_sector;
    if (block + nblocks > 0x100000000ULL) {
        j->cdb[0] = SCSI_READ_16;
        be64_put(&j->cdb[2], block);
        be32_put(&j->cdb[10], nblocks);
        j->cdb_len = 16;
    } else {
        j->cdb[0] = SCSI_READ_10;
        be32_put(&j->cdb[2], (uint32_t)block);
        j->cdb[7] = (uint8_t)(nblocks >> 8);
        j->cdb[8] = (uint8_t)nblocks;
        j->cdb_len = 10;
    }

    if (msd_gate_try_enter(u)) {
        msd_watch_arm(u, j);
        msd_job_begin(j);
        return OK;
    }

    spin_lock(&u->q_lock);
    if (u->q_tail) {
        u->q_tail->next = r;
    } else {
        u->q_head = r;
    }
    u->q_tail = r;
    spin_unlock(&u->q_lock);

    msd_start_queued(u);
    return OK;
}

int xhci_msd_read(uint8_t unit, uint64_t lba, uint32_t count, void* buffer)
{
    return msd_rw(unit, lba, count, buffer, false);
}

int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer)
{
    return msd_rw(unit, lba, count, (void*)buffer, true);
}

int xhci_msd_flush(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return -1;
    }

    uint8_t cdb[16] = {0};
    uint8_t cdb_len;
    if (u->blocks > 0x100000000ULL) {
        cdb[0] = SCSI_SYNC_CACHE_16;
        cdb_len = 16;
    } else {
        cdb[0] = SCSI_SYNC_CACHE_10;
        cdb_len = 10;
    }

    msd_gate_enter(u);
    int rc = msd_command(u, cdb, cdb_len, 0, 0, false, NULL);
    msd_gate_leave(u);
    msd_give_back(u);

    return (rc >= 0) ? 0 : -1;
}