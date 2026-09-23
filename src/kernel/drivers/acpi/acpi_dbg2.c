/* acpi_dbg2.c — the two tables in which a machine names its own debug port.
 *
 * Sources, quoted where they decide something:
 *
 *   Microsoft, "Debug Port Table 2 (DBG2)", revision 0, document revision
 *   2023-03-22. Table 1 (table format), Table 2 (Debug Device Information
 *   structure), Table 3 (port types and subtypes), the note on the fields
 *   of the Generic Address Structure, and the note on 16550-based UARTs.
 *
 *   Microsoft, "Debug Port Specification" (DBGP): one interface type byte
 *   and one Generic Address Structure, the layout ACPICA carries as
 *   ACPI_TABLE_DBGP in actbl2.h.
 *
 *   Microsoft, "Serial Port Console Redirection Table" (SPCR), version
 *   1.10, 2023-05-01, table revision 4. The field table quoted below for
 *   Interface Type (offset 36), Base Address (40), Interrupt Type (52),
 *   IRQ (53) and Global System Interrupt (54).
 *
 *   ACPI 6.5 §5.2.3.2 Table 5.1 — Generic Address Structure: Address Space
 *   ID 0 = System Memory, 1 = System I/O; Access Size 0 = undefined,
 *   1 = byte, 2 = word, 3 = dword, 4 = qword.
 *
 *   National Semiconductor PC16550D §8 — the register file this kernel
 *   drives is eight byte-wide registers. That is why a stride narrower
 *   than a byte is refused and why an address window smaller than eight
 *   strides is refused: neither can hold the part.
 */

#include "acpi_internal.h"
#include "acpi_dbg2.h"
#include "klib.h"

/* DBG2 Table 1 — the two fields past the standard SDT header. */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t          info_offset;   /* OffsetDbgDeviceInfo, byte 36 */
    uint32_t          info_count;    /* NumberDbgDeviceInfo, byte 40 */
} __attribute__((packed)) AcpiDbg2Header;
_Static_assert(sizeof(AcpiDbg2Header) == 44, "DBG2 header is 44 bytes (DBG2 Table 1)");
_Static_assert(__builtin_offsetof(AcpiDbg2Header, info_offset) == 36, "OffsetDbgDeviceInfo at 36");
_Static_assert(__builtin_offsetof(AcpiDbg2Header, info_count) == 40, "NumberDbgDeviceInfo at 40");

/* DBG2 Table 2 — Debug Device Information structure. */
typedef struct {
    uint8_t  revision;            /*  0 */
    uint16_t length;              /*  1 — whole structure, strings included */
    uint8_t  gas_count;           /*  3 — NumberofGenericAddressRegisters   */
    uint16_t namespace_length;    /*  4 */
    uint16_t namespace_offset;    /*  6 */
    uint16_t oem_data_length;     /*  8 */
    uint16_t oem_data_offset;     /* 10 */
    uint16_t port_type;           /* 12 */
    uint16_t port_subtype;        /* 14 */
    uint16_t reserved;            /* 16 */
    uint16_t base_offset;         /* 18 — BaseAddressRegisterOffset */
    uint16_t size_offset;         /* 20 — AddressSizeOffset        */
} __attribute__((packed)) AcpiDbg2Device;
_Static_assert(sizeof(AcpiDbg2Device) == 22, "Debug Device Information is 22 bytes (DBG2 Table 2)");
_Static_assert(__builtin_offsetof(AcpiDbg2Device, port_type) == 12, "Port Type at 12");
_Static_assert(__builtin_offsetof(AcpiDbg2Device, base_offset) == 18, "BaseAddressRegisterOffset at 18");

/* DBGP — ACPICA ACPI_TABLE_DBGP. */
typedef struct {
    acpi_sdt_header_t header;
    uint8_t           interface_type;   /* 36 */
    uint8_t           reserved[3];      /* 37 */
    acpi_gas_t        base_address;     /* 40 */
} __attribute__((packed)) AcpiDbgpTable;
_Static_assert(sizeof(AcpiDbgpTable) == 52, "DBGP is 52 bytes");

/* SPCR, as far as this kernel reads it.
 *
 * Everything past the Global System Interrupt is the baud rate the firmware
 * used, the PCI location of a headless port, the UART clock and (revision 4)
 * a namespace string. None of it decides where the port is or how it
 * interrupts, so a table that stops before those fields is not a table this
 * kernel is unable to read, and the length check below asks for exactly the
 * bytes taken. */
typedef struct {
    acpi_sdt_header_t header;           /*  0 */
    uint8_t           interface_type;   /* 36 */
    uint8_t           reserved[3];      /* 37 — "Must be 0" */
    acpi_gas_t        base_address;     /* 40 */
    uint8_t           interrupt_type;   /* 52 */
    uint8_t           pc_at_irq;        /* 53 */
    uint32_t          gsi;              /* 54 */
} __attribute__((packed)) AcpiSpcrTable;
_Static_assert(sizeof(AcpiSpcrTable) == 58,
               "SPCR through its Global System Interrupt is 58 bytes");
_Static_assert(__builtin_offsetof(AcpiSpcrTable, interrupt_type) == 52, "Interrupt Type at 52");
_Static_assert(__builtin_offsetof(AcpiSpcrTable, gsi) == 54, "Global System Interrupt at 54");

/* SPCR Interrupt Type, offset 52: "Interrupt type(s) used by the UART …
 * Where 0 = Not supported, 1 = Supported. Set this field to zero if the
 * described interface only supports polled operation." */
#define SPCR_LINE_PC_AT_IRQ    0x01u   /* "PC-AT-compatible dual-8259 IRQ"  */
#define SPCR_LINE_IOAPIC       0x02u   /* "I/O APIC interrupt (GSI)"        */
#define SPCR_LINE_IOSAPIC      0x04u   /* "I/O SAPIC interrupt (GSI)"       */
#define SPCR_LINE_ARM_GIC      0x08u   /* "ARMH GIC interrupt (GSI)"        */
#define SPCR_LINE_RISCV_PLIC   0x10u   /* "RISC-V PLIC/APLIC interrupt"     */
#define SPCR_LINE_ANY_GSI      (SPCR_LINE_IOAPIC | SPCR_LINE_IOSAPIC | \
                                SPCR_LINE_ARM_GIC | SPCR_LINE_RISCV_PLIC)

/* The table revision at which Interface Type stopped being SPCR's own
 * two-value enumeration and became the DBG2 subtype table: "For Revision 2
 * or higher: See the Serial Port Subtypes in Table 3 of the DBG2
 * Specification." */
#define SPCR_REVISION_SPEAKING_DBG2_SUBTYPES  2u

/* DBG2 Table 3 — port types. */
#define DBG2_TYPE_SERIAL   0x8000u
#define DBG2_TYPE_1394     0x8001u
#define DBG2_TYPE_USB      0x8002u
#define DBG2_TYPE_NET      0x8003u

/* DBG2 Table 3 — the serial subtypes whose access rules the spec states. */
#define DBG2_SERIAL_LEGACY_IO    0x0000u  /* "legacy port I/O as seen on x86" */
#define DBG2_SERIAL_DBGP_SUBSET  0x0001u  /* base address only, as DBGP       */
#define DBG2_SERIAL_GAS_DEFINED  0x0012u  /* width and stride from the GAS    */
#define DBG2_SERIAL_INTEL_LPSS   0x0014u

/* ACPI 6.5 §5.2.3.2 Table 5.1. */
#define GAS_SPACE_SYSTEM_MEMORY  0u
#define GAS_SPACE_SYSTEM_IO      1u

/* Whose Generic Address Structure a record's address came out of, because
 * that is what decides whether its Address Space ID field means anything at
 * all. See serial_space. */
typedef enum {
    GasFromDbg2,        /* the field is forced to zero and states nothing */
    GasFromDbgpOrSpcr   /* the field states the space, and is believed    */
} GasOrigin;

/* The x86 I/O address space is 2^16 byte-wide ports (Intel SDM Vol 1 §19.1),
 * so a port number that does not fit sixteen bits is not a port number. */
#define IO_PORT_SPACE_BYTES      0x10000u

/* PC16550D §8: RBR/THR, IER, IIR/FCR, LCR, MCR, LSR, MSR, SCR. */
#define UART_16550_REGISTERS     8u

static const char *serial_subtype_name(uint16_t subtype)
{
    switch (subtype) {
    case 0x0000: return "a fully 16550-compatible port on legacy x86 port I/O";
    case 0x0001: return "a 16550 subset port of the kind DBGP describes";
    case 0x0002: return "a MAX311xE SPI UART";
    case 0x0003: return "an Arm PL011 UART";
    case 0x0004: return "an MSM8x60 UART";
    case 0x0005: return "an NVIDIA 16550";
    case 0x0006: return "a TI OMAP UART";
    case 0x0008: return "an APM88xxxx UART";
    case 0x0009: return "an MSM8974 UART";
    case 0x000A: return "a SAM5250 UART";
    case 0x000B: return "an Intel USIF UART";
    case 0x000C: return "an i.MX 6 UART";
    case 0x000D: return "a deprecated Arm SBSA generic UART (32-bit accesses only)";
    case 0x000E: return "an Arm SBSA generic UART";
    case 0x000F: return "an Arm DCC";
    case 0x0010: return "a BCM2835 UART";
    case 0x0011: return "an SDM845 UART clocked at 1.8432 MHz";
    case 0x0012: return "a 16550 whose register width and stride the table itself states";
    case 0x0013: return "an SDM845 UART clocked at 7.372 MHz";
    case 0x0014: return "an Intel LPSS UART";
    case 0x0015: return "a RISC-V SBI console";
    default:     return "a serial port of a subtype this specification revision does not name";
    }
}

/* Copy the NUL-terminated ASCII namespace path, which DBG2 Table 2 says
 * "must be present", into a bounded buffer. A single '.' is the spec's way
 * of saying the device has no namespace object at all. */
static void copy_namepath(char *dst, size_t dst_size,
                          const uint8_t *rec, uint16_t rec_len,
                          const AcpiDbg2Device *dev)
{
    dst[0] = '\0';
    if (dev->namespace_offset == 0 || dev->namespace_length == 0) return;
    if (dev->namespace_offset >= rec_len) return;

    uint32_t avail = (uint32_t)rec_len - dev->namespace_offset;
    if (avail > dev->namespace_length) avail = dev->namespace_length;

    size_t n = 0;
    for (uint32_t i = 0; i < avail && n + 1 < dst_size; i++) {
        char c = (char)rec[dev->namespace_offset + i];
        if (c == '\0') break;
        /* Anything outside printable ASCII is not a namespace path; stop
         * rather than put control bytes on the console. */
        if (c < 0x20 || c > 0x7E) break;
        dst[n++] = c;
    }
    dst[n] = '\0';
}

/* Which address space a debug port's registers are in — the one question
 * DBG2's Generic Address Structure is forbidden to answer.
 *
 * DBG2, note on the fields of the Generic Address Structure: "The Address
 * Space ID and Register Bit Offset fields must be 0." A zero read there is
 * therefore the value every conforming firmware is required to write, and
 * it says nothing about the machine. What DBG2 gives instead is the
 * subtype, and its note on 16550-based UARTs states the space of two of
 * them outright: 0x0000 "refers to a serial port which uses 'legacy' port
 * I/O as seen on x86-based platforms", while 0x0001 "supports memory mapped
 * UARTs, but only ones that are describable in the DBGP ACPI table".
 *
 * The other two say nothing about it — 0x0012 "supports all serial ports
 * which can be described by the subtypes 0x0 and 0x1, as well as new ones",
 * and 0x0014 names an Intel LPSS part — so there the address is the last
 * thing left that can decide, and it does: the x86 I/O space is 64 KiB
 * wide, and the bottom 64 KiB of physical memory on this machine is the
 * real-mode interrupt vector table, the BIOS data area and conventional
 * DRAM. No device's register window is ever put there, and no I/O port
 * exists above it, so the two ranges cannot be confused with one another.
 *
 * A firmware that writes System I/O into a field its own specification
 * forces to zero has said something it did not have to say, and is taken at
 * its word whatever the subtype.
 *
 * DBGP and SPCR are under no such rule: SPCR's field table works out the
 * legacy port as "COM1 (0x3F8) would be: Integer Form: 0x 01 08 00 00
 * 00000000000003F8", Address Space ID 1. In those two tables the field is a
 * statement, and it is read as one. */
static DebugPortSpace serial_space(GasOrigin origin, uint16_t subtype,
                                   const acpi_gas_t *gas)
{
    if (gas->address_space == GAS_SPACE_SYSTEM_IO) return DebugPortSpaceIo;
    if (origin != GasFromDbg2)                     return DebugPortSpaceMemory;

    switch (subtype) {
    case DBG2_SERIAL_LEGACY_IO:   return DebugPortSpaceIo;
    case DBG2_SERIAL_DBGP_SUBSET: return DebugPortSpaceMemory;
    default:
        return gas->address < IO_PORT_SPACE_BYTES ? DebugPortSpaceIo
                                                  : DebugPortSpaceMemory;
    }
}

/* Turn one Generic Address Structure plus a subtype into the three things
 * the line needs — space, stride, access width — or refuse with a reason.
 * Returns NULL when the record is usable, otherwise the reason. */
static const char *serial_access_rules(GasOrigin origin, uint16_t subtype,
                                       const acpi_gas_t *gas,
                                       uint32_t addr_size, DebugPortDescription *out)
{
    switch (subtype) {
    case DBG2_SERIAL_LEGACY_IO:
    case DBG2_SERIAL_DBGP_SUBSET:
        /* Both are the plain part — eight byte-wide registers one byte
         * apart — and neither takes anything out of the structure but the
         * address: of 0x0001 the spec says "operating system
         * implementations may treat this as equivalent to a DBGP-provided
         * debug port and honor only the Base Address field of the Generic
         * Address Structure", and 0x0000 is the same part on port I/O. */
        out->stride = 1;
        out->access = 1;
        break;

    case DBG2_SERIAL_GAS_DEFINED:
    case DBG2_SERIAL_INTEL_LPSS: {
        /* "The Generic Address Structure in BaseAddressRegister[0] is used
         * to specify the register bit width and access size used by some
         * serial subtypes… The Register Bit Width field contains the
         * register stride and must be a power of 2 that is at least as
         * large as the access size." */
        uint32_t width = gas->bit_width;
        if (width == 0 || (width & (width - 1)) != 0)
            return "its table entry leaves the register stride at a value that is not a power of two, so the table does not say how far apart its registers sit";
        if (width < 8)
            return "its table entry claims a register stride narrower than one byte, which cannot hold a 16550 register";

        uint32_t access_bytes;
        switch (gas->access_size) {
        case 1: access_bytes = 1; break;
        case 2: access_bytes = 2; break;
        case 3: access_bytes = 4; break;
        case 4: return "its table entry asks for 64-bit accesses, and a 16550 register is one byte";
        default: return "its table entry leaves the access size undefined, and this kernel will not guess how wide an access its registers want";
        }

        uint32_t stride_bytes = width / 8u;
        if (access_bytes > stride_bytes)
            return "its table entry asks for accesses wider than the distance between its registers, which the table's own rule forbids";

        out->stride = (uint8_t)stride_bytes;
        out->access = (uint8_t)access_bytes;
        break;
    }

    default:
        return "it is not a part this kernel knows how to drive";
    }

    out->space   = serial_space(origin, subtype, gas);
    out->address = gas->address;
    out->subtype = subtype;

    if (out->address == 0)
        return "its table entry gives the address zero, which names nothing";

    if (out->space == DebugPortSpaceIo) {
        if (out->address >= IO_PORT_SPACE_BYTES)
            return "its table entry puts an I/O port above the 64 KiB the x86 I/O space has";
        if (out->access != 1)
            return "its table entry asks for a multi-byte access to an I/O port, and a 16550 register there is one byte";
    } else {
        if (out->address % out->access != 0)
            return "its table entry puts the registers at an address that is not aligned to the access width it asks for";
    }

    /* The window the firmware declared has to hold the eight registers. A
     * declared size of zero is the firmware saying nothing, not saying none. */
    uint64_t need = (uint64_t)out->stride * UART_16550_REGISTERS;
    if (addr_size != 0 && (uint64_t)addr_size < need)
        return "the window its table entry declares is too small to hold the eight registers a 16550 has";

    return NULL;
}

/* One Debug Device Information record: say what it is, and fill `out` when
 * it is a port this kernel can drive. */
static bool read_dbg2_record(const uint8_t *rec, uint16_t rec_len, uint32_t index,
                             DebugPortDescription *out, bool already_found)
{
    AcpiDbg2Device dev;
    memcpy(&dev, rec, sizeof(dev));

    char namepath[40];
    copy_namepath(namepath, sizeof(namepath), rec, rec_len, &dev);
    const char *named = (namepath[0] && !(namepath[0] == '.' && namepath[1] == '\0'))
                      ? namepath : NULL;

    if (dev.port_type != DBG2_TYPE_SERIAL) {
        const char *what =
            dev.port_type == DBG2_TYPE_USB
                ? (dev.port_subtype == 0x0000
                     ? "an xHCI controller with a debug interface, which needs its own rings and its own device context before a single byte moves"
                     : "an EHCI controller with a debug interface")
          : dev.port_type == DBG2_TYPE_1394 ? "an IEEE 1394 host controller"
          : dev.port_type == DBG2_TYPE_NET  ? "a network adapter"
          : "a port of a type this specification revision does not name";
        kprintf("[DBG2] debug port %u is %s (type 0x%04x subtype 0x%04x)%s%s — "
                "this kernel carries nothing on it\n",
                index, what, (unsigned)dev.port_type, (unsigned)dev.port_subtype,
                named ? ", ACPI object " : "", named ? named : "");
        return false;
    }

    if (dev.gas_count == 0 || dev.base_offset == 0 ||
        (uint32_t)dev.base_offset + sizeof(acpi_gas_t) > rec_len) {
        kprintf("[DBG2] debug port %u says it is %s but carries no address the "
                "table can be read out of, so where it is cannot be known here\n",
                index, serial_subtype_name(dev.port_subtype));
        return false;
    }

    acpi_gas_t gas;
    memcpy(&gas, rec + dev.base_offset, sizeof(gas));

    uint32_t addr_size = 0;
    if (dev.size_offset != 0 &&
        (uint32_t)dev.size_offset + sizeof(uint32_t) <= rec_len) {
        memcpy(&addr_size, rec + dev.size_offset, sizeof(addr_size));
    }

    DebugPortDescription cand;
    memset(&cand, 0, sizeof(cand));
    const char *refused = serial_access_rules(GasFromDbg2, dev.port_subtype,
                                              &gas, addr_size, &cand);

    if (refused) {
        kprintf("[DBG2] debug port %u is %s (subtype 0x%04x)%s%s, and this kernel "
                "leaves it alone: %s\n",
                index, serial_subtype_name(dev.port_subtype),
                (unsigned)dev.port_subtype,
                named ? ", ACPI object " : "", named ? named : "",
                refused);
        return false;
    }

    kprintf("[DBG2] debug port %u is %s at %s 0x%lx, its registers %u byte(s) "
            "apart and read %u byte(s) at a time%s%s\n",
            index, serial_subtype_name(dev.port_subtype),
            cand.space == DebugPortSpaceIo ? "I/O port" : "physical address",
            (unsigned long)cand.address, (unsigned)cand.stride, (unsigned)cand.access,
            named ? ", ACPI object " : "", named ? named : "");

    if (already_found) return false;

    cand.source = "DBG2";
    if (named) strncpy(cand.namepath, named, sizeof(cand.namepath) - 1);
    *out = cand;
    return true;
}

static bool read_dbg2(DebugPortDescription *out)
{
    acpi_sdt_header_t *hdr = acpi_find_table("DBG2");
    if (!hdr) return false;

    if (hdr->length < sizeof(AcpiDbg2Header)) {
        kprintf("[DBG2] the firmware's debug port table is %u bytes, shorter than "
                "the 44 its own header needs — nothing in it can be read\n",
                hdr->length);
        return false;
    }

    AcpiDbg2Header head;
    memcpy(&head, hdr, sizeof(head));

    if (head.info_count == 0 || head.info_offset < sizeof(AcpiDbg2Header) ||
        head.info_offset >= hdr->length) {
        kprintf("[DBG2] the firmware published a debug port table that describes "
                "no port (%u entries at offset %u in %u bytes)\n",
                head.info_count, head.info_offset, hdr->length);
        return false;
    }

    kprintf("[DBG2] the firmware names %u debug port(s) of its own; this table, "
            "not the PC/AT convention, is where this machine says where its "
            "console is\n", head.info_count);

    bool found = false;
    uint32_t offset = head.info_offset;
    for (uint32_t i = 0; i < head.info_count; i++) {
        if (offset + sizeof(AcpiDbg2Device) > hdr->length) {
            kprintf("[DBG2] the table promises %u debug port(s) but runs out of "
                    "bytes after %u — the rest cannot be read\n",
                    head.info_count, i);
            break;
        }
        const uint8_t *rec = (const uint8_t *)hdr + offset;

        uint16_t rec_len;
        memcpy(&rec_len, rec + __builtin_offsetof(AcpiDbg2Device, length), sizeof(rec_len));
        if (rec_len < sizeof(AcpiDbg2Device) || offset + rec_len > hdr->length) {
            kprintf("[DBG2] debug port %u declares a length of %u bytes, which does "
                    "not fit the table it is in — reading stops here\n", i, rec_len);
            break;
        }

        if (read_dbg2_record(rec, rec_len, i, out, found)) found = true;
        offset += rec_len;
    }

    return found;
}

static bool read_dbgp(DebugPortDescription *out)
{
    acpi_sdt_header_t *hdr = acpi_find_table("DBGP");
    if (!hdr) return false;

    if (hdr->length < sizeof(AcpiDbgpTable)) {
        kprintf("[DBGP] the older debug port table is %u bytes, shorter than the "
                "52 it needs to hold an address\n", hdr->length);
        return false;
    }

    AcpiDbgpTable dbgp;
    memcpy(&dbgp, hdr, sizeof(dbgp));

    /* DBGP knows two interface types: 0 = full 16550, 1 = the 16550 subset
     * DBG2 carries forward as subtype 0x0001. Both are byte-wide registers
     * one byte apart; unlike DBG2, DBGP's Address Space ID is meaningful. */
    if (dbgp.interface_type > 1) {
        kprintf("[DBGP] the older debug port table names interface type %u, which "
                "is not a 16550 — this kernel leaves it alone\n",
                (unsigned)dbgp.interface_type);
        return false;
    }

    DebugPortDescription cand;
    memset(&cand, 0, sizeof(cand));
    cand.space   = serial_space(GasFromDbgpOrSpcr, dbgp.interface_type,
                                &dbgp.base_address);
    cand.address = dbgp.base_address.address;
    cand.stride  = 1;
    cand.access  = 1;
    cand.subtype = dbgp.interface_type;
    cand.source  = "DBGP";

    if (cand.address == 0 ||
        (cand.space == DebugPortSpaceIo && cand.address >= IO_PORT_SPACE_BYTES)) {
        kprintf("[DBGP] the older debug port table gives the address 0x%lx, which "
                "names no port this kernel can open\n",
                (unsigned long)cand.address);
        return false;
    }

    kprintf("[DBGP] the older debug port table names a %s at %s 0x%lx\n",
            dbgp.interface_type == 0 ? "fully 16550-compatible port"
                                     : "16550 subset port",
            cand.space == DebugPortSpaceIo ? "I/O port" : "physical address",
            (unsigned long)cand.address);

    *out = cand;
    return true;
}

/* SPCR offset 53: "The PC-AT-compatible IRQ used by the UART: 2-7, 9-12,
 * 14-15 = Valid IRQs respectively. 0-1, 8, 13, 16-255 = Reserved." The four
 * numbers left out are the four the PC/AT gave away before any expansion
 * card could ask: 0 is the timer, 1 the keyboard controller, 8 the real-time
 * clock and 13 the coprocessor error. A UART cannot be on any of them, so a
 * table that says it is has stated something about itself, not about the
 * machine. */
static bool pc_at_irq_is_a_line_a_uart_can_be_on(uint8_t irq)
{
    return (irq >= 2 && irq <= 7) || (irq >= 9 && irq <= 12) ||
           irq == 14 || irq == 15;
}

/* SPCR — the port the firmware was using as its own console, and the line
 * that port raises. That line is the whole reason this table is read: it is
 * the same interrupt that empties the transmitter and that says a key was
 * pressed, so a console opened without it can be written to and never typed
 * on, and neither DBG2 nor DBGP carries it. */
static bool read_spcr(DebugPortDescription *out)
{
    acpi_sdt_header_t *hdr = acpi_find_table("SPCR");
    if (!hdr) return false;

    if (hdr->length < sizeof(AcpiSpcrTable)) {
        kprintf("[SPCR] the console redirection table is %u bytes, and the "
                "address and interrupt this kernel reads out of it end at byte "
                "%u — there is nothing whole in it to read\n",
                hdr->length, (unsigned)sizeof(AcpiSpcrTable));
        return false;
    }

    AcpiSpcrTable spcr;
    memcpy(&spcr, hdr, sizeof(spcr));

    /* Offset 40: "The base address of the Serial Port register set …, or 0 if
     * console redirection is disabled." Zero here is the firmware stating
     * that it uses no serial console, which is an answer and not a defect. */
    if (spcr.base_address.address == 0) {
        kprintf("[SPCR] the firmware published a console redirection table with "
                "the base address zero, which this table defines as redirection "
                "being switched off — it is saying it uses no serial console, "
                "not where one is\n");
        return false;
    }

    /* Interface Type is read by the table's OWN revision. Revision 1 gave the
     * field two values of its own — "0 = Full 16550 interface, 1 = Full 16450
     * interface (must also accept writing to the 16550 FCR register)" — and
     * both are byte-wide registers one byte apart whose address space the
     * Generic Address Structure states, which is exactly the rule DBG2
     * carries as subtype 0x0001. From revision 2 the field IS a DBG2
     * subtype, so it is handed to the same reader as one. */
    bool dbg2_subtypes = hdr->revision >= SPCR_REVISION_SPEAKING_DBG2_SUBTYPES;
    if (!dbg2_subtypes && spcr.interface_type > 1) {
        kprintf("[SPCR] revision %u of this table names interface type %u, and "
                "that revision defines only 0 (a full 16550) and 1 (a full "
                "16450) — this kernel will not guess what it would be driving\n",
                (unsigned)hdr->revision, (unsigned)spcr.interface_type);
        return false;
    }
    uint16_t subtype = dbg2_subtypes ? (uint16_t)spcr.interface_type
                                     : DBG2_SERIAL_DBGP_SUBSET;

    DebugPortDescription cand;
    memset(&cand, 0, sizeof(cand));

    /* No declared window: SPCR states a base address and no size at all, and
     * serial_access_rules reads a zero there as the firmware saying nothing
     * rather than saying none. */
    const char *refused = serial_access_rules(GasFromDbgpOrSpcr, subtype,
                                              &spcr.base_address, 0, &cand);
    if (refused) {
        kprintf("[SPCR] the firmware redirected its console to %s (interface "
                "type %u), and this kernel leaves it alone: %s\n",
                serial_subtype_name(subtype), (unsigned)spcr.interface_type,
                refused);
        return false;
    }
    cand.subtype = (uint16_t)spcr.interface_type;
    cand.source  = "SPCR";
    /* No namespace path: SPCR only grew one at revision 4, past every field
     * read here, and nothing in this kernel needs a name to open a port. */

    if (spcr.interrupt_type & SPCR_LINE_PC_AT_IRQ) {
        if (pc_at_irq_is_a_line_a_uart_can_be_on(spcr.pc_at_irq)) {
            cand.line        = DebugPortLineIsaIrq;
            cand.line_number = spcr.pc_at_irq;
        } else {
            kprintf("[SPCR] this table says its console raises a PC-AT "
                    "interrupt and then gives IRQ %u, which its own field "
                    "table reserves — the number is not believed\n",
                    (unsigned)spcr.pc_at_irq);
        }
    }
    if (cand.line == DebugPortLineNotStated &&
        (spcr.interrupt_type & SPCR_LINE_ANY_GSI) != 0) {
        cand.line        = DebugPortLineGsi;
        cand.line_number = spcr.gsi;
    }

    kprintf("[SPCR] the firmware redirected its own console to %s at %s 0x%lx, "
            "its registers %u byte(s) apart and read %u byte(s) at a time\n",
            serial_subtype_name(subtype),
            cand.space == DebugPortSpaceIo ? "I/O port" : "physical address",
            (unsigned long)cand.address, (unsigned)cand.stride,
            (unsigned)cand.access);

    switch (cand.line) {
    case DebugPortLineIsaIrq:
        kprintf("[SPCR] and it states the line that port raises — PC-AT IRQ %u. "
                "That is the one thing neither DBG2 nor DBGP carries, and it is "
                "what lets a console be typed on rather than only written to\n",
                (unsigned)cand.line_number);
        break;
    case DebugPortLineGsi:
        kprintf("[SPCR] and it states global system interrupt %u for that port, "
                "but no PC-AT IRQ. A global number on its own names a pin and "
                "says nothing about how the pin is wired — no polarity, no "
                "trigger, and no bus whose rule would supply them — so it is "
                "carried as what it is and armed by nobody who cannot describe "
                "it\n", (unsigned)cand.line_number);
        break;
    case DebugPortLineNotStated:
    default:
        kprintf("[SPCR] and it states no interrupt for that port at all "
                "(Interrupt Type 0x%02x), which this table defines as an "
                "interface that only supports polled operation\n",
                (unsigned)spcr.interrupt_type);
        break;
    }

    *out = cand;
    return true;
}

/* The address came from the table that describes this machine's DEBUG port,
 * and neither DBG2 nor DBGP says a word about interrupts. SPCR does — but an
 * interrupt belongs to the device that raises it, so a line stated for one
 * address says nothing about a port at another, and is left where it was
 * found rather than carried across. */
static void adopt_the_stated_line(DebugPortDescription *port,
                                  const DebugPortDescription *console)
{
    if (console->line == DebugPortLineNotStated) return;

    if (console->space != port->space || console->address != port->address) {
        kprintf("[SPCR] the console this table describes sits at %s 0x%lx and "
                "the port %s names sits at %s 0x%lx — two devices, so the line "
                "stated here is not the line that port raises\n",
                console->space == DebugPortSpaceIo ? "I/O port" : "physical address",
                (unsigned long)console->address, port->source,
                port->space == DebugPortSpaceIo ? "I/O port" : "physical address",
                (unsigned long)port->address);
        return;
    }

    port->line        = console->line;
    port->line_number = console->line_number;
    kprintf("[SPCR] this is the same port %s names, so what this kernel opens "
            "arrives with its interrupt as well as its address\n", port->source);
}

bool AcpiDebugPortRead(DebugPortDescription *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* DBG2 first: it "replaces the ACPI Debug Port Table (DBGP) on platforms
     * whose debug port implementations cannot be described using DBGP", so
     * where both exist DBG2 is the one that describes the machine fully. */
    bool named = read_dbg2(out);
    if (!named) {
        DebugPortDescription older;
        if (read_dbgp(&older)) {
            *out  = older;
            named = true;
        }
    }

    /* SPCR is read whatever those two said, because it answers a different
     * question: not "where is the debug port" but "which port was the
     * firmware's own console, and on which line does it interrupt". Where
     * the others named a port, this supplies that port's missing half; where
     * they named none, this is the only machine-written answer there is. */
    DebugPortDescription console;
    bool redirected = read_spcr(&console);

    if (named) {
        if (redirected) adopt_the_stated_line(out, &console);
        return true;
    }

    if (redirected) {
        *out = console;
        return true;
    }

    if (!acpi_find_table("DBG2") && !acpi_find_table("DBGP") &&
        !acpi_find_table("SPCR")) {
        kprintf("[DBG2] this firmware published none of DBG2, DBGP and SPCR, so "
                "nothing on this machine names a debug port or a console, and "
                "the legacy PC/AT address stays a guess\n");
    }
    return false;
}
