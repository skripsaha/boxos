#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "xhci_interrupt.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_port.h"
#include "xhci_caps.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "pit.h"
#include "idt.h"
#include "irqchip.h"
#include "pic.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "amp.h"

static xhci_controller_t global_controller = {0};

xhci_controller_t* xhci_get_controller(void) {
    if (!global_controller.initialized) {
        return NULL;
    }
    return &global_controller;
}

/* The specification gives the controller 16 ms to halt; some take longer, so
 * this follows Linux in allowing 32. A reset is allowed a full second — the
 * one number here that is generous on purpose, because a controller that has
 * just been taken away from firmware may have work of its own to finish. */
#define XHCI_HALT_TIMEOUT_MS     32
#define XHCI_RESET_TIMEOUT_MS    1000
#define XHCI_READY_TIMEOUT_MS    1000
#define XHCI_START_TIMEOUT_MS    100

/* Wait for a bit in a controller register to reach a value.
 *
 * Every wait in this driver is a wait on hardware that offers no event to
 * sleep on — the controller signals by changing a bit in a register we share
 * with it. Polling is therefore the mechanism, not a shortcut past one. What
 * matters is that the clock is the TSC and not a loop count: a count is a
 * measure of how fast this CPU happens to be, and BoxOS has already been
 * bitten once by a delay that could not fail and could hang.
 *
 * Returns true when the condition was met, false on timeout. */
static bool xhci_wait_bit(volatile uint32_t* reg, uint32_t mask, bool want_set,
                          uint32_t timeout_ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    for (;;) {
        uint32_t value = *reg;
        if (value == 0xFFFFFFFFu) {
            return false;               /* the device stopped decoding */
        }
        if (((value & mask) != 0) == want_set) {
            return true;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return false;
        }
        cpu_pause();
    }
}

int xhci_reset(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->op_regs) {
        return -1;
    }

    debug_printf("[xHCI] Starting controller reset...\n");

    /* Nothing may be written to an operational register while Controller Not
     * Ready is set. A controller taken from firmware mid-stride is exactly the
     * case where it is, and the old code wrote USBCMD without asking. */
    if (!xhci_wait_bit(&ctrl->op_regs->usbsts, XHCI_STS_CNR, false,
                       XHCI_READY_TIMEOUT_MS)) {
        kprintf("[xHCI] ERROR: controller still not ready after %u ms "
                "(USBSTS=0x%08x)\n", XHCI_READY_TIMEOUT_MS,
                ctrl->op_regs->usbsts);
        return -1;
    }

    ctrl->op_regs->usbcmd &= ~(uint32_t)XHCI_CMD_RUN;

    if (!xhci_wait_bit(&ctrl->op_regs->usbsts, XHCI_STS_HCH, true,
                       XHCI_HALT_TIMEOUT_MS)) {
        kprintf("[xHCI] ERROR: controller did not halt within %u ms "
                "(USBSTS=0x%08x)\n", XHCI_HALT_TIMEOUT_MS,
                ctrl->op_regs->usbsts);
        return -1;
    }

    debug_printf("[xHCI] Controller halted\n");

    ctrl->op_regs->usbcmd |= XHCI_CMD_RESET;

    if (!xhci_wait_bit(&ctrl->op_regs->usbcmd, XHCI_CMD_RESET, false,
                       XHCI_RESET_TIMEOUT_MS)) {
        kprintf("[xHCI] ERROR: reset did not complete within %u ms\n",
                XHCI_RESET_TIMEOUT_MS);
        return -1;
    }

    if (!xhci_wait_bit(&ctrl->op_regs->usbsts, XHCI_STS_CNR, false,
                       XHCI_READY_TIMEOUT_MS)) {
        kprintf("[xHCI] ERROR: controller not ready after reset\n");
        return -1;
    }

    debug_printf("[xHCI] Reset complete\n");
    return 0;
}

int xhci_start(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->op_regs) {
        return -1;
    }

    debug_printf("[xHCI] Starting controller...\n");

    ctrl->op_regs->usbcmd |= XHCI_CMD_RUN;

    if (!xhci_wait_bit(&ctrl->op_regs->usbsts, XHCI_STS_HCH, false,
                       XHCI_START_TIMEOUT_MS)) {
        kprintf("[xHCI] ERROR: controller failed to start (USBSTS=0x%08x)\n",
                ctrl->op_regs->usbsts);
        return -1;
    }

    debug_printf("[xHCI] Controller running\n");
    return 0;
}

/*
 * Scratchpad — the pages a controller asks for so it has somewhere to keep its
 * own internal state across the operations software asks of it.
 *
 * The count arrives split across two fields at opposite ends of HCSPARAMS2,
 * and the answer goes back in the first entry of the Device Context Base
 * Address Array — the one slot ID zero can never use. There is no register
 * that says "you forgot": a controller wanting scratchpad and given none
 * simply misbehaves later, at a time and in a way that looks like something
 * else entirely. QEMU asks for none, which is exactly why this was missing
 * from a driver that appeared to work.
 */
static int xhci_alloc_scratchpad(xhci_controller_t* ctrl)
{
    uint32_t count = XHCI_HCS2_MAX_SCRATCHPAD(ctrl->cap_regs->hcsparams2);
    ctrl->scratchpad_count = count;

    if (count == 0) {
        return 0;
    }

    /* The buffers are one controller page each, and the controller states its
     * page size rather than assuming ours. Bit 0 means 4 KB, which is the
     * minimum the specification allows and the only size this kernel has. */
    uint32_t pagesize = ctrl->op_regs->pagesize & 0xFFFFu;
    if ((pagesize & 0x1u) == 0) {
        kprintf("[xHCI] ERROR: controller does not support 4 KB pages "
                "(PAGESIZE=0x%04x) — refusing to guess\n", pagesize);
        return -1;
    }

    size_t array_bytes = (size_t)count * sizeof(uint64_t);
    size_t array_pages = vmm_size_to_pages(array_bytes);

    void* array_phys = pmm_alloc_zero(array_pages, PHYS_TAG_DMA32);
    if (!array_phys) {
        kprintf("[xHCI] ERROR: cannot allocate scratchpad array for %u "
                "buffer(s)\n", count);
        return -1;
    }

    ctrl->scratchpad_array      = (uint64_t*)vmm_phys_to_virt((uintptr_t)array_phys);
    ctrl->scratchpad_array_phys = (uint64_t)array_phys;

    for (uint32_t i = 0; i < count; i++) {
        void* buf_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
        if (!buf_phys) {
            kprintf("[xHCI] ERROR: cannot allocate scratchpad buffer %u of "
                    "%u\n", i, count);
            /* Give back what was taken; a half-filled array handed to the
             * controller is worse than none. */
            for (uint32_t j = 0; j < i; j++) {
                pmm_free((void*)ctrl->scratchpad_array[j], 1);
            }
            pmm_free(array_phys, array_pages);
            ctrl->scratchpad_array      = NULL;
            ctrl->scratchpad_array_phys = 0;
            ctrl->scratchpad_count      = 0;
            return -1;
        }
        ctrl->scratchpad_array[i] = (uint64_t)buf_phys;
    }

    __sync_synchronize();
    kprintf("[xHCI] scratchpad: %u page(s) the controller asked for\n", count);
    return 0;
}

static void xhci_free_scratchpad(xhci_controller_t* ctrl)
{
    if (!ctrl->scratchpad_array) {
        return;
    }
    for (uint32_t i = 0; i < ctrl->scratchpad_count; i++) {
        if (ctrl->scratchpad_array[i]) {
            pmm_free((void*)ctrl->scratchpad_array[i], 1);
        }
    }
    pmm_free((void*)ctrl->scratchpad_array_phys,
             vmm_size_to_pages((size_t)ctrl->scratchpad_count * sizeof(uint64_t)));
    ctrl->scratchpad_array      = NULL;
    ctrl->scratchpad_array_phys = 0;
    ctrl->scratchpad_count      = 0;
}

/*
 * On Intel chipsets that carry both an EHCI and an xHCI controller — roughly
 * 2012 through 2015, Panther Point onwards — the USB2 ports come out of reset
 * wired to EHCI. Software moves them by writing two configuration registers,
 * and until it does, the xHCI controller reports the ports as empty no matter
 * what is plugged into them. A keyboard on such a board is simply not there.
 *
 * The registers only exist on parts that have the EHCI companion, so this is
 * gated on actually finding one. On a 300-series PCH — the board this kernel
 * was brought up on — there is no EHCI at all and this does nothing.
 */
#define USB_INTEL_XUSB2PR     0xD0
#define USB_INTEL_USB2PRM     0xD4
#define USB_INTEL_USB3_PSSEN  0xD8
#define USB_INTEL_USB3PRM     0xDC
#define PCI_VENDOR_INTEL      0x8086

static void xhci_intel_route_ports(xhci_controller_t* ctrl)
{
    if (ctrl->pci_dev.vendor_id != PCI_VENDOR_INTEL) {
        return;
    }

    pci_device_t ehci;
    if (pci_find_device_by_class(0x0C, 0x03, 0x20, &ehci) != 0) {
        return;                         /* no companion: nothing to move */
    }

    uint8_t bus = ctrl->pci_dev.bus, dev = ctrl->pci_dev.device,
            fn  = ctrl->pci_dev.function;

    uint32_t usb3_mask = pci_config_read_dword(bus, dev, fn, USB_INTEL_USB3PRM);
    pci_config_write_dword(bus, dev, fn, USB_INTEL_USB3_PSSEN, usb3_mask);

    uint32_t usb2_mask = pci_config_read_dword(bus, dev, fn, USB_INTEL_USB2PRM);
    pci_config_write_dword(bus, dev, fn, USB_INTEL_XUSB2PR, usb2_mask);

    kprintf("[xHCI] Intel EHCI companion present — routed ports to xHCI "
            "(USB3 mask 0x%08x, USB2 mask 0x%08x)\n", usb3_mask, usb2_mask);
}

/*
 * Interrupt delivery. MSI first, for the same reason AHCI prefers it: it goes
 * point-to-point to the LAPIC and sidesteps INTx routing entirely. The PCI
 * interrupt-line byte is a legacy field the firmware is under no obligation to
 * keep accurate on an APIC system, and an xHCI controller whose interrupts
 * never arrive is a USB keyboard that does not type.
 *
 * The polling fallback is real, not a placeholder: xhci_poll_events runs off
 * the timer tick, so a controller with no usable interrupt still enumerates
 * and still delivers keystrokes, just later.
 */
static void xhci_setup_interrupts(xhci_controller_t* ctrl)
{
    /* Resolve the "usb:connect" / "usb:disconnect" Touch tags BEFORE any
     * interrupt can fire. Once enabled the controller may raise a port-change
     * interrupt at any time, and the IRQ-side publish path must find the
     * cached handles already populated or the first hot-plug event is lost. */
    xhci_interrupt_touch_init();

    /* MSI-X before MSI, because that is the order in which controllers
     * actually implement them. QEMU's xHCI offers MSI-X and no MSI at all; the
     * PCH offers both. Trying only MSI meant the emulated controller — the one
     * every test runs against — silently took the legacy path, so the path
     * real hardware uses was never the path being tested. */
    if (pci_msix_enable_vector(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                               ctrl->pci_dev.function, 0, XHCI_MSI_VECTOR,
                               g_amp.bsp_lapic_id) == 0) {
        ctrl->irq_vector  = XHCI_MSI_VECTOR;
        ctrl->use_msi     = true;
        ctrl->use_polling = false;
        kprintf("[xHCI] MSI-X enabled (vector 0x%02x -> LAPIC %u)\n",
                XHCI_MSI_VECTOR, g_amp.bsp_lapic_id);
        return;
    }

    if (pci_msi_enable(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                       ctrl->pci_dev.function, XHCI_MSI_VECTOR,
                       g_amp.bsp_lapic_id) == 0) {
        ctrl->irq_vector  = XHCI_MSI_VECTOR;
        ctrl->use_msi     = true;
        ctrl->use_polling = false;
        kprintf("[xHCI] MSI enabled (vector 0x%02x -> LAPIC %u)\n",
                XHCI_MSI_VECTOR, g_amp.bsp_lapic_id);
        return;
    }

    ctrl->irq_line = pci_config_read_byte(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                                          ctrl->pci_dev.function, 0x3C);

    if (ctrl->irq_line == 0xFF || ctrl->irq_line >= IRQ_MAX_COUNT) {
        kprintf("[xHCI] no MSI capability and no usable INTx line "
                "(0x%02x) — events will be polled from the timer tick\n",
                ctrl->irq_line);
        ctrl->use_polling = true;
        return;
    }

    irq_register_handler(ctrl->irq_line, xhci_irq_handler);
    irqchip_enable_irq(ctrl->irq_line);
    ctrl->use_polling = false;
    kprintf("[xHCI] INTx IRQ %u registered (MSI unavailable)\n", ctrl->irq_line);
}

int xhci_init(void) {
    xhci_controller_t* ctrl = &global_controller;

    debug_printf("[xHCI] Initializing xHCI driver...\n");

    spinlock_init(&ctrl->event_lock);
    xhci_command_init();
    xhci_enumeration_init();

    if (pci_find_device_by_class(0x0C, 0x03, 0x30, &ctrl->pci_dev) != 0) {
        debug_printf("[xHCI] No xHCI controller found\n");
        return -1;
    }

    debug_printf("[xHCI] Found controller: %02x:%02x.%x\n",
                 ctrl->pci_dev.bus, ctrl->pci_dev.device, ctrl->pci_dev.function);
    debug_printf("[xHCI]   Vendor: 0x%04x  Device: 0x%04x\n",
                 ctrl->pci_dev.vendor_id, ctrl->pci_dev.device_id);

    // Use 64-bit BAR read to correctly handle MMIO above 4GB
    uint64_t bar0_full = pci_read_bar64(&ctrl->pci_dev, 0);
    // Check raw BAR for I/O space bit
    uint32_t bar0_raw = pci_config_read_dword(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                                               ctrl->pci_dev.function, PCI_BAR0);
    if ((bar0_raw & 0x01) != 0) {
        debug_printf("[xHCI] ERROR: BAR0 is I/O space, expected MMIO\n");
        return -1;
    }

    ctrl->mmio_base_phys = bar0_full;
    /* One page, and only to read the capability registers — CAPLENGTH,
     * HCSPARAMS1, DBOFF and RTSOFF all live in the first 0x20 bytes. The real
     * extent is computed from those and mapped below.
     *
     * This used to be a flat 64 KB "default, refined after reading capability
     * registers", and nothing refined it. A guess about somebody else's
     * hardware is not a default; it is a number that happens to be right on
     * the machines you tried. On a Gigabyte B365 whose runtime registers sit
     * past 64 KB it was wrong, and the first write to ERSTSZ was a write to an
     * unmapped page.
     *
     * A probe window of one page also means the remap below runs on EVERY
     * controller, so the path is exercised by every boot instead of only on
     * the hardware that needs it — which is how it came to be untested in the
     * first place. */
    ctrl->mmio_size = 0x1000;
    debug_printf("[xHCI] MMIO base: 0x%llx (size: 0x%llx)\n",
                 ctrl->mmio_base_phys, ctrl->mmio_size);

    volatile void* mmio_virt = vmm_map_mmio(ctrl->mmio_base_phys, ctrl->mmio_size,
                                           VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    if (!mmio_virt) {
        debug_printf("[xHCI] ERROR: Failed to map MMIO region: %s\n", vmm_get_last_error());
        return -1;
    }

    debug_printf("[xHCI] MMIO mapped: phys=0x%llx -> virt=%p\n",
                 ctrl->mmio_base_phys, mmio_virt);

    ctrl->cap_regs = (xhci_cap_regs_t*)mmio_virt;

    uint32_t hc_capbase = ctrl->cap_regs->hc_capbase;
    uint8_t caplength = (uint8_t)XHCI_CAP_LENGTH(hc_capbase);

    if (caplength < 0x20) {
        debug_printf("[xHCI] ERROR: Invalid caplength 0x%02x (minimum 0x20)\n", caplength);
        debug_printf("[xHCI] MMIO mapping may have failed - check physical address\n");
        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);
        return -1;
    }

    uint16_t hciversion = (uint16_t)XHCI_CAP_VERSION(hc_capbase);

    // 0xFFFF indicates a bus error (MMIO read failure)
    if (hciversion == 0xFFFF) {
        debug_printf("[xHCI] ERROR: Invalid HCI version 0x%04x (MMIO read failure)\n", hciversion);
        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);
        return -1;
    }

    // 0x0000 may be valid for QEMU qemu-xhci pre-1.0
    if (hciversion == 0x0000) {
        debug_printf("[xHCI] WARNING: HCI version 0x0000 (pre-1.0 controller or MMIO issue)\n");
    }

    uint32_t hcsparams1 = ctrl->cap_regs->hcsparams1;
    ctrl->max_slots = XHCI_HCS1_MAX_SLOTS(hcsparams1);
    ctrl->max_ports = XHCI_HCS1_MAX_PORTS(hcsparams1);
    ctrl->max_interrupters = XHCI_HCS1_MAX_INTRS(hcsparams1);

    uint32_t hccparams1 = ctrl->cap_regs->hccparams1;
    ctrl->context_size = (hccparams1 & (1 << 2)) ? 64 : 32;
    debug_printf("[xHCI] Context size: %u bytes\n", ctrl->context_size);

    if (ctrl->max_slots == 0) {
        debug_printf("[xHCI] ERROR: Invalid max_slots %u\n", ctrl->max_slots);
        goto cleanup_resources;
    }

    if (ctrl->max_ports == 0) {
        debug_printf("[xHCI] ERROR: Invalid max_ports %u\n", ctrl->max_ports);
        goto cleanup_resources;
    }

    debug_printf("[xHCI] Version: %x.%02x\n", hciversion >> 8, hciversion & 0xFF);
    debug_printf("[xHCI] Max slots: %u  Max ports: %u  Max interrupters: %u\n",
                 ctrl->max_slots, ctrl->max_ports, ctrl->max_interrupters);

    uint32_t rtsoff = ctrl->cap_regs->rtsoff & 0xFFFFFFE0;
    uint32_t dboff  = ctrl->cap_regs->dboff  & 0xFFFFFFFC;

    /* How much of this controller do we actually have to reach?
     *
     * Ask the device. Writing all ones into BAR0 and reading it back gives the
     * size of the window it decodes — the whole of it, including the extended
     * capability list, which is the one region no arithmetic over RTSOFF and
     * DBOFF can bound because the list says where it ends only by being
     * walked. Deducing an extent from published offsets produces a lower
     * bound, and BoxOS has already taken a kernel page fault at exactly that
     * boundary on a Gigabyte B365.
     *
     * The three extents below stay as a floor, for the case where the BAR
     * probe comes back with nothing to say. They are the three furthest things
     * this driver touches, straight from the specification:
     *
     *   operational + 0x400 + MaxPorts * 0x10   the port register sets
     *   RTSOFF + 0x20 + MaxIntrs * 0x20         runtime + interrupter array
     *   DBOFF + (MaxSlots + 1) * 4              the doorbell array
     */
    uint64_t need_ports   = (uint64_t)caplength + 0x400
                          + (uint64_t)ctrl->max_ports * 0x10;
    uint64_t need_runtime = (uint64_t)rtsoff + 0x20
                          + (uint64_t)ctrl->max_interrupters * 0x20;
    uint64_t need_db      = (uint64_t)dboff
                          + ((uint64_t)ctrl->max_slots + 1) * 4;

    uint64_t need = need_ports;
    if (need_runtime > need) need = need_runtime;
    if (need_db      > need) need = need_db;

    /* A ceiling, because these are still numbers a device chose. Register
     * space this large is not a controller, and mapping it on the device's
     * say-so would be the kernel helping it. */
    const uint64_t XHCI_MMIO_SANE_MAX = 0x100000;   /* 1 MB */

    uint64_t bar_size = pci_bar_size(&ctrl->pci_dev, 0);
    if (bar_size >= need && bar_size <= XHCI_MMIO_SANE_MAX) {
        need = bar_size;
    } else if (bar_size != 0) {
        kprintf("[xHCI] BAR0 decodes 0x%llx bytes, outside the range this "
                "driver will map — using the computed extent 0x%llx instead\n",
                (unsigned long long)bar_size, (unsigned long long)need);
    }

    need = (need + 0xFFFULL) & ~0xFFFULL;

    if (need > XHCI_MMIO_SANE_MAX) {
        kprintf("[xHCI] ERROR: controller claims 0x%llx bytes of register "
                     "space (RTSOFF=0x%x DBOFF=0x%x ports=%u intrs=%u slots=%u) "
                     "— refusing\n", (unsigned long long)need, rtsoff, dboff,
                     ctrl->max_ports, ctrl->max_interrupters, ctrl->max_slots);
        goto cleanup_resources;
    }

    if (need > ctrl->mmio_size) {
        /* kprintf: this is a fact about the machine, and the machine is the
         * thing we do not know. It printed nothing on the board where the
         * absence of it cost a kernel #PF. */
        kprintf("[xHCI] register space: 0x%llx bytes "
                "(BAR0=0x%llx CAPLENGTH=0x%x RTSOFF=0x%x DBOFF=0x%x "
                "ports=%u intrs=%u slots=%u)\n",
                (unsigned long long)need, (unsigned long long)bar_size,
                caplength, rtsoff, dboff,
                ctrl->max_ports, ctrl->max_interrupters, ctrl->max_slots);

        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);

        mmio_virt = vmm_map_mmio(ctrl->mmio_base_phys, need,
                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
        if (!mmio_virt) {
            kprintf("[xHCI] ERROR: cannot map 0x%llx bytes of MMIO: %s\n",
                    (unsigned long long)need, vmm_get_last_error());
            return -1;
        }
        ctrl->mmio_size = need;
        ctrl->cap_regs  = (xhci_cap_regs_t*)mmio_virt;
    }

    ctrl->op_regs = (xhci_op_regs_t*)((uint8_t*)ctrl->cap_regs + caplength);
    ctrl->runtime_regs = (xhci_runtime_regs_t*)((uint8_t*)ctrl->cap_regs + rtsoff);
    ctrl->doorbells = (xhci_doorbell_array_t*)((uint8_t*)ctrl->cap_regs + dboff);
    ctrl->ports = (xhci_port_regs_t*)((uint8_t*)ctrl->op_regs + 0x400);

    /* Take the controller away from the firmware BEFORE resetting it.
     *
     * On this board, booted through CSM, the firmware owns the xHC and is
     * presenting the USB keyboard to the world as a PS/2 one through SMM.
     * Resetting the controller without the handshake ends that emulation while
     * the SMM handler still believes it is in charge — which is precisely how
     * a machine that typed fine in the boot menu stops typing the moment this
     * kernel loads. */
    if (xhci_claim_from_firmware(ctrl) != 0) {
        kprintf("[xHCI] ERROR: could not establish ownership of the "
                "controller\n");
        goto cleanup_resources;
    }

    /* Only now move the USB2 ports off an EHCI companion, on the chipsets that
     * have one. Rearranging a controller's ports while its firmware still
     * believes it owns the controller is asking two owners to agree; ownership
     * comes first, and then the ports. Nothing below can see a device on a
     * port that is still wired elsewhere. */
    xhci_intel_route_ports(ctrl);

    if (pci_enable_bus_master(&ctrl->pci_dev) != 0) {
        kprintf("[xHCI] ERROR: Failed to enable PCI bus mastering\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] PCI bus master enabled\n");

    if (xhci_reset(ctrl) != 0) {
        goto cleanup_resources;
    }

    /* Every DMA structure below is placed under 4 GB. That is a requirement
     * when the controller is not 64-bit capable and merely harmless when it
     * is, so it is stated once here rather than argued at each allocation. */
    if ((ctrl->cap_regs->hccparams1 & XHCI_HCC1_AC64) == 0) {
        debug_printf("[xHCI] controller is 32-bit addressing only\n");
    }

    if (xhci_ring_init(&ctrl->command_ring, 256, true) != 0) {
        kprintf("[xHCI] ERROR: Failed to allocate command ring\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] Command ring allocated at phys 0x%llx\n",
                 ctrl->command_ring.trbs_phys);

    if (xhci_ring_init(&ctrl->event_ring, 256, false) != 0) {
        kprintf("[xHCI] ERROR: Failed to allocate event ring\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] Event ring allocated at phys 0x%llx\n",
                 ctrl->event_ring.trbs_phys);

    if (xhci_erst_init(&ctrl->event_ring_segment_table, &ctrl->event_ring) != 0) {
        kprintf("[xHCI] ERROR: Failed to allocate ERST\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] ERST allocated at phys 0x%llx\n",
                 ctrl->event_ring_segment_table.entries_phys);

    void* dcbaa_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!dcbaa_phys) {
        kprintf("[xHCI] ERROR: Failed to allocate DCBAA\n");
        goto cleanup_resources;
    }

    ctrl->dcbaa = (xhci_dcbaa_t*)vmm_phys_to_virt((uintptr_t)dcbaa_phys);
    ctrl->dcbaa_phys = (uint64_t)dcbaa_phys;

    debug_printf("[xHCI] DCBAA allocated at phys 0x%llx\n", ctrl->dcbaa_phys);

    /* Slot ID zero can never name a device, so entry zero of the array is
     * where the controller expects the scratchpad it asked for. */
    if (xhci_alloc_scratchpad(ctrl) != 0) {
        goto cleanup_resources;
    }
    ctrl->dcbaa->device_context_ptrs[0] = ctrl->scratchpad_array_phys;

    /* Order matters here, and the old code had it wrong. The specification
     * lays out initialisation as: enable the slots, publish the device context
     * array, publish the command ring, then the interrupter — and within the
     * interrupter, ERSTSZ and ERDP before ERSTBA, because writing ERSTBA is
     * what makes the controller read the table. A controller told where the
     * table is before being told how big it is, or where software will read
     * from, is entitled to act on what it finds. QEMU forgives the order;
     * hardware need not. */
    ctrl->op_regs->config = ctrl->max_slots;
    ctrl->op_regs->dcbaap = ctrl->dcbaa_phys;
    ctrl->op_regs->crcr   = xhci_ring_get_phys_addr(&ctrl->command_ring)
                          | XHCI_CRCR_RCS;

    debug_printf("[xHCI] Operational registers programmed\n");

    ctrl->interrupters = &ctrl->runtime_regs->interrupters[0];

    xhci_interrupter_regs_t* intr0 = &ctrl->runtime_regs->interrupters[0];
    intr0->erstsz = 1;
    intr0->erdp   = xhci_ring_get_phys_addr(&ctrl->event_ring);
    intr0->erstba = xhci_erst_get_phys_addr(&ctrl->event_ring_segment_table);
    intr0->imod   = XHCI_IMOD_DEFAULT;
    intr0->iman   = XHCI_IMAN_IE;

    debug_printf("[xHCI] Primary interrupter configured\n");

    xhci_setup_interrupts(ctrl);

    /* Host System Error is enabled alongside the event interrupt: a controller
     * that has failed catastrophically should say so rather than go quiet. */
    ctrl->op_regs->usbcmd |= XHCI_CMD_INTE | XHCI_CMD_HSEE;

    if (xhci_start(ctrl) != 0) {
        goto cleanup_resources;
    }

    ctrl->running = true;
    ctrl->error_state = false;
    ctrl->initialized = true;

    /* Learn what the ports are before touching any of them. */
    xhci_map_port_protocols(ctrl);

    /* And give them power. On a controller with Port Power Control the ports
     * come out of reset unpowered, and an unpowered port reports no device no
     * matter what is plugged into it. QEMU powers its ports itself, which is
     * why a driver that never wrote this bit appeared to work. */
    xhci_power_ports(ctrl);

    kprintf("[xHCI] %x.%02x controller ready: %u port(s), %u slot(s), "
            "%s interrupts\n",
            hciversion >> 8, hciversion & 0xFF, ctrl->max_ports,
            ctrl->max_slots,
            ctrl->use_polling ? "polled" : (ctrl->use_msi ? "MSI" : "INTx"));

    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        if (xhci_port_has_device(ctrl, port)) {
            kprintf("[xHCI] port %u: device attached at boot (USB %u)\n",
                    port, ctrl->port_major[port]);
            xhci_enumerate_device(ctrl, port);
        }
    }

    return 0;

cleanup_resources:
    xhci_free_scratchpad(ctrl);

    if (ctrl->cap_regs) {
        vmm_unmap_mmio(ctrl->cap_regs, ctrl->mmio_size);
        ctrl->cap_regs = NULL;
        ctrl->op_regs = NULL;
        ctrl->runtime_regs = NULL;
        ctrl->doorbells = NULL;
        ctrl->ports = NULL;
    }

    if (ctrl->dcbaa_phys) {
        pmm_free((void*)ctrl->dcbaa_phys, 1);
        ctrl->dcbaa_phys = 0;
        ctrl->dcbaa = NULL;
    }

    xhci_ring_destroy(&ctrl->command_ring);
    xhci_ring_destroy(&ctrl->event_ring);
    xhci_erst_destroy(&ctrl->event_ring_segment_table);

    ctrl->initialized = false;
    return -1;
}
