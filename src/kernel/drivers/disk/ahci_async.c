#include "ahci_async.h"
#include "ahci.h"
#include "klib.h"

/*
 * AHCI async submission. The berth is the request: admitted on the spot when
 * the port has a slot and nobody waiting, moored in the port's roadstead
 * otherwise and issued when it is called in. Either way the answer comes
 * through berth->answered — from the completion interrupt, from the port's
 * recovery, or from the port leaving the machine.
 */
static error_t ahci_submit_async(uint8_t port_num, RoadsteadBerth* berth)
{
    if (!berth->answered || !berth->dma_phys || berth->sectors == 0 ||
        berth->sectors > 0xFFFFu) {
        return ERR_INVALID_ARGUMENT;
    }

    ahci_port_t* port = ahci_get_port_state(port_num);
    if (!port) {
        return ERR_DEVICE_NOT_READY;
    }

    berth->want = BERTH_ONE;
    uint8_t slot = 0;
    switch (AhciAdmit(port, berth, &slot)) {
    case ROADSTEAD_TAKEN:
        return AhciBoard(port, berth, slot);
    case ROADSTEAD_WAIT:
        return OK;
    case ROADSTEAD_GONE:
    default:
        return ERR_DEVICE_NOT_READY;
    }
}

error_t ahci_submit_read_async(uint8_t port, RoadsteadBerth* berth)
{
    if (!berth) {
        return ERR_INVALID_ARGUMENT;
    }
    berth->write = false;
    return ahci_submit_async(port, berth);
}

error_t ahci_submit_write_async(uint8_t port, RoadsteadBerth* berth)
{
    if (!berth) {
        return ERR_INVALID_ARGUMENT;
    }
    berth->write = true;
    return ahci_submit_async(port, berth);
}
