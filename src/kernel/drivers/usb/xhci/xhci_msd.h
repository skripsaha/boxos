#ifndef XHCI_MSD_H
#define XHCI_MSD_H

#include "ktypes.h"
#include "error.h"
#include "xhci.h"


#define XHCI_MSD_SECTOR_BYTES 512

int  xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

bool xhci_msd_slot_attached(xhci_device_slot_t* slot);

void xhci_msd_release(xhci_device_slot_t* slot);

uint8_t     xhci_msd_unit_count(void);
bool        xhci_msd_unit_present(uint8_t unit);
uint32_t    xhci_msd_unit_max_run(uint8_t unit);

uint64_t    xhci_msd_unit_sectors(uint8_t unit);
uint32_t    xhci_msd_unit_physical_bytes(uint8_t unit);
const char* xhci_msd_unit_name(uint8_t unit);

int xhci_msd_read (uint8_t unit, uint64_t lba, uint32_t count, void* buffer);
int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer);

typedef void (*XhciMsdAsyncCb)(uint8_t index, uint8_t slot,
                               error_t status, void* ctx);

error_t xhci_msd_read_async(uint8_t unit, uint64_t lba, uint32_t count,
                            void* dma_phys, XhciMsdAsyncCb cb, void* ctx);

bool xhci_msd_unit_can_read_async(uint8_t unit);

void xhci_msd_watchdog(void);

int xhci_msd_flush(uint8_t unit);

#endif