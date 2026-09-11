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

#define PCI_BRIDGE_PRIMARY_BUS      0x18
#define PCI_BRIDGE_SECONDARY_BUS    0x19
#define PCI_BRIDGE_SUBORDINATE_BUS  0x1A

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

bool     pci_has_ecam(void);
uint32_t pci_ecam_read_dword(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset);
void     pci_ecam_write_dword(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset, uint32_t value);
uint16_t pci_ecam_read_word(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset);
void     pci_ecam_write_word(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset, uint16_t value);
uint8_t  pci_ecam_read_byte(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset);
void     pci_ecam_write_byte(uint16_t segment, uint8_t bus, uint8_t device,
                              uint8_t function, uint16_t offset, uint8_t value);

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t* out);

int pci_find_nth_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                          uint32_t index, pci_device_t* out);
int pci_enable_bus_master(pci_device_t* device);

#define PCI_STATUS_CAP_LIST    (1u << 4)
#define PCI_CAP_LIST_PTR       0x34
#define PCI_CAP_ID_MSI         0x05
#define PCI_CAP_ID_PCIE        0x10
#define PCI_CAP_ID_MSIX        0x11
#define PCI_EXT_CAP_ID_AER     0x0001
#define PCI_EXT_CAP_ID_VC      0x0002
#define PCI_EXT_CAP_ID_SR_IOV  0x0010

uint8_t  pci_find_capability(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t cap_id);
uint16_t pci_find_ext_capability(uint16_t segment, uint8_t bus,
                                  uint8_t device, uint8_t function,
                                  uint16_t cap_id);

int pci_msi_enable(uint8_t bus, uint8_t device, uint8_t function,
                    uint8_t vector, uint8_t dest_lapic_id);
int pci_msix_enable_vector(uint8_t bus, uint8_t device, uint8_t function,
                            uint16_t table_index, uint8_t vector,
                            uint8_t dest_lapic_id);

uint32_t pci_read_bar(pci_device_t* device, uint8_t bar_num);

uint64_t pci_read_bar64(pci_device_t* device, uint8_t bar_num);

uint64_t pci_bar_size(pci_device_t* device, uint8_t bar_num);

void pci_init(void);

uint32_t pci_enumerate_ecam(void);

#endif