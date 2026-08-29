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

/*
 * Every xHCI controller on the machine, not the first one found.
 *
 * A desktop routinely has two: the one on the chipset, where the keyboard and
 * the sockets on the case are, and another on a graphics card driving its
 * USB-C port. PCI enumeration order decides which comes first and it is not
 * the one anybody means — measured on a live board, this driver took the
 * controller on the graphics card, reported six empty ports, and left the
 * keyboard and the boot drive on the chipset controller untouched. There is no
 * "the" USB controller to get hold of.
 */
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

/* The first controller in service. Kept for the callers that genuinely want
 * any one of them — a query, a diagnostic — and not for the ones that mean
 * "all of them", which now say so. */
/* The vector this controller signals on. Numbered alongside the controllers
 * themselves, so an interrupt says which one spoke. */
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

/*
 * Tell the controller where everything is.
 *
 * Order matters here, and the old code had it wrong. The specification lays
 * initialisation out as: enable the slots, publish the device context array,
 * publish the command ring, then the interrupter — and within the interrupter,
 * ERSTSZ and ERDP before ERSTBA, because writing ERSTBA is what makes the
 * controller read the table. A controller told where the table is before being
 * told how big it is, or where software will read from, is entitled to act on
 * what it finds. QEMU forgives the order; hardware need not.
 *
 * Separate from the allocation above it because a controller that has had to be
 * reset needs every word of this said to it again, and needs it said the same
 * way. A recovery that re-publishes in a different order from the boot is a
 * recovery that works on the machines it was written on.
 */
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

/*
 * Bringing a controller back — the whole of it, in one place.
 *
 * Host Controller Error and Host System Error both mean the same thing in
 * practice: the controller has hit something it cannot continue past and has
 * halted. The specification's answer is a reset — Section 4.24.1 — and until
 * now this driver's answer was to write the fact down and leave the machine
 * without USB until somebody power-cycled it. On a desktop that is a keyboard
 * that stops working; on a machine booting from a flash drive it is the
 * filesystem going away underneath itself.
 *
 * A reset loses every device: slots, addresses, endpoint state, all of it is
 * the controller's and the controller has just forgotten it. So everything is
 * retired first and the bus is surveyed again afterwards, which finds whatever
 * is still plugged in. The memory the driver holds — rings, contexts, the
 * scratchpad the controller asked for — is kept and re-published, because none
 * of it was what went wrong and re-allocating it is a second way to fail.
 *
 * ‼ A RESET IS NOT THIS, AND THE DIFFERENCE IS THE WHOLE POINT.
 *
 * `xhci_reset` halts the controller and clears it. That is one step of eleven.
 * Afterwards CONFIG, DCBAAP, CRCR and ERSTBA are zero, RUN is clear, and this
 * driver's own `running`, `initialized` and `error_state` still say the
 * controller is healthy — so nothing will ever come back for it, and every
 * device slot goes on claiming a device the silicon has forgotten. A caller
 * that wants a controller put right wants all eleven steps or none, and the
 * eleven are only correct in this order.
 *
 * Reached from ordinary kernel context, never from the handler that noticed:
 * this waits up to a second for a reset, and an interrupt handler is not
 * somewhere to spend a second.
 */
static void xhci_controller_recover(xhci_controller_t* ctrl)
{
    kprintf("[xHCI %s] putting the controller back in service\n", ctrl->name);

    /* Nothing may believe a device is still there. */
    /*
     * ‼ `slot_id` is wider than the eight-bit field it is counting over, and has to
     * be. MaxPorts and MaxSlots are both eight bits, so both may legitimately
     * be 255 — and `for (uint8_t i = 1; i <= 255; i++)` never ends: the
     * counter wraps to zero before the test can fail. Every board this has run
     * on reports twenty-four ports and sixty-four slots, which is exactly the
     * kind of number that makes a loop look correct for years.
     */
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

    /* The rings start over. The controller's own pointers into them are
     * reloaded from the registers below, so software's positions and cycle
     * states have to be back where the controller will be looking. */
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

    /* Slot zero still points at the scratchpad; every other entry named a
     * device context the controller has just forgotten. */
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

/*
 * The two repairs that take seconds, carried out where seconds may be spent.
 *
 * One core does them and the rest go away rather than queue up behind it — the
 * arrangement the hubs and the slot teardown already use, and for the same
 * reason. Called from the K-Core guide loop and from the idle loop, which is
 * where deferred work in this kernel actually runs.
 *
 * ‼ AND ON A ONE-CORE MACHINE, NEITHER OF THEM RUNS. The K-Core loop is only
 * entered when the machine is multi-core, and the idle process is never
 * scheduled on a single-core boot — measured by probe: with CORES=1 nothing in
 * this block is reached at all. The line that used to stand here said the
 * opposite, that cpu_idle was the single-core half of the arrangement; it is
 * not, and every scenario that needs this work already asks for four cores
 * because of it (tools/logcheck.sh). That hole is not this function's to close.
 *
 * ‼ The command-ring abort is here rather than where it is decided because
 * where it is decided is IRQ0. The specification allows a controller five
 * seconds to stop its ring (Section 4.6.1.2), and a timer interrupt that does
 * not return for five seconds does not send its end-of-interrupt for five
 * seconds either: the PIT stops being counted, the scheduler clock stops, key
 * repeat stops, Touch delivery stops, and both disk watchdogs stop. That is
 * the machine losing five seconds of its own time at the exact moment
 * something has already gone wrong with it.
 *
 * The abort comes FIRST, and that order is not cosmetic: taking the ring back
 * is what releases the devices whose commands were in flight, and the reset
 * below throws every device away regardless. Doing the reset first would
 * discard the one report that says which of them the controller had actually
 * addressed.
 */
/*
 * One core is inside a repair at a time, and the rest go away rather than
 * queue up behind it. Shared with the door below, because "put this controller
 * back in service" and "put back whichever controller asked to be" are the same
 * work reached by two routes, and two cores doing it at once would each reset a
 * controller the other is halfway through publishing.
 */
static volatile uint32_t g_repair_busy = 0;

/*
 * Put ONE named controller back in service, because somebody asked.
 *
 * The same eleven steps the automatic repair runs, on a controller that has
 * not necessarily failed — which is the whole difference between this and
 * xhci_recover_if_needed: that one asks "has anything gone wrong?", and this
 * one is told "put this right". A person at a machine whose USB has stopped
 * answering has no way to make the first question say yes; a controller wedged
 * in a way it does not report is exactly the case where nothing sets
 * error_state and nothing ever comes back for it.
 *
 * Returns whether the controller is running when this returns. False is a real
 * answer and not an exception: a controller that will not reset is out of
 * service, and it has already said so on its own line.
 *
 * ‼ Every USB device on this controller goes away and comes back, including
 * the medium a filesystem may be mounted from. That is not a side effect to be
 * apologised for — it is what a reset IS — and the machinery that carries a
 * volume through it already exists, because a hand pulling the stick out does
 * the same thing.
 */
bool xhci_put_back_in_service(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->cap_regs) {
        return false;
    }

    /* Waiting rather than refusing: a caller that asked for this wants it done,
     * and whoever is inside is doing exactly the work being asked for. The wait
     * is bounded by that work. */
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

/*
 * Look at the root ports until they have had the time the bus gives them.
 *
 * A controller that has just been reset has ports that were reset with it. A
 * USB 2 port must debounce a connection for 100 ms before it may be believed
 * (USB 2.0 §7.1.7.3), and a SuperSpeed port has to train its link, which is
 * instantaneous on no real silicon. The old code looked once, immediately
 * after the controller started, and a port that was not ready in that instant
 * was never looked at again — nothing rescans, and a device that was already
 * plugged in when the machine was switched on never "arrives", so no change
 * event is coming to correct the mistake.
 *
 * Under emulation a device is connected the moment the controller runs, which
 * is exactly why looking once appeared to be enough. On a real board it found
 * six empty ports with the flash drive the machine had just booted from
 * sitting in one of them.
 *
 * The waiting is shaped by what the bus actually requires rather than by a
 * number chosen to feel safe: the debounce is paid once because the
 * specification says it must be, and after that the survey ends as soon as two
 * passes running turn up nothing new. A machine with nothing plugged in pays
 * the debounce and leaves; a machine with a hub and a stick on it stays until
 * they have both spoken. The ceiling exists only so that a port wedged in a
 * link state it cannot leave costs a bounded amount of boot rather than all of
 * it.
 *
 * Every port is described afterwards either way. On a machine whose only
 * diagnostic is the screen, "powered, nothing attached, link Polling" and "NOT
 * powered" are different faults with different answers, and being able to tell
 * them apart from a photograph is the difference between one reflash and six.
 */
void xhci_survey_root_ports(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->initialized || ctrl->max_ports == 0) {
        return;
    }

    /* The debounce every connection is owed, paid once for all ports. */
    uint64_t debounce = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_POWER_SETTLE_MS);
    while ((int64_t)(rdtsc() - debounce) < 0) {
        cpu_pause();
    }

    /*
     * Which ports have already been enumerated in this survey.
     *
     * ‼ This was one uint32_t, and the loop below stopped at port 31 to match
     * it. MaxPorts is an eight-bit field: a controller may have up to 255 root
     * ports, and on one that does, every device plugged into a port above the
     * thirty-first was invisible to the boot — not reported, not enumerated,
     * not mentioned. Hot-plug would have found them afterwards; a machine
     * booting from a stick in one of those sockets would not have booted.
     *
     * One bit per port the register can name, which is thirty-two bytes of
     * stack and the end of the question.
     */
    uint8_t  seen[(XHCI_PORT_MAP_ENTRIES + 7) / 8];
    memset(seen, 0, sizeof(seen));
    unsigned found = 0;
    unsigned quiet_passes = 0;

    uint64_t ceiling = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_SURVEY_MS);
    while (quiet_passes < 2 && (int64_t)(rdtsc() - ceiling) < 0) {
        bool anything_new = false;

        /* `port` is wider than the field it is counting over on purpose —
         * see the note above xhci_survey_root_ports. */
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

        /* Enumeration is answered by events, and nothing else is draining them
         * yet. */
        xhci_process_events();

        quiet_passes = anything_new ? 0 : (quiet_passes + 1);
        cpu_pause();
    }

    /*
     * And now wait for the conversations this started to finish.
     *
     * Two quiet passes prove that no NEW port has anything on it. They prove
     * nothing whatever about the devices already found: at that moment their
     * port resets are still in flight, because a USB 2 reset takes tens of
     * milliseconds and two passes of this loop take microseconds. Returning
     * there meant the port descriptions below were a snapshot taken mid-reset
     * and the summary line counted sockets rather than devices — on a machine
     * whose only diagnostic is a photograph of the screen, the most important
     * lines in the log were the ones least entitled to be believed.
     *
     * Under emulation a reset completes inside the register write that starts
     * it, so those same three passes really did contain the whole enumeration,
     * and the difference did not exist.
     *
     * The budget is deliberately longer than either watchdog: whether a device
     * comes up is for them to decide and say out loud, not for this loop to
     * decide by running out of patience first. A machine where everything
     * answers leaves as soon as it has.
     */
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

    /* And every port described, one line each — the block that says whether a
     * socket is powered, whether anything is in it, and what link state it is
     * stuck in. On a board with twenty-four of them that is most of a screen. */
}

static int xhci_bring_up(xhci_controller_t* ctrl) {

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

    /* The capability words, said out loud once.
     *
     * Every one of these changes how the structures below are laid out, and
     * every one of them differs between the emulator this was written against
     * and the silicon it has to run on: 64-byte contexts, 64-bit addressing,
     * the page size the controller counts its scratchpad in. A controller that
     * stops with an internal error is a controller that read one of those
     * structures and found something it could not accept — and working out
     * which, from a photograph, means knowing what it said it wanted. */
    kprintf("[xHCI %s] caps: hcc1=0x%08x hcs1=0x%08x hcs2=0x%08x "
            "pagesize=0x%04x  contexts=%u bytes, addressing=%u-bit\n",
            ctrl->name, hccparams1,
            ctrl->cap_regs->hcsparams1, ctrl->cap_regs->hcsparams2,
            ctrl->op_regs->pagesize & 0xFFFFu,
            ctrl->context_size,
            (hccparams1 & XHCI_HCC1_AC64) ? 64 : 32);

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

    /* Slot ID zero can never name a device, so entry zero of the array is
     * where the controller expects the scratchpad it asked for. */
    if (xhci_alloc_scratchpad(ctrl) != 0) {
        goto cleanup_resources;
    }
    ctrl->dcbaa->device_context_ptrs[0] = ctrl->scratchpad_array_phys;

    xhci_publish_structures(ctrl);

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

    kprintf("[xHCI %s] %x.%02x controller ready: %u port(s), %u slot(s), "
            "%s interrupts\n",
            ctrl->name, hciversion >> 8, hciversion & 0xFF, ctrl->max_ports,
            ctrl->max_slots,
            ctrl->use_polling ? "polled" : (ctrl->use_msi ? "MSI" : "INTx"));

    /* Everything this controller said about itself is now on the screen —
     * capabilities, handoff, scratchpad, port power, protocols — and it is
     * about to be pushed off the top by the enumeration. */

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

int xhci_init(void)
{
    xhci_enumeration_init();
    xhci_ep_context_self_test();

    for (uint32_t index = 0; index < XHCI_MAX_CONTROLLERS; index++) {
        pci_device_t dev;
        if (pci_find_nth_by_class(0x0C, 0x03, 0x30, index, &dev) != 0) {
            break;                      /* seen them all */
        }

        xhci_controller_t* ctrl = &g_controllers[g_controller_count];
        memset(ctrl, 0, sizeof(*ctrl));
        ctrl->pci_dev = dev;
        spinlock_init(&ctrl->event_lock);
        ksnprintf(ctrl->name, sizeof(ctrl->name), "%02x:%02x.%u",
                  dev.bus, dev.device, dev.function);

        /* Named on the way in, so a photograph of the screen identifies which
         * silicon this is: a chipset controller and one on a graphics card
         * look identical in every line that follows. */
        kprintf("[xHCI] controller at %02x:%02x.%u  %04x:%04x\n",
                dev.bus, dev.device, dev.function,
                dev.vendor_id, dev.device_id);

        /* The command bookkeeping belongs to the controller and is sized by
         * its command ring, so it is set up with the controller and not once
         * for the machine. */
        xhci_command_init(ctrl);

        if (xhci_bring_up(ctrl) == 0) {
            g_controller_count++;
        } else {
            /* One controller failing is not the machine having no USB. */
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

    /* Surveyed only once every controller is running, so that a device on the
     * second one is not enumerated while the first is still being reset. */
    for (uint8_t i = 0; i < g_controller_count; i++) {
        xhci_survey_root_ports(&g_controllers[i]);
    }
    return 0;
}
