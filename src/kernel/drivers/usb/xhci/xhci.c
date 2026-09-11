#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_device.h"
#include "xhci_interrupt.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_port.h"
#include "xhci_endpoint.h"
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

#define XHCI_MAX_CONTROLLERS 8

static xhci_controller_t g_controllers[XHCI_MAX_CONTROLLERS];
static uint8_t           g_controller_count = 0;

uint8_t xhci_controller_count(void) {
    return g_controller_count;
}

xhci_controller_t* xhci_controller_at(uint8_t index) {
    if (index >= g_controller_count) {
        return NULL;
    }
    return &g_controllers[index];
}

static uint8_t xhci_vector_for(const xhci_controller_t* ctrl)
{
    for (uint8_t i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        if (&g_controllers[i] == ctrl) {
            return (uint8_t)(XHCI_MSI_VECTOR + i);
        }
    }
    return XHCI_MSI_VECTOR;
}

xhci_controller_t* xhci_get_controller(void) {
    return xhci_controller_at(0);
}

#define XHCI_HALT_TIMEOUT_MS     32
#define XHCI_RESET_TIMEOUT_MS    1000
#define XHCI_READY_TIMEOUT_MS    1000
#define XHCI_START_TIMEOUT_MS    100

static bool xhci_wait_bit(volatile uint32_t* reg, uint32_t mask, bool want_set,
                          uint32_t timeout_ms)
{
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    for (;;) {
        uint32_t value = *reg;
        if (value == 0xFFFFFFFFu) {
            return false;
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

static int xhci_alloc_scratchpad(xhci_controller_t* ctrl)
{
    uint32_t count = XHCI_HCS2_MAX_SCRATCHPAD(ctrl->cap_regs->hcsparams2);
    ctrl->scratchpad_count = count;

    if (count == 0) {
        return 0;
    }

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
    kprintf("[xHCI %s] scratchpad: %u page(s) the controller asked for\n",
            ctrl->name, count);
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
        return;
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

static void xhci_setup_interrupts(xhci_controller_t* ctrl)
{
    xhci_interrupt_touch_init();

    if (pci_msix_enable_vector(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                               ctrl->pci_dev.function, 0, xhci_vector_for(ctrl),
                               g_amp.bsp_lapic_id) == 0) {
        ctrl->irq_vector  = xhci_vector_for(ctrl);
        ctrl->use_msi     = true;
        ctrl->use_polling = false;
        kprintf("[xHCI] MSI-X enabled (vector 0x%02x -> LAPIC %u)\n",
                xhci_vector_for(ctrl), g_amp.bsp_lapic_id);
        return;
    }

    if (pci_msi_enable(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                       ctrl->pci_dev.function, xhci_vector_for(ctrl),
                       g_amp.bsp_lapic_id) == 0) {
        ctrl->irq_vector  = xhci_vector_for(ctrl);
        ctrl->use_msi     = true;
        ctrl->use_polling = false;
        kprintf("[xHCI %s] MSI enabled (vector 0x%02x -> LAPIC %u)\n", ctrl->name,
                xhci_vector_for(ctrl), g_amp.bsp_lapic_id);
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

static void xhci_publish_structures(xhci_controller_t* ctrl)
{
    ctrl->op_regs->config = ctrl->max_slots;
    ctrl->op_regs->dcbaap = ctrl->dcbaa_phys;
    ctrl->op_regs->crcr   = xhci_ring_get_phys_addr(&ctrl->command_ring)
                          | XHCI_CRCR_RCS;

    ctrl->interrupters = &ctrl->runtime_regs->interrupters[0];

    xhci_interrupter_regs_t* intr0 = ctrl->interrupters;
    intr0->erstsz = 1;
    intr0->erdp   = xhci_ring_get_phys_addr(&ctrl->event_ring);
    intr0->erstba = xhci_erst_get_phys_addr(&ctrl->event_ring_segment_table);
    intr0->imod   = XHCI_IMOD_DEFAULT;
    intr0->iman   = XHCI_IMAN_IE;

    debug_printf("[xHCI] operational registers and interrupter programmed\n");
}

static void xhci_controller_recover(xhci_controller_t* ctrl)
{
    kprintf("[xHCI %s] putting the controller back in service\n", ctrl->name);

    for (unsigned slot_id = 1; slot_id <= ctrl->max_slots; slot_id++) {
        xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, (uint8_t)slot_id);
        if (slot) {
            xhci_slot_retire(ctrl, slot);
        }
    }
    xhci_slot_service(ctrl);

    ctrl->running     = false;
    ctrl->initialized = false;

    if (xhci_reset(ctrl) != 0) {
        kprintf("[xHCI %s] would not reset — out of service until the machine "
                "is restarted\n", ctrl->name);
        return;
    }

    xhci_command_init(ctrl);
    ctrl->command_ring.enqueue_idx = 0;
    ctrl->command_ring.dequeue_idx = 0;
    ctrl->command_ring.cycle_state = 1;
    if (ctrl->command_ring.trbs) {
        memset(ctrl->command_ring.trbs, 0,
               (size_t)ctrl->command_ring.num_trbs * sizeof(xhci_trb_t));
        xhci_trb_t* link = &ctrl->command_ring.trbs[ctrl->command_ring.num_trbs - 1];
        link->parameter = ctrl->command_ring.trbs_phys;
        link->status    = 0;
        link->control   = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC;
    }

    ctrl->event_ring.enqueue_idx = 0;
    ctrl->event_ring.dequeue_idx = 0;
    ctrl->event_ring.cycle_state = 1;
    if (ctrl->event_ring.trbs) {
        memset(ctrl->event_ring.trbs, 0,
               (size_t)ctrl->event_ring.num_trbs * sizeof(xhci_trb_t));
    }

    for (uint16_t i = 1; i < 256; i++) {
        ctrl->dcbaa->device_context_ptrs[i] = 0;
    }
    ctrl->dcbaa->device_context_ptrs[0] = ctrl->scratchpad_array_phys;

    xhci_publish_structures(ctrl);
    ctrl->op_regs->usbcmd |= XHCI_CMD_INTE | XHCI_CMD_HSEE;

    if (xhci_start(ctrl) != 0) {
        kprintf("[xHCI %s] would not start again — out of service\n",
                ctrl->name);
        return;
    }

    ctrl->running     = true;
    ctrl->initialized = true;
    ctrl->error_state = false;

    xhci_map_port_protocols(ctrl);
    xhci_power_ports(ctrl);

    kprintf("[xHCI %s] back in service — looking again at what is plugged "
            "in\n", ctrl->name);
    xhci_survey_root_ports(ctrl);
}

static volatile uint32_t g_repair_busy = 0;

bool xhci_put_back_in_service(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->cap_regs) {
        return false;
    }

    while (__atomic_exchange_n(&g_repair_busy, 1u, __ATOMIC_ACQUIRE) != 0) {
        cpu_pause();
    }

    xhci_controller_recover(ctrl);
    bool running = ctrl->running;

    __atomic_store_n(&g_repair_busy, 0u, __ATOMIC_RELEASE);
    return running;
}

void xhci_recover_if_needed(void)
{
    bool any = false;
    for (uint8_t i = 0; i < g_controller_count; i++) {
        if (g_controllers[i].error_state ||
            __atomic_load_n(&g_controllers[i].cmd_abort_wanted,
                            __ATOMIC_ACQUIRE)) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }

    if (__atomic_exchange_n(&g_repair_busy, 1u, __ATOMIC_ACQUIRE) != 0) {
        return;
    }

    for (uint8_t i = 0; i < g_controller_count; i++) {
        xhci_command_abort_if_wanted(&g_controllers[i]);
    }

    for (uint8_t i = 0; i < g_controller_count; i++) {
        if (g_controllers[i].error_state) {
            xhci_controller_recover(&g_controllers[i]);
        }
    }

    __atomic_store_n(&g_repair_busy, 0u, __ATOMIC_RELEASE);
}

void xhci_survey_root_ports(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized || ctrl->max_ports == 0) {
        return;
    }

    uint64_t debounce = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_POWER_SETTLE_MS);
    while ((int64_t)(rdtsc() - debounce) < 0) {
        cpu_pause();
    }

    uint8_t  seen[(XHCI_PORT_MAP_ENTRIES + 7) / 8];
    memset(seen, 0, sizeof(seen));
    unsigned found = 0;
    unsigned quiet_passes = 0;

    uint64_t ceiling = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_SURVEY_MS);
    while (quiet_passes < 2 && (int64_t)(rdtsc() - ceiling) < 0) {
        bool anything_new = false;

        for (unsigned port = 1; port <= ctrl->max_ports; port++) {
            if (seen[port >> 3] & (1u << (port & 7))) {
                continue;
            }
            if (!xhci_port_has_device(ctrl, (uint8_t)port)) {
                continue;
            }
            seen[port >> 3] |= (uint8_t)(1u << (port & 7));
            found++;
            anything_new = true;
            kprintf("[xHCI %s] port %u: device attached at boot (USB %u)\n",
                    ctrl->name, port, ctrl->port_major[port]);
            xhci_enumerate_device(ctrl, (uint8_t)port);
        }

        xhci_process_events();

        quiet_passes = anything_new ? 0 : (quiet_passes + 1);
        cpu_pause();
    }

    int unfinished = xhci_enum_settle(ctrl, XHCI_ENUM_TIMEOUT_MS +
                                            XHCI_CMD_TIMEOUT_MS);

    for (unsigned port = 1; port <= ctrl->max_ports; port++) {
        xhci_port_describe(ctrl, (uint8_t)port);
    }

    kprintf("[xHCI %s] %u of %u root port(s) had something on them; the "
            "deepest one drain went was %u event(s) of %u, and the longest "
            "held the ring for %u us\n",
            ctrl->name, found, ctrl->max_ports,
            ctrl->event_high_water, ctrl->event_ring.num_trbs,
            ctrl->drain_longest_us);

    if (unfinished > 0) {
        kprintf("[xHCI %s] %d of them were still being enumerated when the "
                "survey ended\n", ctrl->name, unfinished);
    }

}

static int xhci_bring_up(xhci_controller_t* ctrl) {

    debug_printf("[xHCI] Found controller: %02x:%02x.%x\n",
                 ctrl->pci_dev.bus, ctrl->pci_dev.device, ctrl->pci_dev.function);
    debug_printf("[xHCI]   Vendor: 0x%04x  Device: 0x%04x\n",
                 ctrl->pci_dev.vendor_id, ctrl->pci_dev.device_id);

    uint64_t bar0_full = pci_read_bar64(&ctrl->pci_dev, 0);
    uint32_t bar0_raw = pci_config_read_dword(ctrl->pci_dev.bus, ctrl->pci_dev.device,
                                               ctrl->pci_dev.function, PCI_BAR0);
    if ((bar0_raw & 0x01) != 0) {
        debug_printf("[xHCI] ERROR: BAR0 is I/O space, expected MMIO\n");
        return -1;
    }

    ctrl->mmio_base_phys = bar0_full;
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

    if (hciversion == 0xFFFF) {
        debug_printf("[xHCI] ERROR: Invalid HCI version 0x%04x (MMIO read failure)\n", hciversion);
        vmm_unmap_mmio(mmio_virt, ctrl->mmio_size);
        return -1;
    }

    if (hciversion == 0x0000) {
        debug_printf("[xHCI] WARNING: HCI version 0x0000 (pre-1.0 controller or MMIO issue)\n");
    }

    uint32_t hcsparams1 = ctrl->cap_regs->hcsparams1;
    ctrl->max_slots = XHCI_HCS1_MAX_SLOTS(hcsparams1);
    ctrl->max_ports = XHCI_HCS1_MAX_PORTS(hcsparams1);
    ctrl->max_interrupters = XHCI_HCS1_MAX_INTRS(hcsparams1);

    uint32_t hccparams1 = ctrl->cap_regs->hccparams1;
    ctrl->context_size = (hccparams1 & (1 << 2)) ? 64 : 32;



    if (ctrl->max_slots == 0) {
        debug_printf("[xHCI] ERROR: Invalid max_slots %u\n", ctrl->max_slots);
        goto cleanup_resources;
    }

    if (xhci_slots_attach(ctrl) != 0) {
        kprintf("[xHCI] no memory for %u device record(s)\n", ctrl->max_slots);
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

    uint64_t need_ports   = (uint64_t)caplength + 0x400
                          + (uint64_t)ctrl->max_ports * 0x10;
    uint64_t need_runtime = (uint64_t)rtsoff + 0x20
                          + (uint64_t)ctrl->max_interrupters * 0x20;
    uint64_t need_db      = (uint64_t)dboff
                          + ((uint64_t)ctrl->max_slots + 1) * 4;

    uint64_t need = need_ports;
    if (need_runtime > need) need = need_runtime;
    if (need_db      > need) need = need_db;

    const uint64_t XHCI_MMIO_SANE_MAX = 0x100000;

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
        kprintf("[xHCI %s] register space: 0x%llx bytes "
                "(BAR0=0x%llx CAPLENGTH=0x%x RTSOFF=0x%x DBOFF=0x%x "
                "ports=%u intrs=%u slots=%u)\n", ctrl->name,
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

    kprintf("[xHCI %s] caps: hcc1=0x%08x hcs1=0x%08x hcs2=0x%08x "
            "pagesize=0x%04x  contexts=%u bytes, addressing=%u-bit\n",
            ctrl->name, hccparams1,
            ctrl->cap_regs->hcsparams1, ctrl->cap_regs->hcsparams2,
            ctrl->op_regs->pagesize & 0xFFFFu,
            ctrl->context_size,
            (hccparams1 & XHCI_HCC1_AC64) ? 64 : 32);

    if (xhci_claim_from_firmware(ctrl) != 0) {
        kprintf("[xHCI] ERROR: could not establish ownership of the "
                "controller\n");
        goto cleanup_resources;
    }

    xhci_intel_route_ports(ctrl);

    if (pci_enable_bus_master(&ctrl->pci_dev) != 0) {
        kprintf("[xHCI] ERROR: Failed to enable PCI bus mastering\n");
        goto cleanup_resources;
    }

    debug_printf("[xHCI] PCI bus master enabled\n");

    if (xhci_reset(ctrl) != 0) {
        goto cleanup_resources;
    }

    if ((ctrl->cap_regs->hccparams1 & XHCI_HCC1_AC64) == 0) {
        debug_printf("[xHCI] controller is 32-bit addressing only\n");
    }

    if (xhci_ring_init(&ctrl->command_ring, XHCI_CMD_RING_TRBS, true) != 0) {
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

    if (xhci_alloc_scratchpad(ctrl) != 0) {
        goto cleanup_resources;
    }
    ctrl->dcbaa->device_context_ptrs[0] = ctrl->scratchpad_array_phys;

    xhci_publish_structures(ctrl);

    xhci_setup_interrupts(ctrl);

    ctrl->op_regs->usbcmd |= XHCI_CMD_INTE | XHCI_CMD_HSEE;

    if (xhci_start(ctrl) != 0) {
        goto cleanup_resources;
    }

    ctrl->running = true;
    ctrl->error_state = false;
    ctrl->initialized = true;

    xhci_map_port_protocols(ctrl);

    xhci_power_ports(ctrl);

    kprintf("[xHCI %s] %x.%02x controller ready: %u port(s), %u slot(s), "
            "%s interrupts\n",
            ctrl->name, hciversion >> 8, hciversion & 0xFF, ctrl->max_ports,
            ctrl->max_slots,
            ctrl->use_polling ? "polled" : (ctrl->use_msi ? "MSI" : "INTx"));

    kprintf("[xHCI %s] carries %u device record(s) of its own\n",
            ctrl->name, ctrl->slot_count);


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

    xhci_slots_release(ctrl);

    ctrl->initialized = false;
    return -1;
}

int xhci_init(void)
{
    xhci_enumeration_init();
    xhci_ep_context_self_test();

    for (uint32_t index = 0; index < XHCI_MAX_CONTROLLERS; index++) {
        pci_device_t dev;
        if (pci_find_nth_by_class(0x0C, 0x03, 0x30, index, &dev) != 0) {
            break;
        }

        xhci_controller_t* ctrl = &g_controllers[g_controller_count];
        memset(ctrl, 0, sizeof(*ctrl));
        ctrl->pci_dev = dev;
        spinlock_init(&ctrl->event_lock);
        ksnprintf(ctrl->name, sizeof(ctrl->name), "%02x:%02x.%u",
                  dev.bus, dev.device, dev.function);

        kprintf("[xHCI] controller at %02x:%02x.%u  %04x:%04x\n",
                dev.bus, dev.device, dev.function,
                dev.vendor_id, dev.device_id);

        xhci_command_init(ctrl);

        if (xhci_bring_up(ctrl) == 0) {
            g_controller_count++;
        } else {
            kprintf("[xHCI] the controller at %02x:%02x.%u did not come up — "
                    "carrying on with the others\n",
                    dev.bus, dev.device, dev.function);
        }
    }

    if (g_controller_count == 0) {
        kprintf("[xHCI] no usable xHCI controller on this machine\n");
        return -1;
    }

    kprintf("[xHCI] %u controller(s) in service\n", g_controller_count);

    for (uint8_t i = 0; i < g_controller_count; i++) {
        xhci_survey_root_ports(&g_controllers[i]);
    }
    return 0;
}