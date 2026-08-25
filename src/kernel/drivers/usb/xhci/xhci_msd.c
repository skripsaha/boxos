#include "xhci_msd.h"
#include "boardroom.h"
#include "xhci_endpoint.h"
#include "xhci_enumeration.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_trb.h"
#include "usb_common.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/* ── Bulk-Only Transport wire format (USB MSC BOT 1.0) ──────────────────── */

#define CBW_SIGNATURE 0x43425355u       /* "USBC" */
#define CSW_SIGNATURE 0x53425355u       /* "USBS" */

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;                     /* 0x80 = device to host */
    uint8_t  lun;                       /* bits 3:0 */
    uint8_t  cb_length;                 /* bits 4:0, 1..16 */
    uint8_t  cb[16];
} __attribute__((packed)) MsdCbw;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;                    /* 0 passed, 1 failed, 2 phase error */
} __attribute__((packed)) MsdCsw;

_Static_assert(sizeof(MsdCbw) == 31, "a command wrapper is 31 bytes");
_Static_assert(sizeof(MsdCsw) == 13, "a status wrapper is 13 bytes");

#define CSW_PASSED      0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

/* Class requests, on the control pipe. */
#define MSD_REQ_GET_MAX_LUN 0xFE
#define MSD_REQ_BOT_RESET   0xFF

/* ── SCSI, the part of it a disk needs to answer ────────────────────────── */

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A
#define SCSI_SYNC_CACHE_10    0x35

/*
 * The sixteen-byte forms (SBC-3).
 *
 * READ CAPACITY(10) answers with a 32-bit last block number, so it can say
 * nothing larger than two tebibytes. A device bigger than that is required to
 * answer 0xFFFFFFFF and wait to be asked again properly — and a host that
 * takes that literally decides the drive is exactly two tebibytes, hands the
 * top of it to a filesystem, and finds out later. READ CAPACITY(16) is a
 * SERVICE ACTION IN command: opcode 0x9E with the action in the low five bits
 * of byte 1.
 */
#define SCSI_SERVICE_ACTION_IN_16 0x9E
#define SCSI_SAI_READ_CAPACITY_16 0x10
#define SCSI_READ_16              0x88
#define SCSI_WRITE_16             0x8A
#define SCSI_SYNC_CACHE_16        0x91

/* One transfer's worth of patience. A flash drive answering a read takes
 * milliseconds; five seconds is the point at which it is not answering. */
#define MSD_XFER_TIMEOUT_MS   5000
#define MSD_CTRL_TIMEOUT_MS   1000

/* The bounce buffer every transfer passes through, and therefore the largest
 * piece of a request that crosses the bus at once. Bigger requests are split,
 * so no caller has to know this number exists. */
#define MSD_BOUNCE_BYTES      (64u * 1024u)

/* A medium can legitimately be "becoming ready" for a while after a device is
 * plugged in — the spin-up of a disk, the initialisation of a card reader. */
#define MSD_READY_ATTEMPTS    40
#define MSD_READY_WAIT_MS     50

typedef struct XhciMsdUnit {
    struct XhciMsdUnit* next;

    xhci_controller_t*  ctrl;
    xhci_device_slot_t* slot;
    uint8_t  number;
    uint8_t  lun;
    bool     ready;

    /*
     * Two counts, because the device and the filesystem measure in different
     * units and pretending otherwise is how a 4096-byte-sector drive gets read
     * one eighth of the way through and believed.
     *
     *   blocks       what the device has, in ITS logical blocks
     *   block_bytes  how big one of those is
     *   sectors      the same medium in the 512-byte sectors BoxOS speaks
     *   per_sector   block_bytes / 512, which is 1 on almost everything
     */
    uint64_t blocks;
    uint64_t sectors;
    uint32_t block_bytes;
    uint32_t per_sector;
    uint32_t tag;

    void*    cmd_virt;   uint64_t cmd_phys;      /* wrappers, one page */
    void*    bounce_virt;uint64_t bounce_phys;   /* data */

    spinlock_t lock;
    char     name[41];                  /* "usb0 VENDOR PRODUCT" and room */
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

/* ── small helpers ──────────────────────────────────────────────────────── */

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

/* The list, walked under the lock that guards it. Callers that go on to speak
 * to the device want msd_take instead — this one is for the bookkeeping that
 * only reads a number. */
static XhciMsdUnit* msd_find_locked(uint8_t number)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->number == number) {
            return u;
        }
    }
    return NULL;
}

/*
 * Take hold of a unit, and of the device underneath it.
 *
 * Finding the unit and stepping inside the slot happen under the same lock, so
 * a device that is being taken down cannot slip between the two. The step
 * inside is what keeps the endpoints, the rings and the buffers alive for as
 * long as this caller is using them: the disk is pulled out by a hand, and the
 * hand does not wait for the filesystem to finish its sentence.
 *
 * The unit's own memory is covered by the same hold, because a unit is only
 * ever released while its slot is being taken down, and that waits.
 */
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

/* Lowest number nobody is using. Numbers are stable for the life of a unit, so
 * a disk that leaves and comes back does not renumber the ones beside it. */
static uint8_t msd_next_number(void)
{
    for (uint8_t n = 0; n < 255; n++) {
        if (!msd_find_locked(n)) {
            return n;
        }
    }
    return 255;
}

/* ── endpoint recovery ──────────────────────────────────────────────────── */

/*
 * A stalled bulk endpoint has to be cleared on both sides of the wire: the
 * device is told with CLEAR_FEATURE(ENDPOINT_HALT) and the controller with
 * Reset Endpoint, and doing only one of them leaves the two disagreeing about
 * whether the pipe is usable. The device goes first, which is the order the
 * mass storage class specification lays out for its own error recovery.
 */
static void msd_clear_halt(XhciMsdUnit* u, uint8_t dci)
{
    xhci_endpoint_t* ep = &u->slot->endpoints[dci];

    usb_setup_packet_t setup = {
        .bmRequestType = 0x02,          /* host to device, endpoint */
        .bRequest = USB_REQ_CLEAR_FEATURE,
        .wValue = USB_FEATURE_ENDPOINT_HALT,
        .wIndex = ep->addr,
        .wLength = 0
    };
    xhci_control_transfer_sync(u->ctrl, u->slot, &setup, 0, 0, false,
                               MSD_CTRL_TIMEOUT_MS);

    xhci_ep_recover(u->ctrl, u->slot, dci);
    xhci_command_wait_idle(u->ctrl, MSD_CTRL_TIMEOUT_MS);
}

/*
 * The reset the class defines for when the two ends have lost track of each
 * other entirely — a phase error, or a status wrapper that never came. It puts
 * the device back at the start of a command and both pipes back in order.
 */
static void msd_bot_reset(XhciMsdUnit* u)
{
    kprintf("[USB disk %u] resetting the transport\n", u->number);

    usb_setup_packet_t setup = {
        .bmRequestType = 0x21,          /* host to device, class, interface */
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

/* ── one command, three transfers ───────────────────────────────────────── */

/*
 * Returns 0 when the device carried the command out, positive when it refused
 * it (a SCSI failure the caller may want to ask about), negative when the
 * conversation itself broke down.
 *
 * The caller holds the unit lock; every buffer used here belongs to the unit.
 */
static int msd_command(XhciMsdUnit* u, const uint8_t* cdb, uint8_t cdb_len,
                       uint64_t data_phys, uint32_t data_len, bool data_in,
                       uint32_t* out_transferred)
{
    if (cdb_len == 0 || cdb_len > 16) {
        return -1;
    }

    MsdCbw* cbw = (MsdCbw*)u->cmd_virt;
    MsdCsw* csw = (MsdCsw*)((uint8_t*)u->cmd_virt + 64);
    uint64_t cbw_phys = u->cmd_phys;
    uint64_t csw_phys = u->cmd_phys + 64;

    uint32_t tag = ++u->tag;

    memset(cbw, 0, sizeof(*cbw));
    cbw->signature   = CBW_SIGNATURE;
    cbw->tag         = tag;
    cbw->data_length = data_len;
    cbw->flags       = data_in ? 0x80 : 0x00;
    cbw->lun         = u->lun;
    cbw->cb_length   = cdb_len;
    memcpy(cbw->cb, cdb, cdb_len);

    if (out_transferred) {
        *out_transferred = 0;
    }

    /* ── command ── */
    uint32_t done = 0;
    int code = xhci_ep_transfer(u->ctrl, u->slot, u->slot->ep_bulk_out,
                                cbw_phys, sizeof(MsdCbw),
                                MSD_XFER_TIMEOUT_MS, &done);
    if (code != TRB_COMPLETION_SUCCESS) {
        if (code == TRB_COMPLETION_STALL) {
            msd_clear_halt(u, u->slot->ep_bulk_out);
        }
        return -1;
    }

    /* ── data, if the command has any ── */
    if (data_len > 0) {
        uint8_t dci = data_in ? u->slot->ep_bulk_in : u->slot->ep_bulk_out;
        code = xhci_ep_transfer(u->ctrl, u->slot, dci, data_phys, data_len,
                                MSD_XFER_TIMEOUT_MS, &done);

        if (code == TRB_COMPLETION_STALL) {
            /* A stalled data stage is not the end of the exchange: the device
             * still owes a status wrapper, and reading it is how the driver
             * learns what went wrong. Clear the pipe and carry on to it. */
            msd_clear_halt(u, dci);
        } else if (code != TRB_COMPLETION_SUCCESS &&
                   code != TRB_COMPLETION_SHORT_PKT) {
            msd_bot_reset(u);
            return -1;
        }
        if (out_transferred) {
            *out_transferred = done;
        }
    }

    /* ── status ── */
    memset(csw, 0, sizeof(*csw));
    code = xhci_ep_transfer(u->ctrl, u->slot, u->slot->ep_bulk_in,
                            csw_phys, sizeof(MsdCsw),
                            MSD_XFER_TIMEOUT_MS, &done);
    if (code == TRB_COMPLETION_STALL) {
        /* One retry: the class allows a device to stall the status stage once,
         * and expects the host to clear it and ask again. */
        msd_clear_halt(u, u->slot->ep_bulk_in);
        code = xhci_ep_transfer(u->ctrl, u->slot, u->slot->ep_bulk_in,
                                csw_phys, sizeof(MsdCsw),
                                MSD_XFER_TIMEOUT_MS, &done);
    }
    if (code != TRB_COMPLETION_SUCCESS && code != TRB_COMPLETION_SHORT_PKT) {
        msd_bot_reset(u);
        return -1;
    }

    if (csw->signature != CSW_SIGNATURE || csw->tag != tag) {
        kprintf("[USB disk %u] status wrapper does not match the command "
                "(signature 0x%08x, tag %u for %u)\n",
                u->number, csw->signature, csw->tag, tag);
        msd_bot_reset(u);
        return -1;
    }

    if (csw->status == CSW_PHASE_ERROR) {
        msd_bot_reset(u);
        return -1;
    }

    return (csw->status == CSW_PASSED) ? 0 : 1;
}

/* Ask the device why it refused. Used for its own sake — the sense key is what
 * tells "no medium" apart from "still spinning up" apart from "broken". */
static int msd_request_sense(XhciMsdUnit* u, uint8_t* out_key, uint8_t* out_asc)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 18);
    int rc = msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 18, true, &got);
    if (rc != 0 || got < 14) {
        return -1;
    }

    const uint8_t* s = (const uint8_t*)u->bounce_virt;
    if (out_key) *out_key = s[2] & 0x0F;
    if (out_asc) *out_asc = s[12];
    return 0;
}

/* ── attach ─────────────────────────────────────────────────────────────── */

/*
 * How big it is, asked in whichever form can hold the answer.
 *
 * Ten bytes first, because every device answers it. A device whose last block
 * number does not fit in the thirty-two bits that reply has is required to say
 * 0xFFFFFFFF, which is not a size — it is the device asking to be asked again
 * with the sixteen-byte form. Believing it costs the whole of a drive above
 * two tebibytes, quietly, with a filesystem laid over the part that is not
 * there.
 */
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

    if (last_lba != 0xFFFFFFFFu) {
        u->block_bytes = block;
        u->blocks      = (uint64_t)last_lba + 1;
        return 0;
    }

    /* Sixteen bytes: opcode, service action, an eight-byte LBA that is zero
     * here, a four-byte allocation length, control. */
    uint8_t cdb16[16] = {0};
    cdb16[0]  = SCSI_SERVICE_ACTION_IN_16;
    cdb16[1]  = SCSI_SAI_READ_CAPACITY_16;
    be32_put(&cdb16[10], 32);

    memset(u->bounce_virt, 0, 32);
    got = 0;
    rc = msd_command(u, cdb16, sizeof(cdb16), u->bounce_phys, 32, true, &got);
    if (rc != 0 || got < 12) {
        kprintf("[USB disk %u] says it is larger than a ten-byte capacity can "
                "state and then would not answer the sixteen-byte one\n",
                u->number);
        return -1;
    }

    uint64_t last64 = be64_get(c);
    uint32_t blk64  = be32_get(c + 8);
    if (blk64 == 0) {
        return -1;
    }

    u->block_bytes = blk64;
    u->blocks      = last64 + 1;
    return 0;
}

static int msd_wait_ready(XhciMsdUnit* u)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    for (unsigned attempt = 0; attempt < MSD_READY_ATTEMPTS; attempt++) {
        int rc = msd_command(u, cdb, sizeof(cdb), 0, 0, false, NULL);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;                  /* the transport, not the medium */
        }

        uint8_t key = 0, asc = 0;
        if (msd_request_sense(u, &key, &asc) == 0) {
            /* 0x02 NOT READY with 0x04 "logical unit not ready" is a device
             * still coming up, and waiting is the right answer. 0x3A is no
             * medium at all — a card reader with no card — and waiting is
             * not, because nothing is going to change. */
            if (key == 0x02 && asc == 0x3A) {
                kprintf("[USB disk %u] no medium\n", u->number);
                return -1;
            }
        }
        msd_pause_ms(MSD_READY_WAIT_MS);
    }

    return -1;
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

    /* Vendor is bytes 8..15, product 16..31, both space padded ASCII. */
    const char* id = (const char*)u->bounce_virt;
    char vendor[9], product[17];
    memcpy(vendor, id + 8, 8);   vendor[8] = '\0';
    memcpy(product, id + 16, 16); product[16] = '\0';
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--)  vendor[i] = '\0';
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = '\0';

    ksnprintf(u->name, sizeof(u->name), "usb%u %s %s", u->number,
              vendor, product);
}

/* Everything here is a conversation with the device, and the whole point of a
 * removable disk is that it can be pulled out in the middle of one. The caller
 * below holds the device open for the length of it. */
static int msd_attach_held(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    msd_units_lock_init();

    XhciMsdUnit* u = (XhciMsdUnit*)kmalloc(sizeof(XhciMsdUnit));
    if (!u) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    spinlock_init(&u->lock);
    u->ctrl = ctrl;
    u->slot = slot;

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
    u->number = msd_next_number();
    u->next   = g_units;
    g_units   = u;
    spin_unlock(&g_units_lock);

    /* How many logical units. A device that does not implement the request
     * stalls it, and a stall here means exactly one — which is every flash
     * drive ever made. */
    uint8_t* lun_buf = (uint8_t*)u->bounce_virt;
    *lun_buf = 0;
    usb_setup_packet_t setup = {
        .bmRequestType = 0xA1,          /* device to host, class, interface */
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
    u->lun = 0;                         /* the first one, always */

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

    /*
     * A device whose blocks are larger than a sector is addressed in its own
     * units and translated here, rather than refused.
     *
     * Refusing was honest while nothing did the translation, and it meant a
     * 4096-byte-sector drive was a drive BoxOS could not boot from. What it
     * needs is arithmetic and, for a write that does not land on a block
     * boundary, the read-modify-write below — which is not an optimisation to
     * be skipped but the only way to change part of a block on a device that
     * will only accept whole ones.
     *
     * What cannot be translated is a block that is not a whole number of
     * sectors, or one larger than the buffer everything moves through. Both
     * are said out loud rather than guessed at.
     */
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
    return 0;
}

int xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints ||
        !slot->ep_bulk_in || !slot->ep_bulk_out) {
        return -1;
    }

    if (!xhci_slot_enter(slot)) {
        return -1;                      /* gone before we said hello */
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

    if (u->cmd_phys)    pmm_free((void*)u->cmd_phys, 1);
    if (u->bounce_phys) pmm_free((void*)u->bounce_phys,
                                 vmm_size_to_pages(MSD_BOUNCE_BYTES));
    kfree(u);

    /*
     * And whoever is keeping a filesystem on it is told NOW, not at its next
     * read.
     *
     * A unit number is handed out again as soon as it is free, and a seat
     * holds a unit number — so a medium that leaves and another that arrives
     * before anybody touches the filesystem are, from the seat's point of
     * view, the same medium throughout. Measured: a stick pulled and pushed
     * back was never noticed to have gone at all, because nothing read from it
     * in between, and the seat went on pointing at whatever took the number.
     *
     * This runs from the service pass, which is ordinary kernel context, so
     * saying it here is allowed and is the last moment at which it is still
     * true that the number belongs to nobody.
     */
    BoardroomNoteDeparture();
}

/* ── sector I/O ─────────────────────────────────────────────────────────── */

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

/*
 * Is there a disk here to speak to right now?
 *
 * Asked by whoever holds a seat number and needs to know whether the medium in
 * it is still there — so the answer has to be "could I start a transfer this
 * instant", not "is the bookkeeping still on the list". Those are different
 * moments: a device is marked as leaving where the unplug is noticed, and its
 * unit is unlinked later, in the pass that takes the device down. Between the
 * two, the old answer was yes.
 *
 * That gap was measurable. A filesystem above asked, was told the medium was
 * still there, and went on serving reads out of its block cache — which
 * happens above the disk driver and never asks it anything. One run in ten
 * assembled a whole program image that way and ran it.
 */
bool xhci_msd_unit_present(uint8_t unit)
{
    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return false;
    }
    msd_give_back(u);
    return true;
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

/* The name belongs to the unit, so it is only worth anything while the unit is
 * seated. Every caller copies it straight away, which is the only safe way to
 * use it and the only way it is used. */
const char* xhci_msd_unit_name(uint8_t unit)
{
    if (!g_units_lock_ready) return "";
    spin_lock(&g_units_lock);
    XhciMsdUnit* u = msd_find_locked(unit);
    const char* name = (u && u->ready) ? u->name : "";
    spin_unlock(&g_units_lock);
    return name;
}

/*
 * One run of device blocks, in or out of the bounce buffer.
 *
 * The command is chosen by the numbers rather than by a capability the device
 * was never asked about: ten bytes while the block number fits in the
 * thirty-two the ten-byte form has room for, sixteen when it does not. A drive
 * above two tebibytes is addressed correctly at both ends of itself, and one
 * below never sees a sixteen-byte command it might not implement.
 */
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
        uint8_t key = 0, asc = 0;
        msd_request_sense(u, &key, &asc);
        kprintf("[USB disk %u] %s of %u block(s) at %llu failed "
                "(sense key 0x%x, code 0x%x)\n",
                u->number, write ? "write" : "read", nblocks,
                (unsigned long long)block, key, asc);
        return -1;
    }
    return 0;
}

/*
 * Sectors in, device blocks out.
 *
 * Everything above this speaks 512-byte sectors, which is what filesystems on
 * this kernel are laid out in. Most devices agree and the translation is the
 * identity. A device with larger blocks does not, and the difference is not
 * something a caller should have to know: a read is widened to the blocks that
 * contain it and the wanted part copied out, and a write that does not begin
 * and end on a block boundary reads the blocks it partly covers first, changes
 * the middle, and writes them back whole. There is no other way to change part
 * of a block on a device that will only accept whole ones — and doing it by
 * writing a partial block instead destroys the sectors on either side.
 */
static int msd_rw(uint8_t unit, uint64_t lba, uint32_t count,
                  void* buffer, bool write)
{
    if (!buffer || count == 0) {
        return -1;
    }

    XhciMsdUnit* u = msd_take(unit);
    if (!u) {
        return -1;                      /* gone, or on its way out */
    }
    if (lba + count > u->sectors) {
        kprintf("[USB disk %u] request for sectors %llu..%llu, and it has "
                "%llu\n", unit, (unsigned long long)lba,
                (unsigned long long)(lba + count - 1),
                (unsigned long long)u->sectors);
        msd_give_back(u);
        return -1;
    }

    const uint32_t per_sector  = u->per_sector;          /* sectors per block */
    const uint32_t block_bytes = u->block_bytes;
    /* Whole blocks per pass, so a pass never has to split one. */
    const uint32_t blocks_per_pass = MSD_BOUNCE_BYTES / block_bytes;

    uint8_t* caller = (uint8_t*)buffer;
    int result = 0;

    spin_lock(&u->lock);

    while (count > 0) {
        uint64_t block     = lba / per_sector;
        uint32_t head      = (uint32_t)(lba % per_sector);   /* sectors in */
        uint32_t room      = (blocks_per_pass * per_sector) - head;
        uint32_t chunk     = (count > room) ? room : count;  /* sectors */
        uint32_t nblocks   = (head + chunk + per_sector - 1) / per_sector;
        uint32_t bytes     = nblocks * block_bytes;
        uint32_t head_bytes = head * XHCI_MSD_SECTOR_BYTES;
        uint32_t chunk_bytes = chunk * XHCI_MSD_SECTOR_BYTES;

        bool whole = (head == 0) && ((chunk % per_sector) == 0);

        /* A write that does not cover the blocks it touches has to read them
         * first; a read always reads. */
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

    spin_unlock(&u->lock);
    msd_give_back(u);
    return result;
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

    /* SYNCHRONIZE CACHE with a zero block count means "all of it". A device
     * that does not implement it refuses, and a refusal here is not a failure:
     * a device with no write cache has nothing to synchronise. */
    uint8_t cdb[16] = {0};
    uint8_t cdb_len;
    if (u->blocks > 0x100000000ULL) {
        cdb[0] = SCSI_SYNC_CACHE_16;
        cdb_len = 16;
    } else {
        cdb[0] = SCSI_SYNC_CACHE_10;
        cdb_len = 10;
    }

    spin_lock(&u->lock);
    int rc = msd_command(u, cdb, cdb_len, 0, 0, false, NULL);
    spin_unlock(&u->lock);
    msd_give_back(u);

    return (rc >= 0) ? 0 : -1;
}
