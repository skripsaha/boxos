#ifndef PCI_H
#define PCI_H

#include "ktypes.h"

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_VENDOR_ID           0x00
#define PCI_DEVICE_ID           0x02
#define PCI_COMMAND             0x04
#define PCI_STATUS              0x06
#define PCI_REVISION_ID         0x08
#define PCI_PROG_IF             0x09
#define PCI_SUBCLASS            0x0A
#define PCI_CLASS_CODE          0x0B
#define PCI_CACHE_LINE_SIZE     0x0C
#define PCI_LATENCY_TIMER       0x0D
#define PCI_HEADER_TYPE         0x0E
#define PCI_BIST                0x0F
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14
#define PCI_BAR2                0x18
#define PCI_BAR3                0x1C
#define PCI_BAR4                0x20
#define PCI_BAR5                0x24

#define PCI_CMD_IO_SPACE        0x0001
#define PCI_CMD_MEM_SPACE       0x0002
#define PCI_CMD_BUS_MASTER      0x0004
#define PCI_CMD_SPECIAL_CYCLES  0x0008
#define PCI_CMD_INT_DISABLE     0x0400

// PCI-to-PCI bridge registers (header type 1)
#define PCI_BRIDGE_PRIMARY_BUS      0x18
#define PCI_BRIDGE_SECONDARY_BUS    0x19
#define PCI_BRIDGE_SUBORDINATE_BUS  0x1A

// BAR type bits (MMIO BAR bits [2:1])
#define PCI_BAR_TYPE_32BIT      0x00
#define PCI_BAR_TYPE_64BIT      0x02

#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_BRIDGE        0x06
#define PCI_SUBCLASS_IDE        0x01
#define PCI_SUBCLASS_PCI_BRIDGE 0x04
#define PCI_PROG_IF_PCI_NATIVE  0x80

#define PCI_HEADER_TYPE_NORMAL  0x00
#define PCI_HEADER_TYPE_BRIDGE  0x01
#define PCI_HEADER_TYPE_MASK    0x7F
#define PCI_HEADER_TYPE_MF      0x80

#define PCI_INVALID_VENDOR      0xFFFF

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t header_type;
} pci_device_t;

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value);

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t* out);
int pci_enable_bus_master(pci_device_t* device);

// Legacy 32-bit BAR read (for I/O space BARs and backward compatibility)
uint32_t pci_read_bar(pci_device_t* device, uint8_t bar_num);

// Full 64-bit BAR read: handles 32-bit and 64-bit MMIO BARs correctly
// Returns the full physical address. For 64-bit BARs, reads BAR[n] + BAR[n+1].
uint64_t pci_read_bar64(pci_device_t* device, uint8_t bar_num);

void pci_init(void);

#endif
