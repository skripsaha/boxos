#include "pci.h"
#include "io.h"
#include "klib.h"

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
    uint32_t address = pci_build_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = pci_build_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
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

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t* out) {
    if (!out) {
        return -1;
    }

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint16_t vendor_id = pci_config_read_word(bus, device, function, PCI_VENDOR_ID);

                if (vendor_id == PCI_INVALID_VENDOR) {
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

                    return 0;
                }

                if ((header_type & 0x80) == 0 && function == 0) {
                    break;
                }
            }
        }
    }

    return -1;
}

int pci_enable_bus_master(pci_device_t* device) {
    if (!device) {
        return -1;
    }

    uint16_t command = pci_config_read_word(device->bus, device->device, device->function, PCI_COMMAND);
    command |= PCI_CMD_BUS_MASTER | PCI_CMD_IO_SPACE;
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

void pci_init(void) {
    debug_printf("[PCI] Initializing PCI subsystem...\n");

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
