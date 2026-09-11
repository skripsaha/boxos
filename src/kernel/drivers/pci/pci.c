#include "pci.h"
#include "io.h"
#include "klib.h"
#include "acpi.h"
#include "vmm.h"
#include "touch.h"

static spinlock_t g_pci_cfg_lock = {0};

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

void pci_ecam_write_word(uint16_t segment, uint8_t bus, uint8_t device,
                         uint8_t function, uint16_t offset, uint16_t value) {
    uint32_t aligned_off = offset & 0xFFCu;
    uint32_t dword = pci_ecam_read_dword(segment, bus, device, function,
                                          aligned_off);
    uint8_t shift = (uint8_t)((offset & 0x2u) * 8);
    dword = (dword & ~((uint32_t)0xFFFFu << shift)) |
            ((uint32_t)value << shift);
    pci_ecam_write_dword(segment, bus, device, function, aligned_off, dword);
}

uint8_t pci_ecam_read_byte(uint16_t segment, uint8_t bus, uint8_t device,
                            uint8_t function, uint16_t offset) {
    uint32_t aligned = pci_ecam_read_dword(segment, bus, device, function,
                                            offset & 0xFFCu);
    return (uint8_t)(aligned >> ((offset & 0x3u) * 8));
}

void pci_ecam_write_byte(uint16_t segment, uint8_t bus, uint8_t device,
                         uint8_t function, uint16_t offset, uint8_t value) {
    uint32_t aligned_off = offset & 0xFFCu;
    uint32_t dword = pci_ecam_read_dword(segment, bus, device, function,
                                          aligned_off);
    uint8_t shift = (uint8_t)((offset & 0x3u) * 8);
    dword = (dword & ~((uint32_t)0xFFu << shift)) |
            ((uint32_t)value << shift);
    pci_ecam_write_dword(segment, bus, device, function, aligned_off, dword);
}

static void pci_ecam_init(void) {
    g_ecam_count = 0;
    const acpi_mcfg_info_t* mcfg = acpi_get_mcfg();
    if (!mcfg) {
        debug_printf("[PCI] No MCFG — using legacy 0xCF8/0xCFC config access\n");
        return;
    }
    for (uint8_t i = 0; i < mcfg->count && g_ecam_count < ACPI_MCFG_MAX_SEGMENTS; i++) {
        const acpi_mcfg_segment_t* s = &mcfg->segments[i];
        size_t span = ((size_t)(s->end_bus - s->start_bus) + 1) * (1u << 20);
        volatile void* va = vmm_map_mmio((uintptr_t)s->base_address, span,
                                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                         VMM_FLAG_CACHE_DISABLE);
        if (!va) {
            debug_printf("[PCI] ECAM map failed seg=%u base=0x%lx span=0x%lx\n",
                         s->segment_group,
                         (unsigned long)s->base_address,
                         (unsigned long)span);
            continue;
        }
        pci_ecam_segment_t* slot = &g_ecam[g_ecam_count++];
        slot->segment   = s->segment_group;
        slot->start_bus = s->start_bus;
        slot->end_bus   = s->end_bus;
        slot->phys_base = (uintptr_t)s->base_address;
        slot->virt_base = (volatile uint8_t *)va;
        debug_printf("[PCI] ECAM seg=%u bus %u..%u mapped 0x%lx -> %p (%lu MiB)\n",
                     s->segment_group, s->start_bus, s->end_bus,
                     (unsigned long)s->base_address, (void*)va,
                     (unsigned long)(span >> 20));
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

void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t aligned_offset = offset & 0xFC;
    uint32_t dword = pci_config_read_dword(bus, device, function, aligned_offset);

    uint8_t shift = (offset & 0x02) * 8;
    dword = (dword & ~(0xFFFF << shift)) | ((uint32_t)value << shift);

    pci_config_write_dword(bus, device, function, aligned_offset, dword);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    return (uint8_t)(dword >> ((offset & 0x03) * 8));
}

void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
    uint32_t aligned_offset = offset & 0xFC;
    uint32_t dword = pci_config_read_dword(bus, device, function, aligned_offset);

    uint8_t shift = (offset & 0x03) * 8;
    dword = (dword & ~(0xFF << shift)) | ((uint32_t)value << shift);

    pci_config_write_dword(bus, device, function, aligned_offset, dword);
}

static void pci_visited_clear(uint64_t visited[4]) {
    visited[0] = visited[1] = visited[2] = visited[3] = 0;
}

static bool pci_visited_test_set(uint64_t visited[4], uint8_t bus) {
    uint64_t mask = 1ULL << (bus & 63);
    uint64_t *slot = &visited[bus >> 6];
    if (*slot & mask) return true;
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

                if (skip && *skip > 0) {
                    (*skip)--;
                } else

                return 0;
            }

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
        return bar_value & 0xFFFFFFFC;
    }

    return bar_value & 0xFFFFFFF0;
}

uint64_t pci_read_bar64(pci_device_t* device, uint8_t bar_num) {
    if (!device || bar_num > 5) {
        return 0;
    }

    uint8_t bar_offset = PCI_BAR0 + (bar_num * 4);
    uint32_t bar_low = pci_config_read_dword(device->bus, device->device,
                                              device->function, bar_offset);

    if (bar_low & 0x01) {
        return (uint64_t)(bar_low & 0xFFFFFFFC);
    }

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

    return (uint64_t)(bar_low & 0xFFFFFFF0);
}

uint64_t pci_bar_size(pci_device_t* device, uint8_t bar_num) {
    if (!device || bar_num > 5) {
        return 0;
    }

    uint8_t off = (uint8_t)(PCI_BAR0 + (bar_num * 4));
    uint32_t orig_lo = pci_config_read_dword(device->bus, device->device,
                                             device->function, off);

    if (orig_lo == 0xFFFFFFFFu || orig_lo == 0) {
        return 0;
    }
    if (orig_lo & 0x01u) {
        return 0;
    }

    bool is64 = (((orig_lo >> 1) & 0x03u) == PCI_BAR_TYPE_64BIT);
    if (is64 && bar_num >= 5) {
        return 0;
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

    uint64_t mask = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & 0xFFFFFFF0u);
    if (!is64) {
        mask |= 0xFFFFFFFF00000000ULL;
    }
    if (mask == 0 || mask == ~0ULL) {
        return 0;
    }

    return ~mask + 1ULL;
}


uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t cap_id) {
    uint16_t status = pci_config_read_word(bus, device, function, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;

    uint8_t off = pci_config_read_byte(bus, device, function, PCI_CAP_LIST_PTR);
    off &= 0xFCu;
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

uint16_t pci_find_ext_capability(uint16_t segment, uint8_t bus, uint8_t device,
                                  uint8_t function, uint16_t cap_id) {
    if (!pci_has_ecam()) return 0;
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


static inline uint32_t msi_addr_lo(uint8_t dest_lapic_id) {
    return 0xFEE00000u | ((uint32_t)dest_lapic_id << 12);
}
static inline uint32_t msi_data(uint8_t vector) {
    return (uint32_t)vector;
}

int pci_msi_enable(uint8_t bus, uint8_t device, uint8_t function,
                    uint8_t vector, uint8_t dest_lapic_id) {
    uint8_t cap = pci_find_capability(bus, device, function, PCI_CAP_ID_MSI);
    if (cap == 0) return -1;

    uint16_t cmd = pci_config_read_word(bus, device, function, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write_word(bus, device, function, PCI_COMMAND, cmd);

    uint16_t mc = pci_config_read_word(bus, device, function, (uint8_t)(cap + 2));
    bool is64 = (mc & (1u << 7)) != 0;

    pci_config_write_dword(bus, device, function, (uint8_t)(cap + 4),
                            msi_addr_lo(dest_lapic_id));
    if (is64) {
        pci_config_write_dword(bus, device, function, (uint8_t)(cap + 8), 0);
        pci_config_write_word(bus, device, function, (uint8_t)(cap + 12),
                                (uint16_t)msi_data(vector));
    } else {
        pci_config_write_word(bus, device, function, (uint8_t)(cap + 8),
                                (uint16_t)msi_data(vector));
    }

    mc &= ~(uint16_t)0x70u;
    mc |= 0x0001u;
    pci_config_write_word(bus, device, function, (uint8_t)(cap + 2), mc);
    return 0;
}

int pci_msix_enable_vector(uint8_t bus, uint8_t device, uint8_t function,
                            uint16_t table_index, uint8_t vector,
                            uint8_t dest_lapic_id) {
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
    entry[0] = msi_addr_lo(dest_lapic_id);
    entry[1] = 0;
    entry[2] = msi_data(vector);
    entry[3] = 0;

    uint16_t cmd = pci_config_read_word(bus, device, function, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write_word(bus, device, function, PCI_COMMAND, cmd);

    mc |= 0x8000u;
    mc &= ~(uint16_t)0x4000u;
    pci_config_write_word(bus, device, function, (uint8_t)(cap + 2), mc);
    return 0;
}

static void log_pcie_link(uint16_t segment, uint8_t bus, uint8_t dev,
                          uint8_t fn, uint8_t cap_off) {
    uint16_t link_status = pci_config_read_word(bus, dev, fn,
                                                  (uint8_t)(cap_off + 0x12));
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
    (void)segment;
    debug_printf("[PCI]     PCIe link: x%u @ %s\n", width, speed);
}

static void enumerate_function(uint16_t segment, uint8_t bus, uint8_t dev,
                                uint8_t fn, uint32_t* counter) {
    uint16_t vendor = pci_config_read_word(bus, dev, fn, PCI_VENDOR_ID);
    if (vendor == PCI_INVALID_VENDOR) return;

    uint16_t devid = pci_config_read_word(bus, dev, fn, PCI_DEVICE_ID);
    uint8_t  cls   = pci_config_read_byte(bus, dev, fn, PCI_CLASS_CODE);
    uint8_t  sub   = pci_config_read_byte(bus, dev, fn, PCI_SUBCLASS);
    uint8_t  prog  = pci_config_read_byte(bus, dev, fn, PCI_PROG_IF);

    debug_printf("[PCI]   %04x:%02x:%02x.%x  %04x:%04x  class=%02x.%02x.%02x\n",
                 segment, bus, dev, fn, vendor, devid, cls, sub, prog);

    struct {
        uint16_t segment;
        uint8_t  bus, dev, fn;
        uint16_t vendor, devid;
        uint8_t  cls, sub, prog;
    } ev = { segment, bus, dev, fn, vendor, devid, cls, sub, prog };

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

    uint8_t msi_off  = pci_find_capability(bus, dev, fn, PCI_CAP_ID_MSI);
    uint8_t msix_off = pci_find_capability(bus, dev, fn, PCI_CAP_ID_MSIX);
    uint8_t pcie_off = pci_find_capability(bus, dev, fn, PCI_CAP_ID_PCIE);
    if (msi_off || msix_off || pcie_off) {
        debug_printf("[PCI]     caps:%s%s%s\n",
                     msi_off  ? " MSI"   : "",
                     msix_off ? " MSI-X" : "",
                     pcie_off ? " PCIe"  : "");
    }
    if (pcie_off) {
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

    (*counter)++;
}

uint32_t pci_enumerate_ecam(void) {
    if (!pci_has_ecam()) {
        debug_printf("[PCI] ECAM enumeration skipped — no MCFG\n");
        return 0;
    }

    uint32_t total = 0;
    for (uint8_t s = 0; s < g_ecam_count; s++) {
        pci_ecam_segment_t* seg = &g_ecam[s];
        debug_printf("[PCI] enumerating segment %u (buses %u..%u)\n",
                     seg->segment, seg->start_bus, seg->end_bus);

        for (uint16_t b = seg->start_bus; b <= seg->end_bus; b++) {
            for (uint8_t d = 0; d < 32; d++) {
                uint16_t vendor = pci_ecam_read_word(seg->segment,
                                                       (uint8_t)b, d, 0,
                                                       PCI_VENDOR_ID);
                if (vendor == PCI_INVALID_VENDOR) continue;

                enumerate_function(seg->segment, (uint8_t)b, d, 0, &total);

                uint8_t htype = pci_ecam_read_byte(seg->segment,
                                                     (uint8_t)b, d, 0,
                                                     PCI_HEADER_TYPE);
                if (htype & PCI_HEADER_TYPE_MF) {
                    for (uint8_t f = 1; f < 8; f++) {
                        uint16_t v = pci_ecam_read_word(seg->segment,
                                                          (uint8_t)b, d, f,
                                                          PCI_VENDOR_ID);
                        if (v != PCI_INVALID_VENDOR) {
                            enumerate_function(seg->segment, (uint8_t)b, d, f,
                                                &total);
                        }
                    }
                }
            }
        }
    }
    debug_printf("[PCI] ECAM enumeration: %u function(s) seen\n", total);
    return total;
}

void pci_init(void) {
    debug_printf("[PCI] Initializing PCI subsystem...\n");

    pci_ecam_init();
    pci_enumerate_ecam();

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