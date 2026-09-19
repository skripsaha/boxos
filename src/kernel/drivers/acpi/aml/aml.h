#ifndef ACPI_AML_H
#define ACPI_AML_H

#include "ktypes.h"

/*
 * AML table reader — ACPI 6.5 §20, namespace only.
 *
 * ‼ THERE IS NO INTERPRETER HERE, AND NONE IS MISSING.
 *
 * This file parses the DSDT and every SSDT and builds the namespace: named
 * objects, their types, and what the firmware declared about them. It never
 * runs a Method, never touches an OperationRegion, and never performs an
 * I/O, MMIO or PCI-config access. Nothing was forgotten and nothing is
 * half-finished — an executor was here and was REMOVED, deliberately.
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
 * ‼ AND NOTHING READS THE NAMESPACE TODAY. aml_init() is the whole surface:
 * it builds the objects and announces them on "aml:ready". The lookup calls
 * that used to sit here went out with the sleep path, because that path was
 * their only caller and a function with no caller is garbage. When a real
 * reader arrives — GPE handlers, an embedded controller, a thermal zone —
 * it gets the accessors it actually needs, written then; keeping empty ones
 * warm on spec is the "code for the future" this project refuses. So the
 * open question in this file is not what is missing from it, it is whether
 * a namespace nobody reads should be built at all.
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
 * out loud when the heap cannot follow; scope nesting is capped; a Buffer is
 * capped at 1 MiB.
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

#endif /* ACPI_AML_H */
