#include "xhci_msd.h"
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

    uint64_t sectors;                   /* in XHCI_MSD_SECTOR_BYTES units */
    uint32_t block_bytes;
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

static XhciMsdUnit* msd_find(uint8_t number)
{
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->number == number) {
            return u;
        }
    }
    return NULL;
}

/* Lowest number nobody is using. Numbers are stable for the life of a unit, so
 * a disk that leaves and comes back does not renumber the ones beside it. */
static uint8_t msd_next_number(void)
{
    for (uint8_t n = 0; n < 255; n++) {
        if (!msd_find(n)) {
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

static int msd_read_capacity(XhciMsdUnit* u)
{
    uint8_t cdb[10] = { SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t got = 0;

    memset(u->bounce_virt, 0, 8);
    int rc = msd_command(u, cdb, sizeof(cdb), u->bounce_phys, 8, true, &got);
    if (rc != 0 || got < 8) {
        return -1;
    }

    const uint8_t* c = (const uint8_t*)u->bounce_virt;
    uint32_t last_lba = be32_get(c);
    uint32_t block    = be32_get(c + 4);

    if (block == 0) {
        return -1;
    }

    u->block_bytes = block;
    u->sectors     = (uint64_t)last_lba + 1;
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

int xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints ||
        !slot->ep_bulk_in || !slot->ep_bulk_out) {
        return -1;
    }

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

    if (u->block_bytes != XHCI_MSD_SECTOR_BYTES) {
        /* Refused rather than misread. A 4096-byte-sector device needs the
         * layer above to address it in its own units, and pretending its
         * blocks are 512 bytes would read every eighth sector and call the
         * result a filesystem. */
        kprintf("[USB disk %u] %s: %u-byte blocks, and this kernel keeps "
                "filesystems in %u-byte sectors — not using it\n",
                u->number, u->name, u->block_bytes, XHCI_MSD_SECTOR_BYTES);
        xhci_msd_release(slot);
        return -1;
    }

    u->ready = true;

    uint64_t mib = (u->sectors * XHCI_MSD_SECTOR_BYTES) / (1024u * 1024u);
    kprintf("[USB disk %u] %s: %llu sectors, %llu MiB\n",
            u->number, u->name,
            (unsigned long long)u->sectors, (unsigned long long)mib);
    return 0;
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
}

/* ── sector I/O ─────────────────────────────────────────────────────────── */

uint8_t xhci_msd_unit_count(void)
{
    uint8_t n = 0;
    for (XhciMsdUnit* u = g_units; u; u = u->next) {
        if (u->ready) n++;
    }
    return n;
}

bool xhci_msd_unit_present(uint8_t unit)
{
    XhciMsdUnit* u = msd_find(unit);
    return u && u->ready;
}

uint64_t xhci_msd_unit_sectors(uint8_t unit)
{
    XhciMsdUnit* u = msd_find(unit);
    return (u && u->ready) ? u->sectors : 0;
}

const char* xhci_msd_unit_name(uint8_t unit)
{
    XhciMsdUnit* u = msd_find(unit);
    return (u && u->ready) ? u->name : "";
}

static int msd_rw(uint8_t unit, uint64_t lba, uint32_t count,
                  void* buffer, bool write)
{
    XhciMsdUnit* u = msd_find(unit);
    if (!u || !u->ready || !buffer || count == 0) {
        return -1;
    }
    if (lba + count > u->sectors) {
        kprintf("[USB disk %u] request for sectors %llu..%llu, and it has "
                "%llu\n", unit, (unsigned long long)lba,
                (unsigned long long)(lba + count - 1),
                (unsigned long long)u->sectors);
        return -1;
    }

    const uint32_t per_pass = MSD_BOUNCE_BYTES / XHCI_MSD_SECTOR_BYTES;
    uint8_t* caller = (uint8_t*)buffer;
    int result = 0;

    spin_lock(&u->lock);

    while (count > 0) {
        uint32_t chunk = (count > per_pass) ? per_pass : count;
        uint32_t bytes = chunk * XHCI_MSD_SECTOR_BYTES;

        if (write) {
            memcpy(u->bounce_virt, caller, bytes);
        }

        uint8_t cdb[10] = {0};
        cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        be32_put(&cdb[2], (uint32_t)lba);
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)chunk;

        uint32_t moved = 0;
        int rc = msd_command(u, cdb, sizeof(cdb), u->bounce_phys, bytes,
                             !write, &moved);
        if (rc != 0 || moved != bytes) {
            uint8_t key = 0, asc = 0;
            msd_request_sense(u, &key, &asc);
            kprintf("[USB disk %u] %s of %u sector(s) at %llu failed "
                    "(sense key 0x%x, code 0x%x)\n",
                    unit, write ? "write" : "read", chunk,
                    (unsigned long long)lba, key, asc);
            result = -1;
            break;
        }

        if (!write) {
            memcpy(caller, u->bounce_virt, bytes);
        }

        caller += bytes;
        lba    += chunk;
        count  -= chunk;
    }

    spin_unlock(&u->lock);
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
    XhciMsdUnit* u = msd_find(unit);
    if (!u || !u->ready) {
        return -1;
    }

    /* SYNCHRONIZE CACHE with a zero block count means "all of it". A device
     * that does not implement it refuses, and a refusal here is not a failure:
     * a device with no write cache has nothing to synchronise. */
    uint8_t cdb[10] = { SCSI_SYNC_CACHE_10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    spin_lock(&u->lock);
    int rc = msd_command(u, cdb, sizeof(cdb), 0, 0, false, NULL);
    spin_unlock(&u->lock);

    return (rc >= 0) ? 0 : -1;
}
