#include "pci_class_words.h"
#include "klib.h"

/* ============================================================
 * The class-code vocabulary.
 *
 * Every row here is one line of the PCI Code and ID Assignment
 * Specification Rev. 1.14, §1 "Class Codes" — the document PCI Local Bus
 * Specification Rev. 3.0 §6.2.1 delegates the naming to when it defines the
 * Class Code register at configuration offsets 09h/0Ah/0Bh. Nothing in this
 * file is a guess about a machine: it is the assignment list, transcribed,
 * and a code that is not on the list gets no word.
 *
 * Three fields, searched from the most specific answer to the least:
 *   cls  — Base Class, offset 0Bh
 *   sub  — Sub-Class, offset 0Ah, or PCI_CLASS_ANY for "the whole base class"
 *   prog — Programming Interface, offset 09h, or PCI_CLASS_ANY for
 *          "whatever interface this sub-class is speaking"
 *
 * A Programming Interface only earns a row of its own where it changes which
 * driver can own the device — AHCI against a vendor-specific SATA interface,
 * xHCI against EHCI, NVM Express against NVMHCI. Where the interface byte
 * names no separate thing, the sub-class row answers for all of them.
 * ============================================================ */

#define PCI_CLASS_ANY 0x100u

typedef struct {
    uint8_t     cls;
    uint16_t    sub;
    uint16_t    prog;
    const char *words;
} PciClassRow;

static const PciClassRow kPciClassRows[] = {
    /* 00h — built before the class code register meant anything. */
    { 0x00, 0x01,          PCI_CLASS_ANY, "a VGA-compatible device from before class codes existed" },
    { 0x00, PCI_CLASS_ANY, PCI_CLASS_ANY, "a device from before class codes existed" },

    /* 01h — Mass Storage Controller. */
    { 0x01, 0x00,          PCI_CLASS_ANY, "a SCSI controller" },
    { 0x01, 0x01,          PCI_CLASS_ANY, "an IDE controller" },
    { 0x01, 0x02,          PCI_CLASS_ANY, "a floppy disk controller" },
    { 0x01, 0x03,          PCI_CLASS_ANY, "an IPI bus controller" },
    { 0x01, 0x04,          PCI_CLASS_ANY, "a RAID controller" },
    { 0x01, 0x05,          PCI_CLASS_ANY, "an ATA controller" },
    { 0x01, 0x06,          0x00,          "a SATA controller with a vendor-specific interface" },
    { 0x01, 0x06,          0x01,          "a SATA controller speaking the AHCI programming interface" },
    { 0x01, 0x06,          0x02,          "a SATA controller on the Serial Storage Bus" },
    { 0x01, 0x06,          PCI_CLASS_ANY, "a SATA controller" },
    { 0x01, 0x07,          PCI_CLASS_ANY, "a Serial Attached SCSI controller" },
    { 0x01, 0x08,          0x01,          "a non-volatile memory controller speaking NVMHCI" },
    { 0x01, 0x08,          0x02,          "a non-volatile memory controller speaking NVM Express" },
    { 0x01, 0x08,          PCI_CLASS_ANY, "a non-volatile memory controller" },
    { 0x01, 0x09,          PCI_CLASS_ANY, "a Universal Flash Storage controller" },
    { 0x01, 0x80,          PCI_CLASS_ANY, "a mass storage controller of no defined kind" },
    { 0x01, PCI_CLASS_ANY, PCI_CLASS_ANY, "a mass storage controller" },

    /* 02h — Network Controller. */
    { 0x02, 0x00,          PCI_CLASS_ANY, "an Ethernet controller" },
    { 0x02, 0x01,          PCI_CLASS_ANY, "a Token Ring controller" },
    { 0x02, 0x02,          PCI_CLASS_ANY, "an FDDI controller" },
    { 0x02, 0x03,          PCI_CLASS_ANY, "an ATM controller" },
    { 0x02, 0x04,          PCI_CLASS_ANY, "an ISDN controller" },
    { 0x02, 0x05,          PCI_CLASS_ANY, "a WorldFip controller" },
    { 0x02, 0x06,          PCI_CLASS_ANY, "a PICMG 2.14 multi-computing controller" },
    { 0x02, 0x07,          PCI_CLASS_ANY, "an InfiniBand network controller" },
    { 0x02, 0x08,          PCI_CLASS_ANY, "a host fabric controller" },
    { 0x02, 0x80,          PCI_CLASS_ANY, "a network controller of no defined kind" },
    { 0x02, PCI_CLASS_ANY, PCI_CLASS_ANY, "a network controller" },

    /* 03h — Display Controller. */
    { 0x03, 0x00,          0x00,          "a VGA-compatible display controller" },
    { 0x03, 0x00,          0x01,          "an 8514-compatible display controller" },
    { 0x03, 0x00,          PCI_CLASS_ANY, "a VGA display controller" },
    { 0x03, 0x01,          PCI_CLASS_ANY, "an XGA display controller" },
    { 0x03, 0x02,          PCI_CLASS_ANY, "a 3D controller that is not VGA-compatible" },
    { 0x03, 0x80,          PCI_CLASS_ANY, "a display controller of no defined kind" },
    { 0x03, PCI_CLASS_ANY, PCI_CLASS_ANY, "a display controller" },

    /* 04h — Multimedia Device. */
    { 0x04, 0x00,          PCI_CLASS_ANY, "a video device" },
    { 0x04, 0x01,          PCI_CLASS_ANY, "an audio device" },
    { 0x04, 0x02,          PCI_CLASS_ANY, "a computer telephony device" },
    { 0x04, 0x03,          PCI_CLASS_ANY, "a High Definition Audio controller" },
    { 0x04, 0x80,          PCI_CLASS_ANY, "a multimedia device of no defined kind" },
    { 0x04, PCI_CLASS_ANY, PCI_CLASS_ANY, "a multimedia device" },

    /* 05h — Memory Controller. */
    { 0x05, 0x00,          PCI_CLASS_ANY, "a RAM controller" },
    { 0x05, 0x01,          PCI_CLASS_ANY, "a flash memory controller" },
    { 0x05, 0x02,          PCI_CLASS_ANY, "a CXL memory controller" },
    { 0x05, 0x80,          PCI_CLASS_ANY, "a memory controller of no defined kind" },
    { 0x05, PCI_CLASS_ANY, PCI_CLASS_ANY, "a memory controller" },

    /* 06h — Bridge Device. */
    { 0x06, 0x00,          PCI_CLASS_ANY, "a host bridge" },
    { 0x06, 0x01,          PCI_CLASS_ANY, "an ISA bridge" },
    { 0x06, 0x02,          PCI_CLASS_ANY, "an EISA bridge" },
    { 0x06, 0x03,          PCI_CLASS_ANY, "a MicroChannel bridge" },
    { 0x06, 0x04,          0x01,          "a PCI-to-PCI bridge that decodes subtractively" },
    { 0x06, 0x04,          PCI_CLASS_ANY, "a PCI-to-PCI bridge" },
    { 0x06, 0x05,          PCI_CLASS_ANY, "a PCMCIA bridge" },
    { 0x06, 0x06,          PCI_CLASS_ANY, "a NuBus bridge" },
    { 0x06, 0x07,          PCI_CLASS_ANY, "a CardBus bridge" },
    { 0x06, 0x08,          PCI_CLASS_ANY, "a RACEway bridge" },
    { 0x06, 0x09,          PCI_CLASS_ANY, "a semi-transparent PCI-to-PCI bridge" },
    { 0x06, 0x0A,          PCI_CLASS_ANY, "an InfiniBand-to-PCI host bridge" },
    { 0x06, 0x80,          PCI_CLASS_ANY, "a bridge of no defined kind" },
    { 0x06, PCI_CLASS_ANY, PCI_CLASS_ANY, "a bridge" },

    /* 07h — Simple Communication Controller. */
    { 0x07, 0x00,          0x02,          "a 16550-compatible serial controller" },
    { 0x07, 0x00,          PCI_CLASS_ANY, "a serial controller" },
    { 0x07, 0x01,          PCI_CLASS_ANY, "a parallel port" },
    { 0x07, 0x02,          PCI_CLASS_ANY, "a multiport serial controller" },
    { 0x07, 0x03,          PCI_CLASS_ANY, "a modem" },
    { 0x07, 0x04,          PCI_CLASS_ANY, "a GPIB controller" },
    { 0x07, 0x05,          PCI_CLASS_ANY, "a smart card reader" },
    { 0x07, 0x80,          PCI_CLASS_ANY, "a communication controller of no defined kind" },
    { 0x07, PCI_CLASS_ANY, PCI_CLASS_ANY, "a simple communication controller" },

    /* 08h — Base System Peripheral. */
    { 0x08, 0x00,          0x10,          "an I/O APIC interrupt controller" },
    { 0x08, 0x00,          0x20,          "an I/O(x) APIC interrupt controller" },
    { 0x08, 0x00,          PCI_CLASS_ANY, "an 8259-style interrupt controller" },
    { 0x08, 0x01,          PCI_CLASS_ANY, "a DMA controller" },
    { 0x08, 0x02,          PCI_CLASS_ANY, "a system timer" },
    { 0x08, 0x03,          PCI_CLASS_ANY, "a real-time clock controller" },
    { 0x08, 0x04,          PCI_CLASS_ANY, "a PCI hot-plug controller" },
    { 0x08, 0x05,          PCI_CLASS_ANY, "an SD host controller" },
    { 0x08, 0x06,          PCI_CLASS_ANY, "an IOMMU" },
    { 0x08, 0x80,          PCI_CLASS_ANY, "a base system peripheral of no defined kind" },
    { 0x08, PCI_CLASS_ANY, PCI_CLASS_ANY, "a base system peripheral" },

    /* 09h — Input Device. */
    { 0x09, 0x00,          PCI_CLASS_ANY, "a keyboard controller" },
    { 0x09, 0x01,          PCI_CLASS_ANY, "a digitizer pen" },
    { 0x09, 0x02,          PCI_CLASS_ANY, "a mouse controller" },
    { 0x09, 0x03,          PCI_CLASS_ANY, "a scanner controller" },
    { 0x09, 0x04,          PCI_CLASS_ANY, "a gameport controller" },
    { 0x09, 0x80,          PCI_CLASS_ANY, "an input device of no defined kind" },
    { 0x09, PCI_CLASS_ANY, PCI_CLASS_ANY, "an input device" },

    /* 0Ah — Docking Station. */
    { 0x0A, PCI_CLASS_ANY, PCI_CLASS_ANY, "a docking station" },

    /* 0Bh — Processor. */
    { 0x0B, 0x40,          PCI_CLASS_ANY, "a co-processor" },
    { 0x0B, PCI_CLASS_ANY, PCI_CLASS_ANY, "a processor" },

    /* 0Ch — Serial Bus Controller. */
    { 0x0C, 0x00,          PCI_CLASS_ANY, "an IEEE 1394 FireWire controller" },
    { 0x0C, 0x01,          PCI_CLASS_ANY, "an ACCESS.bus controller" },
    { 0x0C, 0x02,          PCI_CLASS_ANY, "an SSA controller" },
    { 0x0C, 0x03,          0x00,          "a USB controller speaking UHCI" },
    { 0x0C, 0x03,          0x10,          "a USB controller speaking OHCI" },
    { 0x0C, 0x03,          0x20,          "a USB controller speaking EHCI" },
    { 0x0C, 0x03,          0x30,          "a USB controller speaking xHCI" },
    { 0x0C, 0x03,          0x40,          "a USB4 host interface" },
    { 0x0C, 0x03,          0xFE,          "a USB device, not a host controller" },
    { 0x0C, 0x03,          PCI_CLASS_ANY, "a USB controller" },
    { 0x0C, 0x04,          PCI_CLASS_ANY, "a Fibre Channel controller" },
    { 0x0C, 0x05,          PCI_CLASS_ANY, "an SMBus controller" },
    { 0x0C, 0x06,          PCI_CLASS_ANY, "an InfiniBand serial bus controller" },
    { 0x0C, 0x07,          PCI_CLASS_ANY, "an IPMI interface" },
    { 0x0C, 0x08,          PCI_CLASS_ANY, "a SERCOS interface" },
    { 0x0C, 0x09,          PCI_CLASS_ANY, "a CANbus controller" },
    { 0x0C, 0x0A,          PCI_CLASS_ANY, "a MIPI I3C host controller" },
    { 0x0C, 0x80,          PCI_CLASS_ANY, "a serial bus controller of no defined kind" },
    { 0x0C, PCI_CLASS_ANY, PCI_CLASS_ANY, "a serial bus controller" },

    /* 0Dh — Wireless Controller. */
    { 0x0D, 0x00,          PCI_CLASS_ANY, "an iRDA controller" },
    { 0x0D, 0x01,          PCI_CLASS_ANY, "a consumer infrared controller" },
    { 0x0D, 0x10,          PCI_CLASS_ANY, "an RF controller" },
    { 0x0D, 0x11,          PCI_CLASS_ANY, "a Bluetooth controller" },
    { 0x0D, 0x12,          PCI_CLASS_ANY, "a broadband controller" },
    { 0x0D, 0x20,          PCI_CLASS_ANY, "an 802.11a controller" },
    { 0x0D, 0x21,          PCI_CLASS_ANY, "an 802.11b controller" },
    { 0x0D, 0x40,          PCI_CLASS_ANY, "a cellular controller" },
    { 0x0D, 0x41,          PCI_CLASS_ANY, "a cellular controller with Ethernet" },
    { 0x0D, 0x80,          PCI_CLASS_ANY, "a wireless controller of no defined kind" },
    { 0x0D, PCI_CLASS_ANY, PCI_CLASS_ANY, "a wireless controller" },

    /* 0Eh — Intelligent I/O Controller. */
    { 0x0E, 0x00,          PCI_CLASS_ANY, "an I2O intelligent I/O controller" },
    { 0x0E, PCI_CLASS_ANY, PCI_CLASS_ANY, "an intelligent I/O controller" },

    /* 0Fh — Satellite Communication Controller. */
    { 0x0F, 0x01,          PCI_CLASS_ANY, "a satellite TV receiver" },
    { 0x0F, 0x02,          PCI_CLASS_ANY, "a satellite audio receiver" },
    { 0x0F, 0x03,          PCI_CLASS_ANY, "a satellite voice controller" },
    { 0x0F, 0x04,          PCI_CLASS_ANY, "a satellite data controller" },
    { 0x0F, PCI_CLASS_ANY, PCI_CLASS_ANY, "a satellite communication controller" },

    /* 10h — Encryption/Decryption Controller. */
    { 0x10, 0x00,          PCI_CLASS_ANY, "a network and computing encryption controller" },
    { 0x10, 0x10,          PCI_CLASS_ANY, "an entertainment encryption controller" },
    { 0x10, 0x80,          PCI_CLASS_ANY, "an encryption controller of no defined kind" },
    { 0x10, PCI_CLASS_ANY, PCI_CLASS_ANY, "an encryption controller" },

    /* 11h — Data Acquisition and Signal Processing Controller. */
    { 0x11, 0x00,          PCI_CLASS_ANY, "a DPIO signal processing module" },
    { 0x11, 0x01,          PCI_CLASS_ANY, "a performance counter" },
    { 0x11, 0x10,          PCI_CLASS_ANY, "a communications synchronization controller" },
    { 0x11, 0x20,          PCI_CLASS_ANY, "a management card" },
    { 0x11, 0x80,          PCI_CLASS_ANY, "a signal processing controller of no defined kind" },
    { 0x11, PCI_CLASS_ANY, PCI_CLASS_ANY, "a data acquisition and signal processing controller" },

    /* 12h — Processing Accelerator. */
    { 0x12, 0x01,          PCI_CLASS_ANY, "an SNIA SDXI data mover" },
    { 0x12, PCI_CLASS_ANY, PCI_CLASS_ANY, "a processing accelerator" },

    /* 13h — Non-Essential Instrumentation. */
    { 0x13, PCI_CLASS_ANY, PCI_CLASS_ANY, "a non-essential instrumentation function" },

    /* FFh — the assignment list's own way of saying it has no word either. */
    { 0xFF, PCI_CLASS_ANY, PCI_CLASS_ANY, "a device that fits no defined class" },
};

void PciClassInWords(uint8_t cls, uint8_t sub, uint8_t prog,
                     char *out, size_t out_len) {
    if (!out || out_len == 0) return;

    /* Three passes, narrowest answer first: the whole triple, then the
     * (base, sub) pair whatever the interface byte says, then the base class
     * alone. PCI_CLASS_ANY is 0x100 and the bytes are eight bits wide, so a
     * wildcard row can never be mistaken for an exact match in an earlier
     * pass — the comparison itself keeps the passes apart. */
    for (unsigned pass = 0; pass < 3; pass++) {
        for (unsigned i = 0; i < sizeof(kPciClassRows) / sizeof(kPciClassRows[0]); i++) {
            const PciClassRow *row = &kPciClassRows[i];
            if (row->cls != cls) continue;
            if (pass == 0) {
                if (row->sub != sub || row->prog != prog) continue;
            } else if (pass == 1) {
                if (row->sub != sub || row->prog != PCI_CLASS_ANY) continue;
            } else {
                if (row->sub != PCI_CLASS_ANY) continue;
            }
            ksnprintf(out, out_len, "%s", row->words);
            return;
        }
    }

    ksnprintf(out, out_len,
              "a function of class %02xh.%02xh.%02xh, which the assignment "
              "list does not name", cls, sub, prog);
}
