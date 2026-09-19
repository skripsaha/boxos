#ifndef ACPI_AML_H
#define ACPI_AML_H

#include "ktypes.h"

/*
 * AML interpreter — production subset of ACPI 6.5 §20.
 *
 * Supported opcodes (real-HW core surface):
 *   Namespace:    NameOp, ScopeOp, MethodOp, DeviceOp, OpRegionOp,
 *                 FieldOp, IndexFieldOp, BankFieldOp, AliasOp
 *   Data:         ZeroOp/OneOp/OnesOp, BytePrefix, WordPrefix,
 *                 DWordPrefix, QWordPrefix, StringPrefix, BufferOp,
 *                 PackageOp, VarPackageOp
 *   Arithmetic:   AddOp, SubtractOp, MultiplyOp, DivideOp, IncrementOp,
 *                 DecrementOp, AndOp, OrOp, XorOp, NotOp, ShiftLeftOp,
 *                 ShiftRightOp, ModOp
 *   Logical:      LAndOp, LOrOp, LNotOp, LEqualOp, LGreaterOp,
 *                 LLessOp, LGreaterEqualOp, LLessEqualOp, LNotEqualOp
 *   Control:      IfOp, ElseOp, WhileOp, ReturnOp, BreakOp, ContinueOp
 *   Reference:    StoreOp, RefOfOp, DerefOfOp, IndexOp, SizeOfOp
 *   Extended:     Stall, Sleep, Acquire, Release, Signal, Wait, Reset,
 *                 CondRefOf, FromBCD, ToBCD, Fatal (ExtOpPrefix 0x5B).
 *                 Every other defined 0x5B opcode has its operands
 *                 consumed exactly and is then refused out loud — the
 *                 stream is never re-entered at a sub-opcode byte.
 *   Field access: through OpRegion address-space handlers for SystemIO,
 *                 SystemMemory and PCI_Config only; IndexField goes
 *                 through its Index/Data register pair and BankField
 *                 selects its bank before every access
 *
 * ‼ AN UNHANDLED ADDRESS SPACE IS SILENT, AND THERE IS NO STATUS TO CHECK
 *
 * What stood here claimed that address spaces other than the three above
 * are "reported as BAD_OPCODE so callers can detect unsupported HW". No
 * such report exists, and none of the API can carry one. ACPI 6.5 §20.2.5.2
 * defines eleven RegionSpace values — EmbeddedControl (0x03), SMBus (0x04),
 * System CMOS (0x05), PciBarTarget (0x06), IPMI (0x07), GeneralPurposeIO
 * (0x08), GenericSerialBus (0x09), PCC (0x0A) beyond the three served here
 * — and every one of them is admitted to the namespace looking exactly like
 * a serviceable region: the RegionSpace byte is not validated at parse time,
 * aml_region_read ends in `default: return 0`, aml_region_write's switch has
 * no default at all, and neither field_read/field_write nor exec_store has a
 * status channel to carry a refusal even if one were raised. So a read of an
 * EmbeddedControl field yields zero — indistinguishable, to the AML, from an
 * EC that answered zero — and a write to one is dropped while the method
 * runs on and the executor returns AML_OK.
 *
 * AML_ERR_BAD_OPCODE could not serve that purpose either, and not because
 * it never appears: the executor raises it in a dozen places — an extended
 * opcode whose operands cannot be stepped over, a SuperName that resolves to
 * nothing — and exec_termlist and aml_call_int hand it straight back, so a
 * caller of this header's aml_call_int can and does receive it. What it
 * never means is an address space. No region path raises it; the one thing
 * it tells a caller is that a byte of the method could not be decoded. On
 * the parse side it does not even survive: parse_term_list swallows it to
 * resynchronise the stream and returns AML_OK, so aml_init never reports
 * one. AML_ERR_REGION exists in the enum below, and is the value this would
 * need — nothing produces it.
 *
 * Nothing at HEAD suffers from this — the two entry points that can reach a
 * region access, aml_call_int via acpi_enter_sleep and aml_call_pic, have no
 * callers, and the one live entry, aml_init() from main.c, only builds the
 * namespace. Giving the region path a status return is the work that must
 * come before the first caller that needs an EC.
 *
 * Out of scope for this audit (deferred to ACPICA/uACPI follow-up):
 *   Contended Mutex/Event semantics — nothing in the tree evaluates AML
 *     on two cores, so Acquire always succeeds and Wait always reports
 *     the timeout it is asked to report
 *   Power/Thermal/Processor object semantics beyond namespace insertion
 *   Buffer fields (CreateBitField/CreateByteField/...)
 *   Reference counting + GC of objects across method boundaries
 *   Notify dispatch (the byte is parsed but no listeners run yet)
 *   ToBuffer/ToHexString/ToInteger generic conversion suite
 *   ConcatOp, MatchOp, ObjectTypeOp
 *
 * Designed to be safe to call from kernel context: every executor
 * frame is bounded (256 max), every Buffer/Package is hard-capped to
 * 1 MiB, every While bound to 4 million iterations.
 */

typedef enum {
    AML_OK = 0,
    AML_ERR_NOT_LOADED = -1,
    AML_ERR_NOT_FOUND  = -2,
    AML_ERR_BAD_OPCODE = -3,
    AML_ERR_DEEP_NEST  = -4,
    AML_ERR_TYPE       = -5,
    AML_ERR_BOUNDS     = -6,
    AML_ERR_NOMEM      = -7,
    AML_ERR_REGION     = -8,
    /* Method execution was abandoned on purpose and the interpreter has
     * already said why on the console: a divide by zero (ACPI 6.5
     * §19.6.32, ACPICA AE_AML_DIVIDE_BY_ZERO), a Fatal() from the
     * firmware, an opcode this executor refuses to guess at, or a delay
     * it could not honour. A partial ret_value is not to be trusted. */
    AML_ERR_ABORT      = -9,
} aml_status_t;

typedef enum {
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

aml_status_t aml_init(void);
aml_status_t aml_call_pic(uint32_t mode);
aml_status_t aml_eval(const char* path, aml_object_t** args, int argc,
                       aml_object_t** ret);
aml_status_t aml_find(const char* path, aml_object_t** out);
aml_status_t aml_eval_crs(const char* path, void* buf, size_t buf_len,
                          size_t* used);

/* Read 64-bit integer from a named object (Name, return value, etc.).
 * Convenience for callers like the shutdown path that only need _S5
 * elements. Returns AML_ERR_TYPE if the object isn't an integer. */
aml_status_t aml_read_integer(const char* path, uint64_t* out);

/* Invoke a Method by absolute path with up to 7 integer arguments.
 * Returns the integer return value via `*ret_value` (zero when the
 * method returns no value or a non-integer). The full ASL value model
 * collapses to integers here — sufficient for _PTS / _BFS / _PIC /
 * _STA / _INI which take and return integers. */
aml_status_t aml_call_int(const char* path, uint64_t* args, int argc,
                           uint64_t* ret_value);

#endif /* ACPI_AML_H */
