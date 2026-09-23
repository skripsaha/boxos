#include "pci.h"
#include "pci_class_words.h"  /* PciClassInWords — the class triple said in
                               * words, so the inventory line can be read off
                               * a photograph of a screen */
#include "io.h"
#include "klib.h"
#include "acpi.h"
#include "vmm.h"
#include "touch.h"
#include "aml.h"       /* AmlRouteForPciPin — the ACPI routing table, the
                        * only source here written about THIS machine in
                        * THIS interrupt mode */
#include "lapic.h"     /* LAPIC_XAPIC_DEST_MAX — an MSI destination is an
                        * APIC destination, and there is one width for both */

/* Configuration space has two doors, and only one of them needs a lock.
 *
 * The legacy door is two stages against ONE shared address register:
 *   outl(0xCF8, addr); inl/outl(0xCFC, ...);
 * If two cores interleave, one's data cycle runs against the other's address
 * cycle and reads — or writes — a different (bus,dev,fn,off) entirely. Every
 * legacy accessor below therefore holds g_pci_cfg_lock across BOTH cycles,
 * which is the whole of what this lock is for.
 *
 * The ECAM door is a single MMIO access at an address that already names the
 * function, so there is no shared selector to protect and the ECAM paths take
 * no lock at all. On a machine with an MCFG that is every access, so the lock
 * is not what makes those safe — the addressing is.
 *
 * What neither door offers is atomicity across a read and a later write of
 * the same register. None of the accessors below needs it any more: a word
 * write is a word write and a byte write is a byte write (see
 * pci_config_write_word / pci_config_write_byte). A caller that wants a
 * read-modify-write of one register has to arrange that itself. */
static spinlock_t g_pci_cfg_lock = {0};

/* ============================================================
 * ECAM (PCI Express Enhanced Configuration Access Mechanism)
 *
 * The PCI Firmware Specification 3.0 §4.1.2 fixes the per-device
 * address layout in the MCFG MMIO region:
 *
 *   ecam_phys(seg, bus, dev, fn, off) =
 *       segments[seg].base
 *       + ((bus - segments[seg].start_bus) << 20)
 *       + (dev << 15) + (fn << 12) + off
 *
 * A 256 MB window covers one full PCI segment (256 buses × 32 devices
 * × 8 functions × 4 KB). MCFG can publish multiple segments; we cache
 * the virtual mapping of each on pci_init().
 * ============================================================ */
typedef struct {
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uintptr_t phys_base;
    volatile uint8_t *virt_base;
} pci_ecam_segment_t;

static pci_ecam_segment_t g_ecam[ACPI_MCFG_MAX_SEGMENTS];
static uint8_t  g_ecam_count = 0;

bool pci_has_ecam(void) { return g_ecam_count > 0; }

static volatile uint8_t *ecam_locate(uint16_t segment, uint8_t bus,
                                     uint8_t device, uint8_t function,
                                     uint16_t offset) {
    if (device > 31 || function > 7 || offset > 0xFFF) return NULL;
    for (uint8_t i = 0; i < g_ecam_count; i++) {
        pci_ecam_segment_t* s = &g_ecam[i];
        if (s->segment != segment) continue;
        if (bus < s->start_bus || bus > s->end_bus) continue;
        if (!s->virt_base) continue;
        uintptr_t off =
            ((uintptr_t)(bus - s->start_bus) << 20) |
            ((uintptr_t)device   << 15) |
            ((uintptr_t)function << 12) |
            (uintptr_t)offset;
        return s->virt_base + off;
    }
    return NULL;
}

uint32_t pci_ecam_read_dword(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset) {
    volatile uint8_t* p = ecam_locate(segment, bus, device, function,
                                      offset & 0xFFCu);
    if (!p) return 0xFFFFFFFFu;
    return *(volatile uint32_t *)p;
}

void pci_ecam_write_dword(uint16_t segment, uint8_t bus, uint8_t device,
                          uint8_t function, uint16_t offset, uint32_t value) {
    volatile uint8_t* p = ecam_locate(segment, bus, device, function,
                                      offset & 0xFFCu);
    if (!p) return;
    *(volatile uint32_t *)p = value;
}

uint16_t pci_ecam_read_word(uint16_t segment, uint8_t bus, uint8_t device,
                             uint8_t function, uint16_t offset) {
    uint32_t aligned = pci_ecam_read_dword(segment, bus, device, function,
                                            offset & 0xFFCu);
    return (uint16_t)(aligned >> ((offset & 0x2u) * 8));
}

/* A word is written as a word and a byte as a byte — never as a dword.
 *
 * PCI Local Bus Specification Rev. 3.0 §6.2.3 "Device Status": the Status
 * register at offset 0x06 shares dword 0x04 with the Command register at
 * 0x04, and its error bits — Detected Parity Error (15), Signaled System
 * Error (14), Received Master Abort (13), Received Target Abort (12),
 * Signaled Target Abort (11), Master Data Parity Error (8) — are all
 * write-1-to-clear: "A one bit is reset (if it is not read-only) whenever
 * the register is written, and the write data in the corresponding bit
 * location is a 1." A read-modify-write of the containing dword reads those
 * bits and hands them straight back, which resets every error the device had
 * recorded. Nothing in BoxOS reads them today, so what that destroys is the
 * evidence — for a later AER or GHES reader, for firmware, for a post-mortem
 * — and it destroys it on every pci_enable_bus_master, every BAR-size probe
 * and every MSI/MSI-X arming.
 *
 * The same construction turns a one-byte write into a four-byte write of
 * three neighbours sampled at a different instant, which is the part that is
 * visible today: an AML OperationRegion Field declared ByteAcc or WordAcc on
 * PCI_Config (drivers/acpi/aml/aml.c) and an ACPI Generic Address Structure
 * write into PCI config space (drivers/acpi/acpi_shutdown.c — PM1 control and
 * the reset register) both land here, and a 4-byte write gives those methods
 * a side effect on the neighbouring bytes that their author never wrote.
 *
 * Linux never does it either: pci_generic_config_write issues
 * writeb/writew/writel (drivers/pci/access.c) and pci_mmcfg_write issues
 * mmio_config_writeb/writew/writel (arch/x86/pci/mmconfig_64.c) — a
 * native-width store, never a read-modify-write. */
void pci_ecam_write_word(uint16_t segment, uint8_t bus, uint8_t device,
                         uint8_t function, uint16_t offset, uint16_t value) {
    volatile uint8_t* p = ecam_locate(segment, bus, device, function,
                                      offset & 0xFFEu);
    if (!p) return;
    *(volatile uint16_t *)p = value;
}

uint8_t pci_ecam_read_byte(uint16_t segment, uint8_t bus, uint8_t device,
                            uint8_t function, uint16_t offset) {
    uint32_t aligned = pci_ecam_read_dword(segment, bus, device, function,
                                            offset & 0xFFCu);
    return (uint8_t)(aligned >> ((offset & 0x3u) * 8));
}

void pci_ecam_write_byte(uint16_t segment, uint8_t bus, uint8_t device,
                         uint8_t function, uint16_t offset, uint8_t value) {
    volatile uint8_t* p = ecam_locate(segment, bus, device, function, offset);
    if (!p) return;
    *p = value;
}

/* Map every MCFG segment that ACPI told us about. We map the full
 * (end_bus - start_bus + 1) * 1 MiB range as UC MMIO. On boards with
 * gargantuan segments (256 buses = 256 MB), that's still a one-off
 * VA cost — the mapping lives for the kernel lifetime, like the
 * other PCIe-class MMIO regions. */
static void pci_ecam_init(void) {
    g_ecam_count = 0;
    const acpi_mcfg_info_t* mcfg = acpi_get_mcfg();
    if (!mcfg) {
        /* Said with kprintf, because which door configuration space was read
         * through is a fact about the machine and not a debugging detail.
         *
         * The two doors do not reach the same thing. PCI Local Bus
         * Specification Rev. 3.0 §3.2.2.3.2 lays out CONFIG_ADDRESS at
         * 0xCF8: bits 7:2 are the Register Number and bits 1:0 must be zero,
         * which is six bits of dword index — 64 dwords, 256 bytes of a
         * function and not one byte more. Everything a PCIe function says
         * about itself beyond that (AER, ATS, ACS, and the extended
         * capability chain that anchors them at offset 0x100) is unreachable
         * through this door in principle, not by omission. */
        kprintf("[PCI] the firmware published no MCFG table, so every "
                "configuration access on this machine goes through the "
                "0xCF8/0xCFC window — six bits of dword index (PCI 3.0 "
                "§3.2.2.3.2), which reaches the first 256 bytes of a function "
                "and nothing above them, so no extended capability on this "
                "board can be read on this boot\n");
        return;
    }
    for (uint8_t i = 0; i < mcfg->count && g_ecam_count < ACPI_MCFG_MAX_SEGMENTS; i++) {
        const acpi_mcfg_segment_t* s = &mcfg->segments[i];
        size_t span = ((size_t)(s->end_bus - s->start_bus) + 1) * (1u << 20);
        volatile void* va = vmm_map_mmio((uintptr_t)s->base_address, span,
                                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                         VMM_FLAG_CACHE_DISABLE);
        if (!va) {
            /* The silent version of this failure was the whole defect: with
             * nothing said, a board whose ECAM never mapped behaves exactly
             * like a board whose ECAM did, because every register anyone
             * reads during boot — vendor, class, BARs, the MSI capability —
             * lives below offset 0x100 and the legacy door delivers it. What
             * is lost is invisible until something asks for an extended
             * capability and is told 0xFFFFFFFF, which reads as "absent". */
            kprintf("[PCI] %[E]segment %u published an ECAM window at 0x%lx "
                    "for buses %u..%u and this kernel could not map its %lu "
                    "MiB — that segment falls back to 0xCF8/0xCFC, which "
                    "reaches only the first 256 bytes of each function, so "
                    "every extended capability behind it now reads as "
                    "absent%[D]\n",
                    s->segment_group,
                    (unsigned long)s->base_address,
                    s->start_bus, s->end_bus,
                    (unsigned long)(span >> 20));
            continue;
        }
        pci_ecam_segment_t* slot = &g_ecam[g_ecam_count++];
        slot->segment   = s->segment_group;
        slot->start_bus = s->start_bus;
        slot->end_bus   = s->end_bus;
        slot->phys_base = (uintptr_t)s->base_address;
        slot->virt_base = (volatile uint8_t *)va;
        debug_printf("[PCI] ECAM seg=%u virt base %p\n",
                     s->segment_group, (void*)va);
        kprintf("[PCI] segment %u answers through ECAM: buses %u..%u at "
                "0x%lx, %lu MiB mapped uncached — the whole 4 KiB of every "
                "function's configuration space, extended capabilities "
                "included\n",
                s->segment_group, s->start_bus, s->end_bus,
                (unsigned long)s->base_address,
                (unsigned long)(span >> 20));
    }

    /* MCFG said ECAM exists and not one window could be mapped. Every read
     * below silently becomes a legacy read, so the machine still boots and
     * still looks right — which is exactly why it has to be said here. */
    if (g_ecam_count == 0) {
        kprintf("[PCI] %[E]MCFG named %u segment(s) and not one of their "
                "windows could be mapped — this kernel reads configuration "
                "space through 0xCF8/0xCFC on a machine whose firmware says "
                "it has ECAM%[D]\n", mcfg->count);
    }
}

static inline uint32_t pci_build_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return (uint32_t)(
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        ((uint32_t)offset & 0xFC) |
        0x80000000
    );
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    /* Fast path: ECAM for segment 0 when available. */
    volatile uint8_t* p = ecam_locate(0, bus, device, function,
                                      (uint16_t)(offset & 0xFCu));
    if (p) return *(volatile uint32_t *)p;

    uint32_t address = pci_build_address(bus, device, function, offset);
    spin_lock(&g_pci_cfg_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t v = inl(PCI_CONFIG_DATA);
    spin_unlock(&g_pci_cfg_lock);
    return v;
}

void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    volatile uint8_t* p = ecam_locate(0, bus, device, function,
                                      (uint16_t)(offset & 0xFCu));
    if (p) {
        *(volatile uint32_t *)p = value;
        return;
    }

    uint32_t address = pci_build_address(bus, device, function, offset);
    spin_lock(&g_pci_cfg_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
    spin_unlock(&g_pci_cfg_lock);
}

uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    return (uint16_t)(dword >> ((offset & 0x02) * 8));
}

/* Native-width word and byte writes on the legacy path too — see the note
 * above pci_ecam_write_word for why a read-modify-write of the containing
 * dword is not an option. 0xCF8 carries the dword-aligned address and the
 * width is chosen by which byte of the 0xCFC data port is addressed: Linux
 * writes `outw((u16)value, 0xCFC + (reg & 2))` and `outb((u8)value, 0xCFC +
 * (reg & 3))` after the same address cycle (pci_conf1_write,
 * arch/x86/pci/direct.c). Address cycle and data cycle are one critical
 * section, as they are for the dword accessors above. */
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    volatile uint8_t* p = ecam_locate(0, bus, device, function,
                                      (uint16_t)(offset & 0xFEu));
    if (p) {
        *(volatile uint16_t *)p = value;
        return;
    }

    uint32_t address = pci_build_address(bus, device, function, offset);
    spin_lock(&g_pci_cfg_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 0x02u)), value);
    spin_unlock(&g_pci_cfg_lock);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    return (uint8_t)(dword >> ((offset & 0x03) * 8));
}

void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
    volatile uint8_t* p = ecam_locate(0, bus, device, function, offset);
    if (p) {
        *p = value;
        return;
    }

    uint32_t address = pci_build_address(bus, device, function, offset);
    spin_lock(&g_pci_cfg_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    outb((uint16_t)(PCI_CONFIG_DATA + (offset & 0x03u)), value);
    spin_unlock(&g_pci_cfg_lock);
}

/* Recursive bus scan with cycle guard: a malformed or virtualised bridge
 * graph can advertise the same secondary bus from two different bridges,
 * or even point back at an already-visited bus, sending plain recursion
 * into an infinite loop and overflowing the kernel stack. The visited
 * bitmap (256 bits = 32 bytes) costs nothing and guarantees termination. */
static void pci_visited_clear(uint64_t visited[4]) {
    visited[0] = visited[1] = visited[2] = visited[3] = 0;
}

static bool pci_visited_test_set(uint64_t visited[4], uint8_t bus) {
    uint64_t mask = 1ULL << (bus & 63);
    uint64_t *slot = &visited[bus >> 6];
    if (*slot & mask) return true;   /* already visited */
    *slot |= mask;
    return false;
}

static int pci_scan_bus_impl(uint8_t bus, uint8_t class_code, uint8_t subclass,
                             uint8_t prog_if, pci_device_t* out,
                             uint64_t visited[4], uint32_t* skip) {
    if (pci_visited_test_set(visited, bus)) {
        debug_printf("[PCI] cycle guard: bus %u already visited, skipping\n", bus);
        return -1;
    }
    for (uint8_t device = 0; device < 32; device++) {
        for (uint8_t function = 0; function < 8; function++) {
            uint16_t vendor_id = pci_config_read_word(bus, device, function, PCI_VENDOR_ID);

            if (vendor_id == PCI_INVALID_VENDOR) {
                if (function == 0) break;
                continue;
            }

            uint16_t device_id = pci_config_read_word(bus, device, function, PCI_DEVICE_ID);
            uint8_t header_type = pci_config_read_byte(bus, device, function, PCI_HEADER_TYPE);
            uint8_t device_class = pci_config_read_byte(bus, device, function, PCI_CLASS_CODE);
            uint8_t device_subclass = pci_config_read_byte(bus, device, function, PCI_SUBCLASS);
            uint8_t device_prog_if = pci_config_read_byte(bus, device, function, PCI_PROG_IF);

            debug_printf("[PCI Scan] %02x:%02x.%u: Vendor=0x%04x Device=0x%04x Class=0x%02x/0x%02x/0x%02x\n",
                         bus, device, function, vendor_id, device_id,
                         device_class, device_subclass, device_prog_if);

            if (device_class == class_code &&
                device_subclass == subclass &&
                (prog_if == 0xFF || device_prog_if == prog_if)) {

                out->bus = bus;
                out->device = device;
                out->function = function;
                out->vendor_id = vendor_id;
                out->device_id = device_id;
                out->class_code = device_class;
                out->subclass = device_subclass;
                out->prog_if = device_prog_if;
                out->revision_id = pci_config_read_byte(bus, device, function, PCI_REVISION_ID);
                out->header_type = header_type & PCI_HEADER_TYPE_MASK;

                /* Matches before the one asked for are walked past rather than
                 * returned. A machine may hold several devices of one class —
                 * two xHCI controllers, say, one on the chipset and one on a
                 * graphics card — and "the first one found" names none of them
                 * in particular. */
                if (skip && *skip > 0) {
                    (*skip)--;
                } else

                return 0;
            }

            // Recurse into PCI-to-PCI bridges to find devices behind them
            if ((header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
                uint8_t secondary_bus = pci_config_read_byte(bus, device, function,
                                                              PCI_BRIDGE_SECONDARY_BUS);
                if (secondary_bus != 0 && secondary_bus != bus) {
                    debug_printf("[PCI] Bridge %02x:%02x.%u -> secondary bus %u\n",
                                 bus, device, function, secondary_bus);
                    int result = pci_scan_bus_impl(secondary_bus, class_code,
                                                   subclass, prog_if, out,
                                                   visited, skip);
                    if (result == 0) {
                        return 0;
                    }
                }
            }

            // Single-function device: skip remaining functions
            if ((header_type & PCI_HEADER_TYPE_MF) == 0 && function == 0) {
                break;
            }
        }
    }

    return -1;
}

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass,
                             uint8_t prog_if, pci_device_t* out) {
    return pci_find_nth_by_class(class_code, subclass, prog_if, 0, out);
}

int pci_find_nth_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                          uint32_t index, pci_device_t* out) {
    if (!out) {
        return -1;
    }
    uint32_t skip = index;

    // Check if host bridge is multi-function (multiple PCI domains)
    uint8_t host_header = pci_config_read_byte(0, 0, 0, PCI_HEADER_TYPE);

    uint64_t visited[4];
    pci_visited_clear(visited);

    if (host_header & PCI_HEADER_TYPE_MF) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint16_t vid = pci_config_read_word(0, 0, fn, PCI_VENDOR_ID);
            if (vid == PCI_INVALID_VENDOR) continue;

            int result = pci_scan_bus_impl(fn, class_code, subclass, prog_if,
                                           out, visited, &skip);
            if (result == 0) return 0;
        }
    } else {
        return pci_scan_bus_impl(0, class_code, subclass, prog_if, out, visited,
                                 &skip);
    }

    return -1;
}

int pci_enable_bus_master(pci_device_t* device) {
    if (!device) {
        return -1;
    }

    uint16_t command = pci_config_read_word(device->bus, device->device, device->function, PCI_COMMAND);
    command |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
    pci_config_write_word(device->bus, device->device, device->function, PCI_COMMAND, command);

    uint16_t verify = pci_config_read_word(device->bus, device->device, device->function, PCI_COMMAND);
    if ((verify & PCI_CMD_BUS_MASTER) == 0) {
        return -1;
    }

    return 0;
}

uint32_t pci_read_bar(pci_device_t* device, uint8_t bar_num) {
    if (!device || bar_num > 5) {
        return 0;
    }

    uint8_t bar_offset = PCI_BAR0 + (bar_num * 4);
    uint32_t bar_value = pci_config_read_dword(device->bus, device->device, device->function, bar_offset);

    if (bar_value & 0x01) {
        return bar_value & 0xFFFFFFFC;  // I/O space
    }

    return bar_value & 0xFFFFFFF0;      // MMIO (32-bit portion only)
}

uint64_t pci_read_bar64(pci_device_t* device, uint8_t bar_num) {
    if (!device || bar_num > 5) {
        return 0;
    }

    uint8_t bar_offset = PCI_BAR0 + (bar_num * 4);
    uint32_t bar_low = pci_config_read_dword(device->bus, device->device,
                                              device->function, bar_offset);

    // I/O space BAR
    if (bar_low & 0x01) {
        return (uint64_t)(bar_low & 0xFFFFFFFC);
    }

    // MMIO BAR: check type field (bits [2:1])
    uint8_t bar_type = (bar_low >> 1) & 0x03;

    if (bar_type == PCI_BAR_TYPE_64BIT) {
        if (bar_num >= 5) {
            debug_printf("[PCI] WARNING: BAR5 claims 64-bit but no next BAR\n");
            return (uint64_t)(bar_low & 0xFFFFFFF0);
        }

        uint8_t bar_hi_offset = PCI_BAR0 + ((bar_num + 1) * 4);
        uint32_t bar_high = pci_config_read_dword(device->bus, device->device,
                                                   device->function, bar_hi_offset);

        uint64_t addr = ((uint64_t)bar_high << 32) | (uint64_t)(bar_low & 0xFFFFFFF0);
        return addr;
    }

    // 32-bit MMIO BAR
    return (uint64_t)(bar_low & 0xFFFFFFF0);
}

/*
 * pci_bar_size — how large is the window this BAR decodes?
 *
 * The device answers the question itself. Write all ones into the BAR and read
 * it back: every address bit the device does not decode reads back as zero, so
 * the size is the complement of what remains, plus one. This is the only way
 * to learn the extent — nothing in configuration space states it outright, and
 * a driver that instead deduces the extent from offsets the device published
 * (RTSOFF, a port count) is deducing a lower bound and calling it a size.
 * BoxOS paid for that once already: an xHCI controller whose extended
 * capabilities sat past the deduced end was a kernel page fault waiting for
 * the right motherboard.
 *
 * Memory decode is turned off around the probe. While the all-ones value sits
 * in the BAR the device claims a window it does not own, and a bus access that
 * lands there in the meantime reaches the wrong device.
 *
 * Returns 0 for an unimplemented BAR, an I/O-space BAR, or a device that
 * decodes nothing.
 */
uint64_t pci_bar_size(pci_device_t* device, uint8_t bar_num) {
    if (!device || bar_num > 5) {
        return 0;
    }

    uint8_t off = (uint8_t)(PCI_BAR0 + (bar_num * 4));
    uint32_t orig_lo = pci_config_read_dword(device->bus, device->device,
                                             device->function, off);

    if (orig_lo == 0xFFFFFFFFu || orig_lo == 0) {
        return 0;                       /* unimplemented */
    }
    if (orig_lo & 0x01u) {
        return 0;                       /* I/O space — callers here want MMIO */
    }

    bool is64 = (((orig_lo >> 1) & 0x03u) == PCI_BAR_TYPE_64BIT);
    if (is64 && bar_num >= 5) {
        return 0;                       /* claims 64-bit with no upper half */
    }

    uint8_t off_hi = (uint8_t)(off + 4);
    uint32_t orig_hi = is64 ? pci_config_read_dword(device->bus, device->device,
                                                    device->function, off_hi)
                            : 0;

    uint16_t cmd = pci_config_read_word(device->bus, device->device,
                                        device->function, PCI_COMMAND);
    pci_config_write_word(device->bus, device->device, device->function,
                          PCI_COMMAND,
                          (uint16_t)(cmd & ~(uint16_t)PCI_CMD_MEM_SPACE));

    pci_config_write_dword(device->bus, device->device, device->function,
                           off, 0xFFFFFFFFu);
    uint32_t mask_lo = pci_config_read_dword(device->bus, device->device,
                                             device->function, off);
    uint32_t mask_hi = 0xFFFFFFFFu;
    if (is64) {
        pci_config_write_dword(device->bus, device->device, device->function,
                               off_hi, 0xFFFFFFFFu);
        mask_hi = pci_config_read_dword(device->bus, device->device,
                                        device->function, off_hi);
        pci_config_write_dword(device->bus, device->device, device->function,
                               off_hi, orig_hi);
    }
    pci_config_write_dword(device->bus, device->device, device->function,
                           off, orig_lo);

    pci_config_write_word(device->bus, device->device, device->function,
                          PCI_COMMAND, cmd);

    /* The low four bits are the BAR's type field, not address bits. */
    uint64_t mask = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & 0xFFFFFFF0u);
    if (!is64) {
        mask |= 0xFFFFFFFF00000000ULL;
    }
    if (mask == 0 || mask == ~0ULL) {
        return 0;
    }

    return ~mask + 1ULL;
}

/* ============================================================
 * Capability list walkers
 * ============================================================ */

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t cap_id) {
    uint16_t status = pci_config_read_word(bus, device, function, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;

    uint8_t off = pci_config_read_byte(bus, device, function, PCI_CAP_LIST_PTR);
    off &= 0xFCu;
    /* PCI 3.0 §6.7: cap pointer points into 0x40..0xFC. Bound the walk
     * so a malformed firmware can't drive us into a loop. */
    for (uint8_t hops = 0; off != 0 && hops < 64; hops++) {
        if (off < 0x40 || off > 0xFCu) return 0;
        uint8_t id   = pci_config_read_byte(bus, device, function, off);
        uint8_t next = pci_config_read_byte(bus, device, function,
                                              (uint8_t)(off + 1));
        if (id == cap_id) return off;
        if (next == 0) return 0;
        off = next & 0xFCu;
    }
    return 0;
}

/* The same chain, read through the window the function actually lives behind.
 *
 * pci_find_capability() above is the segment-0 API and pci.h says so: it
 * reads through pci_config_read_*, whose ECAM fast path is hardcoded to
 * segment 0 and whose fallback is 0xCF8/0xCFC — a mechanism with no concept
 * of a segment at all. The ECAM enumerator walks every segment group the
 * MCFG published, and PCI Firmware Specification Rev. 3.0 §4.1.2 gives one
 * base address per segment group: a (bus, dev, fn) means nothing without the
 * group it belongs to. So the enumerator needs the walk with the segment
 * carried through. Same anchor, same bounds (PCI 3.0 §6.7), different door —
 * the two cannot be folded into one without taking the legacy fallback away
 * from machines that have no MCFG. */
static uint8_t pci_find_capability_seg(uint16_t segment, uint8_t bus,
                                        uint8_t device, uint8_t function,
                                        uint8_t cap_id) {
    uint16_t status = pci_ecam_read_word(segment, bus, device, function,
                                          PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;

    uint8_t off = pci_ecam_read_byte(segment, bus, device, function,
                                      PCI_CAP_LIST_PTR);
    off &= 0xFCu;
    for (uint8_t hops = 0; off != 0 && hops < 64; hops++) {
        if (off < 0x40 || off > 0xFCu) return 0;
        uint8_t id   = pci_ecam_read_byte(segment, bus, device, function, off);
        uint8_t next = pci_ecam_read_byte(segment, bus, device, function,
                                           (uint16_t)(off + 1));
        if (id == cap_id) return off;
        if (next == 0) return 0;
        off = next & 0xFCu;
    }
    return 0;
}

uint16_t pci_find_ext_capability(uint16_t segment, uint8_t bus, uint8_t device,
                                  uint8_t function, uint16_t cap_id) {
    if (!pci_has_ecam()) return 0;
    /* PCIe Base §7.9: extended cap list starts at offset 0x100, every
     * entry has 16-bit ID + 4-bit version + 12-bit next-ptr; ptr 0 ends
     * the chain. The first DWORD at 0x100 == 0xFFFFFFFF means "no caps". */
    uint16_t off = 0x100;
    for (uint16_t hops = 0; off != 0 && hops < 256; hops++) {
        uint32_t hdr = pci_ecam_read_dword(segment, bus, device, function, off);
        if (hdr == 0xFFFFFFFFu || hdr == 0) return 0;
        uint16_t id   = (uint16_t)(hdr & 0xFFFFu);
        uint16_t next = (uint16_t)((hdr >> 20) & 0xFFFu);
        if (id == cap_id) return off;
        if (next == 0) return 0;
        off = next;
    }
    return 0;
}

/* ============================================================
 * MSI / MSI-X programming
 *
 * Both use the standard Intel xAPIC "compatibility format":
 *   Message Address = 0xFEE_{dest:8}_0000 (bits 19:12 = dest APIC ID)
 *   Message Data    = vector | (delivery_mode << 8)  ; we use fixed=0.
 * MSI-X tables live in MMIO at (BAR.bir + offset); each entry is 16 bytes:
 *   +0   addr_lo
 *   +4   addr_hi
 *   +8   data
 *   +12  vector control (bit 0 = masked)
 * ============================================================ */

static inline uint32_t msi_data(uint8_t vector) {
    return (uint32_t)vector;       /* fixed delivery, edge, physical, no RH */
}

/*
 * The message address, or the reason there is none.
 *
 * Bits 19:12 of the Message Address are the whole Destination ID in this
 * format — eight bits, and Intel SDM Vol 3A, "Message Address Register
 * Format", says they are the same eight the IOAPIC uses: "Destination ID —
 * This field contains an 8-bit destination ID. It identifies the message's
 * target processor(s). The destination ID corresponds to bits 63:56 of the
 * I/O APIC Redirection Table Entry if the IOAPIC is used to dispatch the
 * interrupt to the processor(s)." It is also the same field the xAPIC ICR
 * has. Linux spells the
 * layout out as a bitfield (arch/x86/include/asm/msi.h: `destid_0_7 : 8`
 * in x86_msi_addr_lo, `destid_8_31 : 24` only in x86_msi_addr_hi) and
 * says in __irq_msi_compose_msg what the high bits cost: "Only the IOMMU
 * itself can use the trick of putting destination APIC ID into the high
 * bits of the address. Anything else would just be writing to memory if
 * it tried that, and needs IR to address APICs which can't be addressed
 * in the normal 32-bit address range at 0xFFExxxxx." Its own compatibility
 * path ends in `WARN_ON_ONCE(cfg->dest_apicid > 0xFF)`.
 *
 * Truncating such an ID does not lose the interrupt, it MISDELIVERS it:
 * the low eight bits name a different, running processor, which takes a
 * vector whose handler belongs to somebody else's device — while the
 * device that was armed waits for an interrupt that is answered elsewhere.
 * Shifting it in whole is worse still: `0xFEE00000 | (0x100 << 12)` is
 * 0xFEF00000, outside the message window entirely, and the device's write
 * lands on whatever decodes that address.
 *
 * Refusal and composition are ONE function for that reason. The test and
 * the arithmetic are the same statement about the same eight bits, and
 * splitting them leaves an address-builder that a later caller can reach
 * without passing the test — the invariant would then rest on discipline
 * instead of on there being no other way in. There is no other way in.
 *
 * Lifting the refusal takes interrupt remapping (VT-d IR / AMD-Vi), whose
 * IRTE carries the full 32-bit destination and turns this address into an
 * index. Until that is armed there is no honest eight-bit answer here.
 */
static bool msi_address_for(uint32_t dest_lapic_id, const char *what,
                            uint8_t bus, uint8_t device, uint8_t function,
                            uint32_t *out_addr_lo) {
    if (dest_lapic_id > LAPIC_XAPIC_DEST_MAX) {
        /* Two different accidents, told apart. Above 0xFF the high bits are
         * simply gone; at exactly 0xFF every bit survives and the field
         * means something else — SDM Vol 3A, section "Physical Destination
         * Mode", gives FFH in physical destination mode to "a broadcast to
         * all APICs", so the device would interrupt every processor on this
         * machine at once, on a vector whose handler is written for one.
         * The MSI text confirms in its own words that FFH is not any one
         * processor's ID — "Message Address Register Format", field 3 (RH):
         * "When RH is 1 and the physical destination mode is used, the
         * Destination ID field must not be set to FFH; it must point to a
         * processor that is present and enabled to receive the interrupt."
         * This composer leaves RH clear, so that sentence is not the rule
         * being enforced here; it is a second statement, from the MSI
         * chapter itself, of what FFH is. (Cited by title; see
         * LAPIC_XAPIC_DEST_MAX in lapic.h for the editions.) */
        if (dest_lapic_id == LAPIC_XAPIC_DEST_BROADCAST) {
            kprintf("[PCI] %[E]%s not armed on %02x:%02x.%u: APIC ID 255 is "
                    "the broadcast Destination ID of the compatibility "
                    "message address, so this device would interrupt EVERY "
                    "processor instead of one%[D]\n",
                    what, bus, device, function);
        } else {
            kprintf("[PCI] %[E]%s not armed on %02x:%02x.%u: APIC ID %u does "
                    "not fit the 8-bit Destination ID of the compatibility "
                    "message address, and the low eight bits name the core "
                    "with ID %u instead%[D]\n",
                    what, bus, device, function, dest_lapic_id,
                    dest_lapic_id & 0xFFu);
        }
        return false;
    }

    *out_addr_lo = 0xFEE00000u | (dest_lapic_id << 12);
    return true;
}

int pci_msi_enable(uint8_t bus, uint8_t device, uint8_t function,
                    uint8_t vector, uint32_t dest_lapic_id) {
    uint32_t addr_lo;
    if (!msi_address_for(dest_lapic_id, "MSI", bus, device, function, &addr_lo))
        return -5;

    uint8_t cap = pci_find_capability(bus, device, function, PCI_CAP_ID_MSI);
    if (cap == 0) return -1;

    /* Mask INTx so the device cannot fire both at once. */
    uint16_t cmd = pci_config_read_word(bus, device, function, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write_word(bus, device, function, PCI_COMMAND, cmd);

    uint16_t mc = pci_config_read_word(bus, device, function, (uint8_t)(cap + 2));
    bool is64 = (mc & (1u << 7)) != 0;

    pci_config_write_dword(bus, device, function, (uint8_t)(cap + 4), addr_lo);
    if (is64) {
        pci_config_write_dword(bus, device, function, (uint8_t)(cap + 8), 0);
        pci_config_write_word(bus, device, function, (uint8_t)(cap + 12),
                                (uint16_t)msi_data(vector));
    } else {
        pci_config_write_word(bus, device, function, (uint8_t)(cap + 8),
                                (uint16_t)msi_data(vector));
    }

    /* Allocate one vector (MMC stays 000 = 1 message), set enable bit 0. */
    mc &= ~(uint16_t)0x70u;        /* MME=0 -> 1 vector */
    mc |= 0x0001u;                  /* MSI enable */
    pci_config_write_word(bus, device, function, (uint8_t)(cap + 2), mc);
    return 0;
}

int pci_msix_enable_vector(uint8_t bus, uint8_t device, uint8_t function,
                            uint16_t table_index, uint8_t vector,
                            uint32_t dest_lapic_id) {
    uint32_t addr_lo;
    if (!msi_address_for(dest_lapic_id, "MSI-X", bus, device, function,
                         &addr_lo))
        return -5;

    uint8_t cap = pci_find_capability(bus, device, function, PCI_CAP_ID_MSIX);
    if (cap == 0) return -1;

    uint16_t mc        = pci_config_read_word(bus, device, function,
                                                 (uint8_t)(cap + 2));
    uint32_t table_off = pci_config_read_dword(bus, device, function,
                                                  (uint8_t)(cap + 4));
    uint16_t table_sz  = (uint16_t)((mc & 0x7FFu) + 1u);

    if (table_index >= table_sz) {
        debug_printf("[PCI] MSI-X index %u >= table size %u\n",
                     table_index, table_sz);
        return -2;
    }

    /* The BAR Indicator Register sits in the low 3 bits; the rest is a
     * dword-aligned byte offset into that BAR. */
    uint8_t bir = (uint8_t)(table_off & 0x7u);
    uint32_t off = table_off & ~0x7u;

    pci_device_t dev = {
        .bus = bus, .device = device, .function = function
    };
    uint64_t bar = pci_read_bar64(&dev, bir);
    if (bar == 0) {
        debug_printf("[PCI] MSI-X BAR%u is zero\n", bir);
        return -3;
    }

    /* Map the table region — sized for declared entries × 16 bytes,
     * rounded up to a page so DMA pages stay aligned. */
    size_t span = (size_t)table_sz * 16u;
    span = (span + 4095u) & ~(size_t)4095u;
    volatile uint8_t *table = (volatile uint8_t *)vmm_map_mmio(
        (uintptr_t)bar + off, span,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE);
    if (!table) {
        debug_printf("[PCI] MSI-X table mmio map failed\n");
        return -4;
    }

    volatile uint32_t *entry = (volatile uint32_t *)(table + table_index * 16u);
    entry[0] = addr_lo;                      /* addr lo */
    entry[1] = 0;                            /* addr hi */
    entry[2] = msi_data(vector);             /* data */
    entry[3] = 0;                            /* vector control: unmasked */

    /* Disable INTx, ensure bus-mastering, enable MSI-X. */
    uint16_t cmd = pci_config_read_word(bus, device, function, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write_word(bus, device, function, PCI_COMMAND, cmd);

    mc |= 0x8000u;                          /* MSI-X enable */
    mc &= ~(uint16_t)0x4000u;               /* function mask = 0 */
    pci_config_write_word(bus, device, function, (uint8_t)(cap + 2), mc);
    return 0;
}

/*
 * The bridge that owns `bus`, or false when nothing does.
 *
 * Two facts bound this search, and neither is a guess. PCI-to-PCI Bridge
 * Architecture Specification 1.2 §3.2.5.5: "the secondary bus number must be
 * greater than the primary bus number" — so bus 0 is never behind a bridge
 * at all, and a bridge that owns bus B is always on a bus numbered below B.
 * Everything above B is therefore not searched, and for the common case of a
 * device on bus 0 nothing is searched.
 */
static bool PciParentBridgeOf(uint8_t bus, uint8_t *pbus, uint8_t *pdev,
                               uint8_t *pfn) {
    if (bus == 0) return false;
    for (uint32_t b = 0; b < bus; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            if (pci_config_read_word((uint8_t)b, d, 0, PCI_VENDOR_ID) ==
                PCI_INVALID_VENDOR)
                continue;
            uint8_t hdr0 = pci_config_read_byte((uint8_t)b, d, 0,
                                                PCI_HEADER_TYPE);
            uint8_t fn_count = (hdr0 & PCI_HEADER_TYPE_MF) ? 8u : 1u;
            for (uint8_t f = 0; f < fn_count; f++) {
                if (f && pci_config_read_word((uint8_t)b, d, f,
                                              PCI_VENDOR_ID) ==
                         PCI_INVALID_VENDOR)
                    continue;
                uint8_t hdr = pci_config_read_byte((uint8_t)b, d, f,
                                                   PCI_HEADER_TYPE);
                if ((hdr & PCI_HEADER_TYPE_MASK) != PCI_HEADER_TYPE_BRIDGE)
                    continue;
                if (pci_config_read_byte((uint8_t)b, d, f,
                                         PCI_BRIDGE_SECONDARY_BUS) != bus)
                    continue;
                *pbus = (uint8_t)b;
                *pdev = d;
                *pfn  = f;
                return true;
            }
        }
    }
    return false;
}

/*
 * Ask the ACPI routing table where this function's INTx pin goes.
 *
 * _PRT describes the bus directly beneath the object that carries it, so the
 * question cannot be asked with a bus number alone: the path from the root
 * bus down to this function is what decides which table answers and how many
 * times the pin is rotated on the way. That path is a fact about config
 * space, which is this file's to read; which namespace object describes each
 * level is aml.c's to know.
 *
 * The walk goes UP — this function, then the bridge that owns its bus, and so
 * on — and the array is reversed before it is handed over. It cannot run
 * away: every step marks a bus in the same visited bitmap the scanner uses,
 * a bus already marked is a loop in the bridge graph and is refused aloud,
 * and a PCI bus number is eight bits (PCI 3.0 §3.2.2.3.2) so there are 256 of
 * them and no step can be taken twice.
 */
#define PCI_BUS_NUMBER_COUNT 256   /* the bus number field is eight bits */

static bool PciRouteFor(uint8_t bus, uint8_t device, uint8_t function,
                         uint8_t intx_pin, AmlRoute *out) {
    AmlPciStep step[PCI_BUS_NUMBER_COUNT];
    uint32_t   depth = 0;
    uint64_t   visited[4];
    pci_visited_clear(visited);

    uint8_t cur_bus = bus, cur_dev = device, cur_fn = function;
    for (;;) {
        if (pci_visited_test_set(visited, cur_bus)) {
            kprintf("[PCI] %[E]the bridge graph leads back to bus %u — this "
                    "function's path to a root bus is a loop, and its "
                    "interrupt routing is refused rather than followed "
                    "round%[D]\n", cur_bus);
            return false;
        }
        step[depth].device   = cur_dev;
        step[depth].function = cur_fn;
        depth++;

        uint8_t pb = 0, pd = 0, pf = 0;
        if (!PciParentBridgeOf(cur_bus, &pb, &pd, &pf)) break;
        cur_bus = pb;
        cur_dev = pd;
        cur_fn  = pf;
    }

    for (uint32_t i = 0, j = depth - 1; i < j; i++, j--) {
        AmlPciStep swap = step[i];
        step[i] = step[j];
        step[j] = swap;
    }

    /* Segment 0: the legacy 0xCF8/0xCFC door and the ECAM window this file
     * maps for it address one segment group, and the PCI Firmware
     * Specification 3.0 §4.1.2 gives every other segment its own base. A
     * root bridge that declares a different _SEG will not match, which is
     * the right answer rather than a wrong one. */
    return AmlRouteForPciPin(0, cur_bus, step, depth, intx_pin, out);
}

bool PciSummons(uint8_t bus, uint8_t device, uint8_t function, Summons *out) {
    if (!out) return false;

    /* §6.2.4, Interrupt Pin: "A value of 1 corresponds to INTA#. A value of 2
     * corresponds to INTB#. A value of 3 corresponds to INTC#. A value of 4
     * corresponds to INTD#. Devices (or device functions) that do not use an
     * interrupt pin must put a 0 in this register. The values 05h through FFh
     * are reserved."
     *
     * Asked FIRST, and it never was before. A function that reports 0 has no
     * INTx wire at all, and its Interrupt Line byte means nothing — whatever
     * POST happened to leave there was read as a GSI and a pin was unmasked
     * for a device that can never drive it. */
    uint8_t pin = pci_config_read_byte(bus, device, function,
                                       PCI_INTERRUPT_PIN);
    if (pin == 0) {
        kprintf("[PCI] %02x:%02x.%u has no INTx line: its Interrupt Pin "
                "register reads 0, which PCI 3.0 §6.2.4 defines as a function "
                "that does not use an interrupt pin\n", bus, device, function);
        return false;
    }
    if (pin > 4) {
        kprintf("[PCI] %[E]%02x:%02x.%u reports Interrupt Pin %u, which PCI "
                "3.0 §6.2.4 reserves — there is no INTx# wire that value "
                "names%[D]\n", bus, device, function, pin);
        return false;
    }

    /* ‼ THE ROUTING TABLE FIRST, because it is the only source here that was
     * written about THIS machine in THIS interrupt mode. Everything below is
     * the fallback, and it stays exactly as strict as it was: a board that
     * does not answer leaves this function reading the byte the firmware
     * wrote for the 8259, and the refusal is printed by the code that
     * refused. Nothing is silently substituted in either direction. */
    uint32_t        gsi      = 0;
    SummonsSource   gsi_from = SUMMONS_FROM_NOBODY;
    SummonsPolarity polarity = SUMMONS_ACTIVE_LOW;
    SummonsTrigger  trigger  = SUMMONS_LEVEL;
    SummonsSource   wiring_from = SUMMONS_FROM_BUS_RULE;

    AmlRoute route;
    if (PciRouteFor(bus, device, function, pin, &route)) {
        gsi      = route.gsi;
        gsi_from = SUMMONS_FROM_ROUTING_TABLE;
        if (route.wiring_known) {
            /* Only a link's own _CRS describes the wire. A directly numbered
             * entry says which GSI and nothing else, and the PCI bus rule
             * already answers the rest — inventing a polarity from silence
             * is the defect the Summons exists to close. */
            polarity    = route.active_low ? SUMMONS_ACTIVE_LOW
                                           : SUMMONS_ACTIVE_HIGH;
            trigger     = route.level ? SUMMONS_LEVEL : SUMMONS_EDGE;
            wiring_from = SUMMONS_FROM_ROUTING_TABLE;
        }
        kprintf("[PCI] %02x:%02x.%u INT%c# is routed to GSI %u by %s\n",
                bus, device, function, 'A' + (pin - 1), gsi,
                route.named_by ? route.named_by : "the ACPI routing table");
    } else {
        uint8_t line = pci_config_read_byte(bus, device, function,
                                            PCI_INTERRUPT_LINE);
        if (line == 0xFF) {
            kprintf("[PCI] %02x:%02x.%u drives INT%c# but its Interrupt Line "
                    "byte is 255, which PCI 3.0 §6.2.4 footnote 43 defines as "
                    "\"unknown\" or \"no connection\" — the firmware is saying "
                    "it does not know where this wire goes\n",
                    bus, device, function, 'A' + (pin - 1));
            return false;
        }
        if (line > 15) {
            /* Footnote 43 again: "Values between 15 and 254 are reserved."
             * Taken anyway, and said out loud, because firmware on an APIC
             * machine routinely writes the resolved GSI into a field the
             * specification only defines for the 8259s. This is the one value
             * in the whole description with no document behind it, which is
             * precisely why the Summons records where it came from — and why
             * _PRT, when a board answers it, is believed over this. */
            kprintf("[PCI] %02x:%02x.%u: Interrupt Line reads %u, a value PCI "
                    "3.0 §6.2.4 footnote 43 reserves — read as a GSI because "
                    "firmware on an APIC machine writes one there, and only "
                    "because the routing table did not answer\n",
                    bus, device, function, line);
        }
        gsi      = line;
        gsi_from = SUMMONS_FROM_INTERRUPT_LINE;
    }

    /* There is no range test here any more, and the reason it went is worth
     * writing down. It used to refuse any GSI at or above twenty-four,
     * because idt_init installed gates for exactly that many vectors and a
     * line past them had nowhere to be delivered. Both halves of that have
     * changed: every vector now has a gate, and a line is given one from the
     * pool when it settles rather than by adding thirty-two to its pin
     * number. So the only question left is the chip's own — does an IOAPIC on
     * this machine have this input — and SummonsPinIsReachable asks it, out
     * of the VER register, at arming time. A routing table that names a pin
     * on a part with a hundred and twenty inputs is therefore taken, where
     * it used to be refused for a reason that was about this kernel rather
     * than about the part.
     *
     * ‼ A PIN ON A SECOND IOAPIC IS STILL REFUSED, AND THIS COMMENT USED TO
     * CLAIM IT WAS NOT. Exactly one chip is brought up: acpi_madt.c keeps
     * the FIRST I/O APIC Structure the firmware names and only counts the
     * rest, irqchip.c hands that one register address and that one Global
     * System Interrupt base to ioapic_init, and ioapic.c holds one of each.
     * A GSI belonging to any other chip — and a desktop PCH publishes a
     * second the moment firmware enables a root port's own I/OxAPIC — gets
     * this far, and is then turned away by SummonsPinIsReachable as below
     * that one chip's GSI base or past its last pin. That refusal is true
     * about the pin and silent about the cause, which is that one entry out
     * of N was read; the count acpi_madt.c prints is where the cause can be
     * seen. */

    /* §6.2.2, Table 6-1, bit 10: "This bit disables the device/function from
     * asserting INTx#. A value of 0 enables the assertion of its INTx#
     * signal. A value of 1 disables the assertion of its INTx# signal."
     * Unmasking an IOAPIC pin for a function that cannot drive its wire arms
     * nothing, and both callers reach this only after an MSI attempt — the
     * one path in this file that deliberately sets that bit. */
    uint16_t cmd = pci_config_read_word(bus, device, function, PCI_COMMAND);
    if (cmd & PCI_CMD_INT_DISABLE) {
        pci_config_write_word(bus, device, function, PCI_COMMAND,
                              (uint16_t)(cmd & ~(uint16_t)PCI_CMD_INT_DISABLE));
    }

    Summons line_desc = {
        .gsi         = gsi,
        /* No ISA override is applied to this number. The MADT Interrupt
         * Source Override table describes ISA sources — the kernel's own
         * ioapic.c says so where it keeps it — and remapping a PCI line
         * through it would land the pin wherever some unrelated legacy IRQ
         * was moved to. The honest translation from (device, pin) to GSI is
         * _PRT's, and it is the one asked for above. */
        /* Given when the line settles, not here -- see SummonsForIsaLine. */
        .vector      = 0,
        /* A PCI INTx line belongs to a device with a transfer in flight far
         * more often than not, and a device whose interrupt waits is a device
         * whose own deadline expires. A driver that knows better sets this
         * itself before arming. */
        .row         = RIGHT_OF_WAY_WORK_IS_REDONE,
        .intx_pin    = pin,
        .polarity    = polarity,
        .trigger     = trigger,
        /* Shareable whatever the source says. A link's _CRS may state
         * "exclusive", and PCI 3.0 §6.5 states the opposite about every INTx
         * wire: "PCI interrupts are shareable. PCI device drivers are
         * required to support shared interrupts". The descriptor describes
         * one link's allocation; the bus rule describes the wire. */
        .shared      = true,
        .gsi_from    = gsi_from,
        .wiring_from = wiring_from,
        .bus         = "PCI",
    };
    *out = line_desc;
    return true;
}

/* Decode a PCIe Express capability (cap 0x10) to a one-line summary
 * — link width and current speed taken from the Link Status register
 * at cap+0x12. */
static void log_pcie_link(uint16_t segment, uint8_t bus, uint8_t dev,
                          uint8_t fn, uint8_t cap_off) {
    uint16_t link_status = pci_ecam_read_word(segment, bus, dev, fn,
                                                (uint16_t)(cap_off + 0x12));
    uint8_t speed_code = (uint8_t)(link_status & 0xF);
    uint8_t width      = (uint8_t)((link_status >> 4) & 0x3F);
    const char* speed = "?";
    switch (speed_code) {
        case 1: speed = "2.5 GT/s"; break;
        case 2: speed = "5.0 GT/s"; break;
        case 3: speed = "8.0 GT/s"; break;
        case 4: speed = "16 GT/s";  break;
        case 5: speed = "32 GT/s";  break;
        case 6: speed = "64 GT/s";  break;
    }
    debug_printf("[PCI]     PCIe link: x%u @ %s\n", width, speed);
}

/* ============================================================
 * The inventory walk, and the door it went through.
 *
 * Which door a function was read through is a fact about the machine, so it
 * travels with the function and is printed on its line. The two doors do not
 * reach the same space: the ECAM window of PCI Firmware Specification Rev.
 * 3.0 §4.1.2 addresses all 4 KiB of a function's configuration space, while
 * CONFIG_ADDRESS/CONFIG_DATA (PCI 3.0 §3.2.2.3.2) carries six bits of dword
 * index and addresses 256 bytes of it. A function read through the second
 * door has no extended capability chain — not "none found", none reachable.
 * ============================================================ */
typedef enum {
    PCI_DOOR_ECAM,   /* MMIO, one window per segment group, offsets 0x000..0xFFF */
    PCI_DOOR_PORTS,  /* 0xCF8/0xCFC, segment 0 only, offsets 0x00..0xFF */
} PciDoor;

static const char *PciDoorName(PciDoor door) {
    return door == PCI_DOOR_ECAM ? "ECAM" : "0xCF8/0xCFC";
}

/* PCI_DOOR_PORTS has no segment to carry: the legacy address register (PCI
 * 3.0 §3.2.2.3.2) holds bus, device, function and register number and
 * nothing else, so it can only ever describe segment 0. The walk passes
 * segment 0 with it for that reason, and never any other. */
static uint16_t PciDoorWord(PciDoor door, uint16_t segment, uint8_t bus,
                            uint8_t dev, uint8_t fn, uint16_t off) {
    if (door == PCI_DOOR_ECAM) {
        return pci_ecam_read_word(segment, bus, dev, fn, off);
    }
    return pci_config_read_word(bus, dev, fn, (uint8_t)off);
}

static uint8_t PciDoorByte(PciDoor door, uint16_t segment, uint8_t bus,
                           uint8_t dev, uint8_t fn, uint16_t off) {
    if (door == PCI_DOOR_ECAM) {
        return pci_ecam_read_byte(segment, bus, dev, fn, off);
    }
    return pci_config_read_byte(bus, dev, fn, (uint8_t)off);
}

static uint8_t PciDoorCapability(PciDoor door, uint16_t segment, uint8_t bus,
                                 uint8_t dev, uint8_t fn, uint8_t cap_id) {
    if (door == PCI_DOOR_ECAM) {
        return pci_find_capability_seg(segment, bus, dev, fn, cap_id);
    }
    return pci_find_capability(bus, dev, fn, cap_id);
}

/* What the walk counts, so its closing line can say it. */
typedef struct {
    uint32_t functions;
    uint32_t bridges;
    uint32_t buses;
} PciWalkTally;

/* How this function can be summoned, in one clause.
 *
 * The Interrupt Pin register is the whole of what a function says about its
 * INTx wire — PCI 3.0 §6.2.4: "A value of 1 corresponds to INTA# ... Devices
 * (or device functions) that do not use an interrupt pin must put a 0 in
 * this register. The values 05h through FFh are reserved."
 *
 * A zero on its own reads like a fault, and on most of a modern board it is
 * the normal answer — so the message capabilities are named in the same
 * breath, because they are what turns "no pin" into either "it interrupts by
 * message" or "nothing on it can interrupt at all". Both come from the
 * capability chain this walk has already walked, so neither costs a further
 * configuration cycle. */
static void PciInterruptClause(uint8_t pin, bool has_msi, bool has_msix,
                               char *out, size_t out_len) {
    const char *msg = has_msix ? (has_msi ? "MSI and MSI-X" : "MSI-X")
                               : (has_msi ? "MSI" : NULL);
    if (pin >= 1 && pin <= 4) {
        if (msg) {
            ksnprintf(out, out_len,
                      "wired to INT%c#, and it can also interrupt by %s",
                      'A' + (pin - 1), msg);
        } else {
            ksnprintf(out, out_len,
                      "wired to INT%c#, which is the only way it can interrupt",
                      'A' + (pin - 1));
        }
    } else if (pin == 0) {
        if (msg) {
            ksnprintf(out, out_len,
                      "with no INTx pin, so %s is the only way it can "
                      "interrupt", msg);
        } else {
            ksnprintf(out, out_len,
                      "with neither an INTx pin nor a message capability, so "
                      "nothing on it can raise an interrupt");
        }
    } else {
        ksnprintf(out, out_len,
                  "reporting Interrupt Pin %u, a value PCI 3.0 §6.2.4 "
                  "reserves, so it names no INTx# wire", pin);
    }
}

/* Walk one (segment, bus, dev, fn) — only valid if vendor != 0xFFFF.
 *
 * Every register here is read through the door the caller came in by, and
 * through the SEGMENT's own ECAM window when that door is ECAM. That is the
 * point of the function taking a segment at all. PCI Firmware Specification
 * Rev. 3.0 §4.1.2 allocates one ECAM base address per PCI Segment Group: a
 * (bus, dev, fn) is only an address together with its group, and two groups
 * routinely both start at bus 0. Reading through the segment-0 API instead —
 * which is what pci_config_read_* is, by its own contract in pci.h — would
 * identify a segment-1 function from whatever segment 0 happens to hold at
 * the same coordinates: either 0xFFFF, and the function silently vanishes
 * from the count, or a different device, whose vendor, class and
 * capabilities then get published under the right address as
 * `pci:vendor:...` / `pci:class:...`. The legacy door is taken only when
 * there is no ECAM window at all, and it has no segment but 0 to describe.
 *
 * ‼ The one line per function is kprintf, not debug_printf, and that is the
 * whole reason it exists. DEBUG is off in a shipped build, CONFIG_DEBUG_ENABLED
 * is then 0, and debug_printf expands to `do { if (0) kprintf(...); } while (0)`
 * — so this kernel used to walk every function on the board and say nothing
 * about a single one of them. A board where a controller failed to appear
 * then looked exactly like a board where it had never been on a bus. */
static void enumerate_function(PciDoor door, uint16_t segment, uint8_t bus,
                                uint8_t dev, uint8_t fn, PciWalkTally* tally) {
    uint16_t vendor = PciDoorWord(door, segment, bus, dev, fn, PCI_VENDOR_ID);
    if (vendor == PCI_INVALID_VENDOR) return;

    uint16_t devid = PciDoorWord(door, segment, bus, dev, fn, PCI_DEVICE_ID);
    uint8_t  cls   = PciDoorByte(door, segment, bus, dev, fn, PCI_CLASS_CODE);
    uint8_t  sub   = PciDoorByte(door, segment, bus, dev, fn, PCI_SUBCLASS);
    uint8_t  prog  = PciDoorByte(door, segment, bus, dev, fn, PCI_PROG_IF);
    uint8_t  htype = PciDoorByte(door, segment, bus, dev, fn, PCI_HEADER_TYPE);
    uint8_t  pin   = PciDoorByte(door, segment, bus, dev, fn,
                                 PCI_INTERRUPT_PIN);

    /* The capability list is asked for BEFORE the line is printed, because
     * what can summon this function belongs in the same sentence as its pin. */
    uint8_t msi_off  = PciDoorCapability(door, segment, bus, dev, fn,
                                         PCI_CAP_ID_MSI);
    uint8_t msix_off = PciDoorCapability(door, segment, bus, dev, fn,
                                         PCI_CAP_ID_MSIX);
    uint8_t pcie_off = PciDoorCapability(door, segment, bus, dev, fn,
                                         PCI_CAP_ID_PCIE);

    char what[96];
    PciClassInWords(cls, sub, prog, what, sizeof(what));

    /* A bridge that does not say which buses are behind it is not described:
     * it is the reason the next bus in this list exists at all. PCI-to-PCI
     * Bridge Architecture Specification Rev. 1.2 §3.2.5.4 and §3.2.5.5 put
     * the Secondary Bus Number at offset 0x19 and the Subordinate Bus Number
     * at 0x1A of a header-type-1 function, and together they name every bus
     * downstream of it. A CardBus bridge (header type 2) numbers its buses
     * in different registers, so it is not read here and says only what it
     * is rather than a range taken from the wrong offsets. */
    char behind[48];
    behind[0] = '\0';
    if ((htype & PCI_HEADER_TYPE_MASK) == PCI_HEADER_TYPE_BRIDGE) {
        uint8_t first_behind = PciDoorByte(door, segment, bus, dev, fn,
                                           PCI_BRIDGE_SECONDARY_BUS);
        uint8_t last_behind  = PciDoorByte(door, segment, bus, dev, fn,
                                           PCI_BRIDGE_SUBORDINATE_BUS);
        ksnprintf(behind, sizeof(behind), " spanning buses %02x..%02x",
                  first_behind, last_behind);
        tally->bridges++;
    }

    char how[112];
    PciInterruptClause(pin, msi_off != 0, msix_off != 0, how, sizeof(how));

    kprintf("[PCI] %04x:%02x:%02x.%x  %04x:%04x  %s%s — read through %s, %s\n",
            segment, bus, dev, fn, vendor, devid, what, behind,
            PciDoorName(door), how);

    /* Every function names itself on Touch as the walk goes past it: a
     * specific `pci:vendor:VVVV:DDDD` tag and a broader `pci:class:CC:SS:PP`
     * tag, both carrying the whole coordinate tuple, so that whoever hears
     * one can talk back to that function without walking the buses again.
     *
     * ‼ NOTHING IN THIS TREE SUBSCRIBES TO EITHER TAG YET, AND THAT IS SAID
     * HERE RATHER THAN LEFT TO BE DISCOVERED. What stood in this comment
     * claimed that drivers subscribe by these tags "instead of registering
     * match tables"; there is no such subscriber anywhere — the three
     * drivers that look for a controller (ahci.c, xhci.c, ata.c) each call
     * pci_find_device_by_class, which takes a walk of its own. The channel
     * is real and the tags are well formed, and this is where a driver that
     * would rather be told about its device than go looking will hook in.
     * Until one does, these two calls publish a fact with no reader. */
    struct {
        uint16_t segment;
        uint8_t  bus, dev, fn;
        uint16_t vendor, devid;
        uint8_t  cls, sub, prog;
    } ev = { segment, bus, dev, fn, vendor, devid, cls, sub, prog };

    /* Build vendor:device tag inline (no kmalloc, no snprintf — pure
     * hex digits). Format: "pci:vendor:XXXX:XXXX\0" = 21 bytes. */
    static const char hex[] = "0123456789abcdef";
    char vt[24] = "pci:vendor:";
    vt[11] = hex[(vendor >> 12) & 0xF];
    vt[12] = hex[(vendor >>  8) & 0xF];
    vt[13] = hex[(vendor >>  4) & 0xF];
    vt[14] = hex[ vendor        & 0xF];
    vt[15] = ':';
    vt[16] = hex[(devid >> 12) & 0xF];
    vt[17] = hex[(devid >>  8) & 0xF];
    vt[18] = hex[(devid >>  4) & 0xF];
    vt[19] = hex[ devid        & 0xF];
    vt[20] = '\0';
    TouchPublish(vt, &ev, sizeof(ev));

    char ct[20] = "pci:class:";
    ct[10] = hex[(cls >> 4) & 0xF]; ct[11] = hex[cls & 0xF];
    ct[12] = ':';
    ct[13] = hex[(sub >> 4) & 0xF]; ct[14] = hex[sub & 0xF];
    ct[15] = ':';
    ct[16] = hex[(prog >> 4) & 0xF]; ct[17] = hex[prog & 0xF];
    ct[18] = '\0';
    TouchPublish(ct, &ev, sizeof(ev));

    if (msi_off || msix_off || pcie_off) {
        debug_printf("[PCI]     caps:%s%s%s\n",
                     msi_off  ? " MSI"   : "",
                     msix_off ? " MSI-X" : "",
                     pcie_off ? " PCIe"  : "");
    }
    /* log_pcie_link and the extended chain both read through the ECAM
     * window by construction; through the legacy door the first would print
     * a width and a speed decoded from 0xFFFF, and the second cannot reach
     * offset 0x100 at all. Neither is guessed at — the closing line of the
     * walk says instead that nothing above offset 0xFF was asked. */
    if (pcie_off && door == PCI_DOOR_ECAM) {
        log_pcie_link(segment, bus, dev, fn, pcie_off);

        uint16_t aer = pci_find_ext_capability(segment, bus, dev, fn,
                                                  PCI_EXT_CAP_ID_AER);
        uint16_t vc  = pci_find_ext_capability(segment, bus, dev, fn,
                                                  PCI_EXT_CAP_ID_VC);
        uint16_t sriov = pci_find_ext_capability(segment, bus, dev, fn,
                                                    PCI_EXT_CAP_ID_SR_IOV);
        if (aer || vc || sriov) {
            debug_printf("[PCI]     ext-caps:%s%s%s\n",
                         aer   ? " AER"    : "",
                         vc    ? " VC"     : "",
                         sriov ? " SR-IOV" : "");
        }
    }

    tally->functions++;
}

/* One bus range, one door, every function that answers on it.
 *
 * The range is walked whole rather than followed through bridges, and the
 * two walks are the same shape for that reason: what this log has to answer
 * is what is THERE, including a function behind a bridge whose Subordinate
 * Bus Number firmware left wrong — the one case where the inventory has to
 * be trusted over the topology. */
static void PciWalkBusRange(PciDoor door, uint16_t segment, uint8_t first_bus,
                            uint8_t last_bus, PciWalkTally* tally) {
    for (uint16_t b = first_bus; b <= last_bus; b++) {
        uint32_t before = tally->functions;

        for (uint8_t d = 0; d < 32; d++) {
            uint16_t vendor = PciDoorWord(door, segment, (uint8_t)b, d, 0,
                                          PCI_VENDOR_ID);
            if (vendor == PCI_INVALID_VENDOR) continue;

            enumerate_function(door, segment, (uint8_t)b, d, 0, tally);

            /* Multi-function device? Bit 7 of header type. enumerate_function
             * returns on its own when a function is absent, so no vendor
             * probe is repeated here. */
            uint8_t htype = PciDoorByte(door, segment, (uint8_t)b, d, 0,
                                        PCI_HEADER_TYPE);
            if (htype & PCI_HEADER_TYPE_MF) {
                for (uint8_t f = 1; f < 8; f++) {
                    enumerate_function(door, segment, (uint8_t)b, d, f, tally);
                }
            }
        }

        if (tally->functions != before) tally->buses++;
    }
}

/* The one line that closes the walk: how many, through which door, how many
 * of them bridges.
 *
 * The door has to be in it. The IOMMU prints a count of its own from an
 * independent sweep of configuration space (core/iommu), and when the two
 * counts disagree the only thing that can tell them apart is which door each
 * walk went through — a bare number would leave that unanswerable. */
static void PciSayWhatTheWalkFound(PciDoor door, const PciWalkTally* tally,
                                   uint8_t segments) {
    if (tally->functions == 0) {
        kprintf("[PCI] %[E]the walk is done and not one function answered "
                "through %s — on a machine that is running, that says the "
                "configuration door itself is not answering, not that the "
                "buses are empty%[D]\n", PciDoorName(door));
        return;
    }
    if (door == PCI_DOOR_ECAM) {
        kprintf("[PCI] the walk is done: %u function(s) answered through ECAM "
                "on %u bus(es) of %u segment group(s), %u of them PCI-to-PCI "
                "bridges — every function this kernel can see is named "
                "above\n",
                tally->functions, tally->buses, segments, tally->bridges);
    } else {
        kprintf("[PCI] the walk is done: %u function(s) answered through "
                "0xCF8/0xCFC on %u bus(es) of segment 0, %u of them "
                "PCI-to-PCI bridges — not one of them was asked anything "
                "above configuration offset 0xFF, because through this door "
                "there is nothing there to ask\n",
                tally->functions, tally->buses, tally->bridges);
    }
}

uint32_t pci_enumerate_ecam(void) {
    if (!pci_has_ecam()) {
        debug_printf("[PCI] ECAM enumeration skipped — no MCFG\n");
        return 0;
    }

    PciWalkTally tally = { 0, 0, 0 };
    for (uint8_t s = 0; s < g_ecam_count; s++) {
        pci_ecam_segment_t* seg = &g_ecam[s];
        PciWalkBusRange(PCI_DOOR_ECAM, seg->segment, seg->start_bus,
                        seg->end_bus, &tally);
    }
    PciSayWhatTheWalkFound(PCI_DOOR_ECAM, &tally, g_ecam_count);
    return tally.functions;
}

/* The same inventory on a machine that has no ECAM window to walk.
 *
 * Not a lesser walk: it reads the same registers of the same functions
 * through the door the machine actually has, and the only thing out of its
 * reach is the extended configuration space — which the closing line says
 * out loud. Without it, a board with no MCFG, and a board whose ECAM windows
 * failed to map, printed no inventory at all — and on such a board a
 * controller that fails to appear looks exactly like a controller that was
 * never on a bus. */
static uint32_t PciEnumerateThroughPorts(void) {
    PciWalkTally tally = { 0, 0, 0 };
    PciWalkBusRange(PCI_DOOR_PORTS, 0, 0, 255, &tally);
    PciSayWhatTheWalkFound(PCI_DOOR_PORTS, &tally, 1);
    return tally.functions;
}

void pci_init(void) {
    debug_printf("[PCI] Initializing PCI subsystem...\n");

    /* Set up ECAM mappings before any bus scan. After this, every
     * pci_config_* call to segment 0 will go through MMIO instead of
     * 0xCF8/0xCFC when an MCFG entry covers the bus. */
    pci_ecam_init();

    /* The inventory, through whichever door this machine turned out to have.
     * Every function names itself on its own line, and that account is what
     * this walk is for: it is the only description of these buses that
     * survives a shipped build, where debug_printf compiles away to nothing.
     *
     * It is NOT how the drivers below find their devices. Each of them asks
     * pci_find_device_by_class, which walks again on its own account. The
     * `pci:vendor:` / `pci:class:` tags this walk publishes on the way past
     * have no subscriber in this tree yet — enumerate_function publishes
     * them and says so there. The walk also leaves the configuration
     * registers of every function hot in the dcache before driver init
     * touches the same ones. */
    if (pci_has_ecam()) {
        pci_enumerate_ecam();
    } else {
        PciEnumerateThroughPorts();
    }

    pci_device_t ide_controller;
    if (pci_find_device_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, 0xFF, &ide_controller) == 0) {
        debug_printf("[PCI] Found IDE controller: %02x:%02x.%x\n",
                     ide_controller.bus, ide_controller.device, ide_controller.function);
        debug_printf("[PCI]   Vendor: 0x%04x  Device: 0x%04x\n",
                     ide_controller.vendor_id, ide_controller.device_id);
        debug_printf("[PCI]   Class: 0x%02x  Subclass: 0x%02x  ProgIF: 0x%02x\n",
                     ide_controller.class_code, ide_controller.subclass, ide_controller.prog_if);
    } else {
        debug_printf("[PCI] No IDE controller found\n");
    }
}
