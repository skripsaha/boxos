#include "xhci_caps.h"
#include "xhci_regs.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

#define XHCI_ECAP_MAX_HOPS  64

#define XHCI_HANDOFF_TIMEOUT_MS  1000

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
            return 0;
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
            return 0;
        }

        if (off != from && XHCI_ECAP_ID(dword0) == want) {
            return off;
        }

        uint32_t next = XHCI_ECAP_NEXT(dword0);
        if (next == 0) {
            return 0;
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
        kprintf("[xHCI] no USB Legacy Support capability — the firmware never "
                "claimed this controller\n");
        return 0;
    }

    volatile uint32_t* legsup = (volatile uint32_t*)((uint8_t*)ctrl->cap_regs + off);
    volatile uint32_t* legctl = legsup + 1;

    uint32_t owned = *legsup;
    if (!(owned & XHCI_LEGSUP_BIOS_OWNED)) {
        *legsup = owned | XHCI_LEGSUP_OS_OWNED;
        kprintf("[xHCI] firmware published the legacy capability but was not "
                "holding it\n");
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
            kprintf("[xHCI] firmware did not release the controller within "
                    "%u ms — taking ownership\n", XHCI_HANDOFF_TIMEOUT_MS);
            *legsup = (*legsup & ~XHCI_LEGSUP_BIOS_OWNED) | XHCI_LEGSUP_OS_OWNED;
        } else {
            kprintf("[xHCI] firmware released the controller\n");
        }
    }

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
        if ((uint64_t)off + 16 > ctrl->mmio_size) {
            break;
        }

        volatile uint32_t* cap = (volatile uint32_t*)((uint8_t*)ctrl->cap_regs + off);
        uint32_t revision  = cap[0];
        uint32_t name      = cap[1];
        uint32_t port_info = cap[2];
        uint32_t slot_info = cap[3];

        if (name != XHCI_PROTO_NAME_USB) {
            continue;
        }

        uint8_t major      = (uint8_t)XHCI_PROTO_MAJOR(revision);
        uint8_t minor      = (uint8_t)XHCI_PROTO_MINOR(revision);
        uint8_t first      = (uint8_t)XHCI_PROTO_PORT_OFF(port_info);
        uint8_t count      = (uint8_t)XHCI_PROTO_PORT_COUNT(port_info);
        uint8_t slot_type  = (uint8_t)XHCI_PROTO_SLOT_TYPE(slot_info);

        if (first == 0 || count == 0) {
            continue;
        }

        kprintf("[xHCI] USB %u.%u on ports %u..%u (slot type %u)\n",
                major, minor >> 4, first,
                (unsigned)(first + count - 1), slot_type);

        for (unsigned i = 0; i < count; i++) {
            unsigned port = (unsigned)first + i;
            if (port > ctrl->max_ports || port >= XHCI_PORT_MAP_ENTRIES) {
                break;
            }
            ctrl->port_major[port]     = major;
            ctrl->port_slot_type[port] = slot_type;
            described++;
        }
    }

    if (described == 0) {
        kprintf("[xHCI] no Supported Protocol capability — treating all %u "
                "port(s) as USB 2\n", ctrl->max_ports);
        for (unsigned port = 1;
             port <= ctrl->max_ports && port < XHCI_PORT_MAP_ENTRIES; port++) {
            ctrl->port_major[port] = 2;
        }
    }

    xhci_pair_port_halves(ctrl);
}

void xhci_pair_port_halves(xhci_controller_t* ctrl)
{
    memset(ctrl->port_pair, 0, sizeof(ctrl->port_pair));

    unsigned below = 1;
    unsigned super = 1;
    unsigned sockets = 0;

    for (;;) {
        while (below <= ctrl->max_ports && ctrl->port_major[below] != 2) {
            below++;
        }
        while (super <= ctrl->max_ports && ctrl->port_major[super] < 3) {
            super++;
        }
        if (below > ctrl->max_ports || super > ctrl->max_ports) {
            break;
        }

        ctrl->port_pair[below] = (uint8_t)super;
        ctrl->port_pair[super] = (uint8_t)below;
        sockets++;
        below++;
        super++;
    }

    if (sockets > 0) {
        kprintf("[xHCI %s] %u socket(s) have both halves — a USB 3 port and a "
                "USB 2 port each, paired by position because the controller "
                "does not say\n", ctrl->name, sockets);
    }
}