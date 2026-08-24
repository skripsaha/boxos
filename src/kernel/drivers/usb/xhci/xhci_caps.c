#include "xhci_caps.h"
#include "xhci_regs.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/* A capability list is a chain of numbers the DEVICE chose, walked inside the
 * kernel's own address space. Every hop is therefore checked: it must be dword
 * aligned, it must land inside what was mapped, and there is a ceiling on how
 * many hops a well-formed list can need. A controller that lies about its own
 * list gets a short walk, not a page fault. */
#define XHCI_ECAP_MAX_HOPS  64

/* One second, which is what the specification allows the firmware for the
 * ownership handshake. There is no event to wait on here — the firmware
 * signals by clearing a bit in a register it shares with us — so this is one
 * of the few places where polling is the mechanism the hardware offers rather
 * than a shortcut around one. */
#define XHCI_HANDOFF_TIMEOUT_MS  1000

/*
 * xhci_ecap_find — byte offset of the next capability with id `want`.
 *
 * `from` == 0 starts at the head of the list; passing back an offset this
 * returned continues past it, which is how the several Supported Protocol
 * capabilities of one controller are collected.
 *
 * Returns 0 when there is no such capability.
 */
static uint32_t xhci_ecap_find(xhci_controller_t* ctrl, uint8_t want, uint32_t from)
{
    if (!ctrl || !ctrl->cap_regs) {
        return 0;
    }

    uint32_t off;
    if (from == 0) {
        uint32_t hccparams1 = ctrl->cap_regs->hccparams1;
        if (hccparams1 == 0xFFFFFFFFu) {
            return 0;
        }
        off = XHCI_HCC1_EXT_CAPS(hccparams1) << 2;
        if (off == 0) {
            return 0;                   /* controller publishes no list */
        }
    } else {
        off = from;
    }

    for (unsigned hop = 0; hop < XHCI_ECAP_MAX_HOPS; hop++) {
        if ((off & 0x3u) != 0 || (uint64_t)off + 4 > ctrl->mmio_size) {
            kprintf("[xHCI] extended capability list leaves the mapped register "
                    "space at 0x%x (mapped 0x%llx) — stopping the walk\n",
                    off, (unsigned long long)ctrl->mmio_size);
            return 0;
        }

        volatile uint32_t* entry =
            (volatile uint32_t*)((uint8_t*)ctrl->cap_regs + off);
        uint32_t dword0 = *entry;
        if (dword0 == 0xFFFFFFFFu) {
            return 0;                   /* dead read: nothing decodes here */
        }

        if (off != from && XHCI_ECAP_ID(dword0) == want) {
            return off;
        }

        uint32_t next = XHCI_ECAP_NEXT(dword0);
        if (next == 0) {
            return 0;                   /* end of list */
        }
        off += next << 2;
    }

    kprintf("[xHCI] extended capability list did not end within %u hops — "
            "stopping the walk\n", XHCI_ECAP_MAX_HOPS);
    return 0;
}

int xhci_claim_from_firmware(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->cap_regs) {
        return -1;
    }

    uint32_t off = xhci_ecap_find(ctrl, XHCI_ECAP_ID_LEGACY, 0);
    if (off == 0) {
        /* No USB Legacy Support capability means no firmware ever claimed the
         * controller, which is the normal case under UEFI without CSM and the
         * only case QEMU presents. Nothing to take. */
        debug_printf("[xHCI] no USB Legacy Support capability — controller was "
                     "never firmware-owned\n");
        return 0;
    }

    volatile uint32_t* legsup = (volatile uint32_t*)((uint8_t*)ctrl->cap_regs + off);
    volatile uint32_t* legctl = legsup + 1;             /* USBLEGCTLSTS, +0x04 */

    uint32_t owned = *legsup;
    if (!(owned & XHCI_LEGSUP_BIOS_OWNED)) {
        /* Published the capability but is not holding it. Still claim it, so
         * that the semaphore reflects the truth for anything that looks. */
        *legsup = owned | XHCI_LEGSUP_OS_OWNED;
        debug_printf("[xHCI] firmware published the legacy capability but did "
                     "not hold it\n");
    } else {
        kprintf("[xHCI] firmware owns the controller — requesting handoff\n");

        *legsup = owned | XHCI_LEGSUP_OS_OWNED;

        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(XHCI_HANDOFF_TIMEOUT_MS);
        while ((int64_t)(rdtsc() - deadline) < 0) {
            if (!(*legsup & XHCI_LEGSUP_BIOS_OWNED)) {
                break;
            }
            cpu_pause();
        }

        if (*legsup & XHCI_LEGSUP_BIOS_OWNED) {
            /* The firmware was asked and did not answer. Taking the semaphore
             * by hand is the only remaining move: leaving it set means the SMM
             * handler keeps servicing the controller this kernel is about to
             * reset, and the machine ends up with two owners instead of one. */
            kprintf("[xHCI] firmware did not release the controller within "
                    "%u ms — taking ownership\n", XHCI_HANDOFF_TIMEOUT_MS);
            *legsup = (*legsup & ~XHCI_LEGSUP_BIOS_OWNED) | XHCI_LEGSUP_OS_OWNED;
        } else {
            kprintf("[xHCI] firmware released the controller\n");
        }
    }

    /* Silence every SMI the firmware armed, and clear the three status bits it
     * may have latched. Without this the firmware keeps taking an SMI on
     * events this kernel now handles itself, and every one of those is time
     * stolen from every core with no trace anywhere.
     *
     * The RsvdP fields survive the write: they are bits this driver has no
     * business changing, and preserving them is what the specification asks
     * of anyone writing this register. */
    uint32_t ctl = *legctl;
    ctl &= XHCI_LEGCTL_RSVDP;
    ctl |= XHCI_LEGCTL_SMI_W1C;
    *legctl = ctl;

    return 0;
}

void xhci_map_port_protocols(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->cap_regs) {
        return;
    }

    memset(ctrl->port_major, 0, sizeof(ctrl->port_major));
    memset(ctrl->port_slot_type, 0, sizeof(ctrl->port_slot_type));

    uint32_t off = 0;
    unsigned described = 0;

    while ((off = xhci_ecap_find(ctrl, XHCI_ECAP_ID_PROTOCOL, off)) != 0) {
        /* Four dwords have to be readable before any of them is believed. */
        if ((uint64_t)off + 16 > ctrl->mmio_size) {
            break;
        }

        volatile uint32_t* cap = (volatile uint32_t*)((uint8_t*)ctrl->cap_regs + off);
        uint32_t revision  = cap[0];
        uint32_t name      = cap[1];
        uint32_t port_info = cap[2];
        uint32_t slot_info = cap[3];

        if (name != XHCI_PROTO_NAME_USB) {
            /* Some other protocol rides this controller. Not ours to drive. */
            continue;
        }

        uint8_t major      = (uint8_t)XHCI_PROTO_MAJOR(revision);
        uint8_t minor      = (uint8_t)XHCI_PROTO_MINOR(revision);
        uint8_t first      = (uint8_t)XHCI_PROTO_PORT_OFF(port_info);
        uint8_t count      = (uint8_t)XHCI_PROTO_PORT_COUNT(port_info);
        uint8_t slot_type  = (uint8_t)XHCI_PROTO_SLOT_TYPE(slot_info);

        if (first == 0 || count == 0) {
            continue;                   /* describes no port */
        }

        kprintf("[xHCI] USB %u.%u on ports %u..%u (slot type %u)\n",
                major, minor >> 4, first,
                (unsigned)(first + count - 1), slot_type);

        for (unsigned i = 0; i < count; i++) {
            unsigned port = (unsigned)first + i;
            if (port > ctrl->max_ports || port >= XHCI_PORT_MAP_ENTRIES) {
                break;                  /* claims ports it does not have */
            }
            ctrl->port_major[port]     = major;
            ctrl->port_slot_type[port] = slot_type;
            described++;
        }
    }

    if (described == 0) {
        /* A controller with no Supported Protocol capability predates the
         * requirement, or is lying. Every root port then has to be treated as
         * USB 2, which is the conservative reading: a USB2 port gets a
         * software reset, and a USB3 port tolerates one. */
        kprintf("[xHCI] no Supported Protocol capability — treating all %u "
                "port(s) as USB 2\n", ctrl->max_ports);
        for (unsigned port = 1;
             port <= ctrl->max_ports && port < XHCI_PORT_MAP_ENTRIES; port++) {
            ctrl->port_major[port] = 2;
        }
    }
}
