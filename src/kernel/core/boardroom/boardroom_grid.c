#include "boardroom.h"
#include "ahci.h"
#include "ata.h"
#include "xhci_msd.h"

/*
 * The grid a write to a seat's medium is cut to (boardroom.h,
 * BoardroomSeatGrid): its physical block, as the Boardroom says it for any
 * reader, and L0 — where the medium's blocks begin — as the driver that read
 * it holds it, already turned into the room's 512-byte sectors: IDENTIFY word
 * 209 by ata_identify.h for an ATA disk and an AHCI port, READ CAPACITY(16)'s
 * LOWEST ALIGNED LOGICAL BLOCK ADDRESS by xhci_msd.c for a USB unit. Its own
 * file beside the Boardroom's: a question of a format-v2 volume's ground,
 * asked of the seats through their public face (seat kind and index).
 */
void BoardroomSeatGrid(uint8_t seat, uint32_t* physical, uint64_t* first_aligned)
{
    uint8_t index = BoardroomSeatIndex(seat);

    *physical      = BoardroomSeatPhysicalBytes(seat);
    *first_aligned = 0;
    switch (BoardroomSeatKind(seat)) {
    case BOARD_USB:
        *first_aligned = xhci_msd_unit_first_aligned(index);
        break;
    case BOARD_AHCI: {
        ahci_port_t* p = ahci_get_port_state(index);
        *first_aligned = p ? p->first_aligned : 0;
        break;
    }
    case BOARD_ATA:
        if (index < 4)
            *first_aligned = g_ata_devices[index].first_aligned;
        break;
    default:
        break;
    }
}
