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
 * objects, their types, and what the firmware declared about them. It never
 * touches an OperationRegion, and never performs an I/O, MMIO or PCI-config
 * access. Nothing was forgotten and nothing is half-finished — an executor
 * was here and was REMOVED, deliberately, with the whole hardware-access
 * path that made it an executor.
 *
 * Why: the only thing that ever reached it was the ACPI S1-S4 sleep path,
 * and BoxOS does not sleep through ACPI. "How the machine goes to sleep" is
 * already answered here in BoxOS's own words — Turn In and Bell: the machine
 * sleeps WHOLE, a strand is laid down until its event rather than polled, and
 * the idle path measured 100% -> 0%. An ACPI S-state would be a second answer
 * to a question that already has one, which is mimicry of a paradigm this
 * project rejects on purpose, and the code behind it was unreachable in the
 * bargain. Powering OFF is a different question with a different answer:
 * acpi_shutdown() is live, and the _S5 values it writes come from the
 * standalone scanner in acpi_dsdt.c, not from anything in this file.
 *
 * ‼ WHAT DOES RUN, AND WHY IT IS NOT THE EXECUTOR COMING BACK.
 * A PCI interrupt line's true destination is in _PRT, and every board on
 * earth writes _PRT as either a Package or four lines of AML:
 *
 *     Method (_PRT, 0) { If (PICM) { Return (AR00) } Return (PR00) }
 *
 * So this file grew a NARROW EVALUATOR: If/Else, Return, Store into a local,
 * an argument or a namespace Name, the logical operators, and the reading of
 * Packages and Buffers. That is the whole grammar. The instant a term needs
 * a value out of an OperationRegion — the only way AML ever reaches a
 * register — the evaluator REFUSES ALOUD and abandons the method rather than
 * inventing a number for it. That refusal is the boundary, not a gap: the
 * line falls back to its weaker source and says so on the console.
 *
 * It never answers zero in place of a refusal. Every path that cannot
 * produce a value returns false and prints what it could not do, because a
 * fabricated value here is a GSI, and a fabricated GSI is a device wired to
 * an interrupt nobody delivers.
 *
 * Its depth is bounded by construction rather than by a counter: statements
 * and expressions are walked with explicit heap-backed stacks so the C stack
 * never descends, and a NameString that resolves to a Method is refused in
 * expression position — so exactly ONE method frame exists at a time and
 * there is no chain to limit.
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
 * Bounded by construction: the object pool is a chain of chunks that grows
 * with the firmware (an ASRock B365M Pro4-F declares 4615 objects) and says
 * out loud when the heap cannot follow; scope nesting is capped, because
 * that pass DOES descend the C stack. A Buffer is not capped by any number
 * written here — ACPI 6.5 §20.2.5.4 lets its declared BufferSize exceed its
 * ByteList, the tail being zero, so what may be READ is the bytes the
 * firmware actually wrote, inside a package length it wrote, inside a table
 * whose own header says how long it is. The 1 MiB that stood here was an
 * invention, and it sat beside a length that was stored unclamped.
 */

typedef enum {
    AML_OK = 0,
    AML_ERR_NOT_LOADED = -1,
    AML_ERR_BAD_OPCODE = -2,
    AML_ERR_DEEP_NEST  = -3,
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
 * False with the reason printed whenever the board did not answer — no
 * namespace, no \_PIC and a link-sourced entry, no root bridge for that
 * segment and bus, no entry for that device and pin, or a _CRS this evaluator
 * cannot read without touching hardware. The caller then keeps its weaker
 * source; nothing is silently substituted.
 *
 * `wiring_known` is false for a directly-numbered entry: _PRT states which
 * GSI, never how it is driven. The bus rule already answers that, and
 * inventing a polarity here would be the very defect the Summons exists to
 * close. It is true only when a link's _CRS described the wire itself.
 * ========================================================================= */
typedef struct AmlPciStep {
    uint8_t device;
    uint8_t function;
} AmlPciStep;

/* Tell the platform which interrupt model OSPM is using, and read what it
 * then says about interrupt routing. ACPI 6.5 §5.8.1 makes the first half a
 * duty of its own, owed whether or not any PCI line ever needs the second:
 * "OSPM must call the \_PIC method to inform the platform of the interrupt
 * model in use". The model is asked of the interrupt controller, so this
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
