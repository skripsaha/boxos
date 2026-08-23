#include "xhci.h"
#include "xhci_input.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "xhci_interrupt.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_port.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "pit.h"
#include "idt.h"
#include "irqchip.h"
#include "pic.h"

static xhci_controller_t global_controller = {0};

xhci_controller_t* xhci_get_controller(void) {
    if (!global_controller.initialized) {
        return NULL;
    }
    return &global_controller;
}

int xhci_reset(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->op_regs) {
        return -1;
    }

    debug_printf("[xHCI] Starting controller reset...\n");

    ctrl->op_regs->usbcmd &= ~XHCI_CMD_RUN;

    uint32_t timeout = 16000;
    while (!(ctrl->op_regs->usbsts & XHCI_STS_HCH) && timeout > 0) {
        pit_sleep_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        debug_printf("[xHCI] ERROR: Timeout waiting for controller halt\n");
        return -1;
    }

    debug_printf("[xHCI] Controller halted\n");

    ctrl->op_regs->usbcmd |= XHCI_CMD_RESET;

    timeout = 16000;
    while ((ctrl->op_regs->usbcmd & XHCI_CMD_RESET) && timeout > 0) {
        pit_sleep_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        debug_printf("[xHCI] ERROR: Reset timeout\n");
        return -1;
    }

    timeout = 16000;
    while ((ctrl->op_regs->usbsts & XHCI_STS_CNR) && timeout > 0) {
        pit_sleep_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        debug_printf("[xHCI] ERROR: Controller not ready after reset\n");
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

    uint32_t timeout = 1000;
    while ((ctrl->op_regs->usbsts & XHCI_STS_HCH) && timeout > 0) {
        pit_sleep_ms(1);
        timeout--;
    }

    if (timeout == 0) {
        debug_printf("[xHCI] ERROR: Controller failed to start\n");
        return -1;
    }

    debug_printf("[xHCI] Controller running\n");
    return 0;
}

int xhci_init(void) {
    xhci_controller_t* ctrl = &global_controller;

    debug_printf("[xHCI] Initializing xHCI driver...\n");

    xhci_command_init();
    xhci_enumeration_init();
    UsbInput_Init();  // Initialize USB input subsystem

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

    uint8_t caplength = ctrl->cap_regs->caplength;

    if (caplength < 0x20) {
        debug_printf("[xHCI] ERROR: Invalid caplength 0x%02x (minimum 0x20)\n", caplength);
        debug_printf("[xHCI] MMIO mapping may have failed - check physical address\n");
        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);
        return -1;
    }

    uint16_t hciversion = ctrl->cap_regs->hciversion;

    debug_printf("[xHCI] MMIO diagnostic: cap_regs=%p hciversion_offset=%lu raw_value=0x%04x\n",
                 ctrl->cap_regs, __builtin_offsetof(xhci_cap_regs_t, hciversion), hciversion);

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
    uint32_t dboff = ctrl->cap_regs->dboff & 0xFFFFFFFC;

    /* How much of this controller do we actually have to reach?
     *
     * The 64 KB mapped above was a guess with a comment promising it would be
     * "refined after reading capability registers", and nothing refined it.
     * Every pointer below is computed from a register the CONTROLLER fills in
     * — RTSOFF, DBOFF, the port count — and none of them was checked against
     * the size of the window they land in. On a controller whose runtime
     * registers sit past 64 KB, the very first write to ERSTSZ is a write to
     * an unmapped page: BoxOS took exactly that on a Gigabyte B365 on
     * 2026-08-24, a kernel #PF at xhci_init+0x24d storing 1 into 0x28(%r12).
     *
     * A hardcoded size is a guess about somebody else's hardware. The
     * controller already tells us where its last register is; ask it, and map
     * to there. The three extents below are the three furthest things this
     * driver touches, straight from the xHCI specification:
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
    need = (need + 0xFFFULL) & ~0xFFFULL;

    /* A ceiling, because these are still numbers a device chose. The xHCI
     * specification caps the register space a controller may claim well below
     * this; anything asking for more is broken, and mapping gigabytes on its
     * say-so would be the kernel helping it. */
    const uint64_t XHCI_MMIO_SANE_MAX = 0x100000;   /* 1 MB */
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
                "(CAPLENGTH=0x%x RTSOFF=0x%x DBOFF=0x%x ports=%u intrs=%u slots=%u)\n",
                (unsigned long long)need, caplength, rtsoff, dboff,
                ctrl->max_ports, ctrl->max_interrupters, ctrl->max_slots);

        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);

        mmio_virt = vmm_map_mmio(ctrl->mmio_base_phys, need,
                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
        if (!mmio_virt) {
            debug_printf("[xHCI] ERROR: cannot map 0x%llx bytes of MMIO: %s\n",
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

    if (pci_enable_bus_master(&ctrl->pci_dev) != 0) {
        debug_printf("[xHCI] ERROR: Failed to enable PCI bus mastering\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] PCI bus master enabled\n");

    if (xhci_reset(ctrl) != 0) {
        goto cleanup_resources;
    }

    if (xhci_ring_init(&ctrl->command_ring, 256, true) != 0) {
        debug_printf("[xHCI] ERROR: Failed to allocate command ring\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] Command ring allocated at phys 0x%llx\n",
                 ctrl->command_ring.trbs_phys);

    if (xhci_ring_init(&ctrl->event_ring, 256, false) != 0) {
        debug_printf("[xHCI] ERROR: Failed to allocate event ring\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] Event ring allocated at phys 0x%llx\n",
                 ctrl->event_ring.trbs_phys);

    if (xhci_erst_init(&ctrl->event_ring_segment_table, &ctrl->event_ring) != 0) {
        debug_printf("[xHCI] ERROR: Failed to allocate ERST\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] ERST allocated at phys 0x%llx\n",
                 ctrl->event_ring_segment_table.entries_phys);

    void* dcbaa_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!dcbaa_phys) {
        debug_printf("[xHCI] ERROR: Failed to allocate DCBAA\n");
        goto cleanup_resources;
    }

    ctrl->dcbaa = (xhci_dcbaa_t*)vmm_phys_to_virt((uintptr_t)dcbaa_phys);
    ctrl->dcbaa_phys = (uint64_t)dcbaa_phys;

    debug_printf("[xHCI] DCBAA allocated at phys 0x%llx\n", ctrl->dcbaa_phys);

    ctrl->op_regs->dcbaap = ctrl->dcbaa_phys;
    ctrl->op_regs->crcr = xhci_ring_get_phys_addr(&ctrl->command_ring) | XHCI_CRCR_RCS;
    ctrl->op_regs->config = ctrl->max_slots;

    debug_printf("[xHCI] Operational registers programmed\n");

    ctrl->interrupters = &ctrl->runtime_regs->interrupters[0];

    xhci_interrupter_regs_t* intr0 = &ctrl->runtime_regs->interrupters[0];
    intr0->erstsz = 1;
    intr0->erstba = xhci_erst_get_phys_addr(&ctrl->event_ring_segment_table);
    intr0->erdp = xhci_ring_get_phys_addr(&ctrl->event_ring);
    intr0->iman = XHCI_IMAN_IE;

    debug_printf("[xHCI] Primary interrupter configured\n");

    ctrl->irq_line = pci_config_read_byte(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                                          ctrl->pci_dev.function, 0x3C);

    debug_printf("[xHCI] IRQ line from PCI config: 0x%02x\n", ctrl->irq_line);

    if (ctrl->irq_line == 0xFF || ctrl->irq_line >= IRQ_MAX_COUNT) {
        debug_printf("[xHCI] WARNING: Invalid IRQ line, using polling mode\n");
        ctrl->use_polling = true;
    } else {
        debug_printf("[xHCI] Registering IRQ handler for IRQ %u\n", ctrl->irq_line);

        /* Resolve and cache "usb:connect" / "usb:disconnect" Touch tags
         * BEFORE the IRQ handler can fire. Once registered + enabled,
         * the controller may raise port-change interrupts at any time,
         * and the IRQ-side TouchPublishIrqPair path MUST find the
         * cached handles already populated (otherwise the first hot-plug
         * event silently drops). */
        xhci_interrupt_touch_init();

        irq_register_handler(ctrl->irq_line, xhci_irq_handler);
        irqchip_enable_irq(ctrl->irq_line);
        ctrl->use_polling = false;
        debug_printf("[xHCI] IRQ %u registered and enabled\n", ctrl->irq_line);
    }

    ctrl->op_regs->usbcmd |= XHCI_CMD_INTE;
    debug_printf("[xHCI] Interrupts enabled in USBCMD\n");

    if (xhci_start(ctrl) != 0) {
        goto cleanup_resources;
    }

    ctrl->running = true;
    ctrl->error_state = false;
    ctrl->initialized = true;
    debug_printf("[xHCI] Initialization complete (running=%d, polling=%d)\n",
                 ctrl->running, ctrl->use_polling);

    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        if (xhci_port_has_device(ctrl, port)) {
            debug_printf("[xHCI] Device detected on port %u at boot, starting enumeration\n", port);
            xhci_enumerate_device(ctrl, port);
        }
    }

    return 0;

cleanup_resources:
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
