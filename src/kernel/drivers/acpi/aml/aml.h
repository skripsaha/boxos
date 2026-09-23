#ifndef ACPI_AML_H
#define ACPI_AML_H

#include "ktypes.h"

/*
 * AML table reader — ACPI 6.5 §20: the namespace, and a reader that asks it
 * exactly one question.
 *
 * ‼ THERE IS NO GENERAL INTERPRETER HERE, AND NONE IS MISSING.
 *
 * This file parses the DSDT and every SSDT and builds the namespace: named
 * objects, their types, and what the firmware declared about them. On top of
 * that sits a NARROW EVALUATOR that answers one question — where does this
 * PCI interrupt pin go — and refuses, aloud, everything that is not on the
 * way to that answer. An executor was here once and was REMOVED with the
 * ACPI S1-S4 sleep path it served, and that removal stands: no sleep path,
 * no Notify, no mutex taken as a means of synchronising with the firmware.
 *
 * Why no S-states: "how the machine goes to sleep" is already answered here
 * in BoxOS's own words — Turn In and Bell: the machine sleeps WHOLE, a
 * strand is laid down until its event rather than polled, and the idle path
 * measured 100% -> 0%. An ACPI S-state would be a second answer to a
 * question that already has one. Powering OFF is a different question with a
 * different answer: acpi_shutdown() is live, and the _S5 values it writes
 * come from the standalone scanner in acpi_dsdt.c, not from anything here.
 *
 * ‼ WHAT THE EVALUATOR RUNS, AND WHAT IT WILL NOT.
 * A PCI interrupt line's true destination is in _PRT, and on every shipping
 * board the road from there to a number runs through AML:
 *
 *     Method (_PRT, 0) { If (PICM) { Return (AR00) } Return (PR00) }
 *     Method (_CRS, 0) { Return (IQCR (PRQ0)) }
 *     Method (IQCR, 1, Serialized) {
 *         Name (PRR0, ResourceTemplate () { Interrupt (...) {0} })
 *         CreateDWordField (PRR0, 0x05, PRRI)
 *         Store (Arg0, PRRI)
 *         Return (PRR0)
 *     }
 *
 * So it runs: If/Else, Return, Store, Name and the CreateXField operators
 * inside a method body, the integer arithmetic and logical operators, method
 * CALLS with their arguments, and the reading of Buffers and Packages. It
 * READS OperationRegions in three address spaces. That is the whole grammar,
 * and everything outside it is refused BY NAME with the method abandoned —
 * never answered with a number nobody computed.
 *
 * ‼ AND IT READS ADDRESS SPACES WITHOUT OWNING ONE.
 *   PCI_Config   -> pci.c's configuration accessors (the only holder of
 *                   0xCF8/0xCFC and of the ECAM window in this kernel).
 *                   A region on a segment other than 0 goes through ECAM
 *                   whatever its offset, because 0xCF8 has no field for a
 *                   segment and would read another segment's device; a
 *                   machine with no MCFG cannot reach it and is told so.
 *                   And the function has to ANSWER: a Vendor ID of 0xFFFF
 *                   means nothing is at the address the namespace led to,
 *                   which is refused rather than read as a register value.
 *   SystemMemory -> vmm_map_mmio, for the length of one field access and not
 *                   one instruction longer. Read-only for a read; writable
 *                   only for the one store described below.
 *   SystemIO     -> io.h
 * The other spaces of ACPI 6.5 Table 5.182 — EmbeddedControl, SMBus,
 * SystemCMOS, PciBarTarget, IPMI, GeneralPurposeIO, GenericSerialBus, PCC,
 * PlatformRtMechanism and FFixedHW — are refused by name: each needs a
 * transport of its own, and none of them carries an interrupt-routing
 * register. So are the codes with no name, and the refusal keeps the two
 * kinds apart as that table does: 0x0C-0x7E is Reserved to the specification,
 * 0x7F is FFixedHW and ACPI-defined, and only 0x80 to 0xFF belongs to the
 * board vendor. (This paragraph used to attribute a reservation of
 * 0x0B..0x7F to §20.2.5.2. §20.2.5.2 is AML grammar and reserves nothing; it
 * names 0x00..0x0A and hands 0x80-0xFF to the OEM, and Table 5.182 is where
 * the rest is written down.)
 *
 * ‼ AND A REGION SAYS WHERE IT IS BEFORE ANYTHING IS READ THROUGH IT.
 * §20.2.5.2 makes RegionOffset and RegionLen TermArgs, and shipping firmware
 * writes a NAME there as readily as a literal — `OperationRegion (RCRB,
 * SystemMemory, \RCBA, 0x4000)` is the Intel chipset form. The parse pass
 * keeps those bytes; AmlRegionArm evaluates them once, at first use, in a
 * run of its own WITH NO WINDOW — so working out where to read can never
 * itself become a read, and the nesting stops at one level by construction.
 * A region whose base or length will not resolve is refused aloud and never
 * touched: a base recorded as zero because nobody could read one is
 * indistinguishable from a base of zero, and zero is an address.
 *
 * ‼ AND IT WRITES IN EXACTLY ONE OF THEM, THROUGH A GATE THAT IS ALL OR
 *   NOTHING.
 *
 * There used to be no function in aml.c that wrote an address space at all,
 * and that was a real guarantee until a real board made it a defect. \_PIC
 * on every modern PCH stores its argument into a field of the firmware's own
 * GNVS block, so a kernel that cannot store cannot tell the platform which
 * interrupt model it runs — and ACPI 6.5 §5.8.1 puts the whole cost of that
 * silence on the OS: "If the platform CPU architecture supports PIC mode and
 * the method is never called, the platform runtime firmware must assume PIC
 * mode." An untold board goes on holding out the table it prepared for the
 * 8259s, and every directly numbered entry in it is then unusable. The old
 * evaluator did worse than refuse: it ENTERED \_PIC, ran as far as the
 * store, and abandoned it there — a firmware method stopped in the middle,
 * which is the one outcome worse than not running it.
 *
 * So the decision moved to before the first instruction. A DRY WALK
 * (AmlGateProve) takes every byte of \_PIC and of every method it calls,
 * goes down BOTH sides of every If, and asks of each operation only whether
 * this evaluator performs it and whether this kernel may carry out each
 * store. Only a method that comes out the far end is run at all, and then
 * with writes allowed for the length of that run and no longer. A method it
 * cannot prove is not entered, and the board is left exactly where it stood.
 *
 * A store that does happen is narrow three times over:
 *   - into a FIELD of a SystemMemory region, never a region named whole,
 *     never an IndexField or a BankField (reaching either means writing its
 *     selector first, and two accesses in a fixed order is a protocol);
 *   - only where the field fills whole access units, because §20.2.5.2's
 *     UpdateRule governs the bits beside a partial field and this pass does
 *     not record it — which also means placing a value never has to read the
 *     register first;
 *   - only at a physical address the board's OWN memory map declares
 *     AddressRangeNVS (ACPI 6.5 Table 15.1). "SystemMemory" alone is not
 *     enough and never was: a SystemMemory region is as often an MMIO
 *     window as it is firmware scratch RAM, and the memory map is the only
 *     thing on the machine that tells them apart.
 * Every other address space this evaluator can name is refused for WRITING
 * by name, each with its own reason printed — see AmlSpaceWriteReason in
 * aml.c, which takes them one at a time. And _SRS — the operator that would CONFIGURE an
 * interrupt link that is carrying nothing — stays a capability this kernel
 * does not have: the register it would write is in SystemIO or PCI
 * configuration space on every board.
 *
 * ‼ THE WINDOW. An address space is reachable only while one of five objects
 * is being evaluated: _PRT (§6.2.13), _CRS (§6.2.2), _PRS (§6.2.12), _STA
 * (§6.3.7), _PIC (§5.8.1). It is not a flag that somebody clears — it is a
 * field of a structure that lives in the C activation record of the one
 * function that evaluates such an object, assigned in exactly one place from
 * the NAME of the object being evaluated. Closing it is that function
 * returning, which happens on every path including every refusal.
 *
 * It never answers zero in place of a refusal. Every path that cannot
 * produce a value returns false and prints what it could not do, because a
 * fabricated value here is a GSI, and a fabricated GSI is a device wired to
 * an interrupt nobody delivers.
 *
 * Its depth and its running time are bounded BY CONSTRUCTION, not by a
 * counter: statements, expressions and method calls are all driven by one
 * loop over explicit heap-backed stacks, so the C stack never descends with
 * the firmware's nesting; and there is no loop opcode at all — DefWhile is
 * refused by name — so every block is a finite byte range walked forward
 * once and the only way to make more work is to call a method, which costs
 * an activation the heap has to supply.
 *
 * Supported declarations (real-HW core surface):
 *   Namespace:  NameOp, ScopeOp, MethodOp, DeviceOp, OpRegionOp, FieldOp,
 *               IndexFieldOp, BankFieldOp, AliasOp, MutexOp, EventOp,
 *               ProcessorOp, PowerResOp, ThermalZoneOp
 *   Data:       ZeroOp/OneOp/OnesOp, BytePrefix, WordPrefix, DWordPrefix,
 *               QWordPrefix, StringPrefix, BufferOp, PackageOp, VarPackageOp
 *
 * A Method is recorded with its flags and the byte range of its TermList, so
 * the object describes the firmware faithfully; it is not run. An
 * OperationRegion is recorded with its address space, base and length, and
 * each Field with its bit offset, width and access rule; none of them is
 * accessed.
 *
 * How exact the walk is depends on WHERE a term stands, and the boundary
 * matters more than the word "exact" does:
 *   - IN AN OPERAND — RegionOffset, RegionLen, BankValue, the arguments of
 *     CreateField — an executable term IS measured and stepped over exactly.
 *     Add, Divide, Store, the logical and reference operators and every
 *     sub-opcode under ExtOpPrefix (a closed set, Table 20.2) have known
 *     operand shapes, and that is what keeps a declaration's operands from
 *     being mistaken for declarations. An extended opcode this pass cannot
 *     measure says so on the console and resynchronises.
 *   - AT TERMOBJ LEVEL the scan knows only the declarations listed above.
 *     An If, While or Return standing there is NOT measured: the TermList
 *     steps ONE BYTE and tries again until a declaration opcode lands, and
 *     that resync is silent. It is a known limit of this pass — declarations
 *     nested behind control flow can be missed — not a property of it.
 *
 * ‼ THE TWO SHAPES A _PRT ENTRY COMES IN, AND BOTH ARE IMPLEMENTED.
 * When Source is the integer zero, §6.2.13 makes Source Index the global
 * system interrupt directly — the ordinary APIC-mode form, the AR00 package
 * of the Intel reference ASL. When Source names a PCI Interrupt Link
 * (_HID PNP0C0F), the number is the link's CURRENT setting and lives in the
 * chipset's PIRQ routing register, which the link's _CRS reads through an
 * OperationRegion. Both are answered here.
 *
 * ‼ AND A LINK'S _CRS ANSWERS THE WIRE AS WELL AS THE NUMBER, WHICH PCI 3.0
 * §2.2.6 ALONE CANNOT. §2.2.6's level-sensitive, asserted-low, open-drain
 * INTx# ends at the PIRQ router; what the interrupt controller sees is the
 * router's OUTPUT. On a legacy platform that output is one of the sixteen
 * 8259 lines and is active HIGH — the emulator on this desk says so in the
 * link's descriptor and again in a MADT override — while on a modern PCH
 * PIRQA-H# reach IO-APIC inputs 16-23 and are active low. The link's own
 * _CRS is right on both, so it is believed, and §2.2.6 stays as the default
 * for a line nothing described. A descriptor that disagrees with §2.2.6 is
 * said out loud, because on a live board that is the first thing an operator
 * needs to see.
 *
 * Bounded by construction: the object pool is a chain of chunks that grows
 * with the firmware (an ASRock B365M Pro4-F declares 4615 objects) and says
 * out loud when the heap cannot follow. NOTHING in this file caps how deeply
 * a board may nest, in either pass: the namespace walk carries its own
 * heap-grown stack of bodies instead of calling itself, and stepping over an
 * operand carries a heap-grown list of what it still owes instead of
 * recursing — so neither descends the C stack, and the 32 and the 16 that
 * used to stand there (one of which dropped an entire subtree, silently) are
 * gone. A Buffer is not capped by any number
 * written here — ACPI 6.5 §20.2.5.4 lets its declared BufferSize exceed its
 * ByteList, the tail being zero, so what may be READ is the bytes the
 * firmware actually wrote, inside a package length it wrote, inside a table
 * whose own header says how long it is. The 1 MiB that stood here was an
 * invention, and it sat beside a length that was stored unclamped.
 */

typedef enum {
    AML_OK = 0,
    /* The term was MEASURED IN FULL and then refused — its package length
     * was read, the stream already stands at the next TermObj, and what was
     * rejected is only the declaration inside it. The TermList must not then
     * resynchronise a byte at a time over a block whose length it knows:
     * scanning a Device body byte by byte declares objects out of misaligned
     * operand bytes, which is the hazard handle_extended already refuses to
     * take with the extended opcodes. That is the whole difference from
     * AML_ERR_BAD_OPCODE, which says the opposite — these bytes could not be
     * measured, and stepping one byte is all that is left to do. */
    AML_REFUSED = 1,
    AML_ERR_NOT_LOADED = -1,
    AML_ERR_BAD_OPCODE = -2,
    /* -3 was AML_ERR_DEEP_NEST: the namespace walk gave up at a nesting
     * depth of 32 and lost the whole subtree below it. The walk no longer
     * descends the C stack, so there is no depth to give up at. */
    AML_ERR_BOUNDS     = -4,
    AML_ERR_NOMEM      = -5,
} aml_status_t;

typedef enum {
    /* Zero on purpose: an object that has been allocated and zeroed is
     * UNTYPED, not an Integer. Do not renumber this away. */
    AML_OBJ_UNINIT = 0,
    AML_OBJ_INTEGER,
    AML_OBJ_STRING,
    AML_OBJ_BUFFER,
    AML_OBJ_PACKAGE,
    AML_OBJ_DEVICE,
    AML_OBJ_METHOD,
    AML_OBJ_REGION,
    AML_OBJ_FIELD,
    AML_OBJ_SCOPE,
    AML_OBJ_NAME,
    AML_OBJ_MUTEX,
    AML_OBJ_EVENT,
    AML_OBJ_POWER_RES,
    AML_OBJ_PROCESSOR,
    AML_OBJ_THERMAL,
    AML_OBJ_ALIAS,
} aml_obj_type_t;

typedef struct aml_object aml_object_t;

/* Parse the DSDT and every SSDT into the namespace, once. */
aml_status_t aml_init(void);

/* ===========================================================================
 * WHERE A PCI INTERRUPT LINE ACTUALLY GOES — ACPI 6.5 §6.2.13, _PRT.
 *
 * The third and most trusted source of a Summons. The two below it are the
 * PCI bus rule (level, active low, shareable — what an INTx wire IS) and the
 * Interrupt Line byte at config offset 0x3C, which PCI 3.0 §6.2.4 footnote 43
 * defines in terms of "IRQ numbers (0-15) of the standard dual 8259
 * configuration". On a machine running its IO-APIC that byte is firmware
 * convention at best. _PRT is the board's own statement, and it is the
 * answer.
 *
 * §6.2.13 gives one entry as Package { Address, Pin, Source, SourceIndex }:
 * Address is the device in its high word with 0xFFFF in the low word, Pin is
 * 0..3 for INTA#..INTD#, and Source is either the integer zero — "the
 * interrupt is allocated from the global interrupt pool", SourceIndex being
 * the global system interrupt number — or the name of a PCI Interrupt Link
 * device (_HID PNP0C0F) whose current setting comes out of its _CRS.
 *
 * ‼ THE CALLER SUPPLIES THE PCI PATH, NOT JUST A BUS NUMBER. A _PRT describes
 * the bus directly beneath the device that carries it, so a function behind
 * a PCI-to-PCI bridge is answered by that bridge's own _PRT when the firmware
 * declared one, and otherwise by the pin rotation of the PCI-to-PCI Bridge
 * Architecture Specification 1.2 §9.1 applied at each level up to the root.
 * Which buses lie between is a fact about the machine's config space, which
 * is pci.c's to know; which object describes them is this file's.
 *
 * `path` runs from the root bus DOWNWARD: path[0] is the first bridge on the
 * root bus, path[depth-1] is the function being asked about. depth == 1 means
 * the function sits on the root bus itself.
 *
 * ‼ \_PIC IS THREE FACTS, NOT A BOOLEAN. A board that declares no \_PIC
 * (§5.8.1 makes that legal) has ONE routing table and it is believed. A
 * board whose \_PIC ran has told the platform which model this kernel runs,
 * which is the strongest case there is. A board whose \_PIC is declared and
 * would not run leaves the table variant unknown — and that costs exactly
 * the DIRECTLY numbered entries, whose meaning depends on the variant; an
 * entry that names an interrupt link is still followed, because the link's
 * _CRS reads the chipset's routing register, which holds one number in
 * either interrupt model.
 *
 * False with the reason printed whenever the board did not answer — no
 * namespace, no root bridge for that segment and bus, no entry for that
 * device and pin, a directly numbered entry on a board whose \_PIC would not
 * run, or a _CRS this evaluator cannot read without touching hardware. The
 * caller then keeps its weaker source; nothing is silently substituted.
 *
 * `wiring_known` is false for a directly-numbered entry: _PRT states which
 * GSI, never how it is driven, so the PCI bus rule answers the rest and
 * inventing a polarity from silence would be the defect the Summons exists
 * to close. It is true when a PCI Interrupt Link's own _CRS described the
 * pin — which is a statement about the router's output and outranks both the
 * bus rule and a MADT ISA override; see the note in aml.c above
 * AmlIrqFromResources for why, measured on two kinds of machine.
 * ========================================================================= */
typedef struct AmlPciStep {
    uint8_t device;
    uint8_t function;
} AmlPciStep;

/* Tell the platform which interrupt model OSPM is using, and read what it
 * then says about interrupt routing. ACPI 6.5 §5.8.1 makes the call itself
 * optional — "Notice that calling this method is optional for OSPM" — and
 * makes staying silent expensive all the same: "If the platform CPU
 * architecture supports PIC mode and the method is never called, the
 * platform runtime firmware must assume PIC mode." So the first half is
 * owed whether or not any PCI line ever needs the second, because an untold
 * firmware goes on holding out the description it prepared for the 8259s.
 * The model is asked of the interrupt controller, so this
 * belongs after irqchip_init and before the kernel registers its first
 * platform interrupt — which is where acpi_sci_register calls it. Idempotent,
 * and a no-op on a machine whose firmware declares neither \_PIC nor a PCI
 * root bridge. */
void AmlRoutingAdopt(void);

typedef struct AmlRoute {
    uint32_t    gsi;
    bool        wiring_known;
    bool        active_low;
    bool        level;
    bool        shareable;
    const char *named_by;      /* for the log line that says who answered */
} AmlRoute;

bool AmlRouteForPciPin(uint16_t segment, uint8_t root_bus,
                       const AmlPciStep *path, uint32_t depth,
                       uint8_t intx_pin, AmlRoute *out);

#endif /* ACPI_AML_H */
