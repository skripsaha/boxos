#ifndef ACPI_AML_H
#define ACPI_AML_H

#include "ktypes.h"

/*
 * AML interpreter — production subset of ACPI 6.5 §20.
 *
 * Supported opcodes (real-HW core surface):
 *   Namespace:    NameOp, ScopeOp, MethodOp, DeviceOp, OpRegionOp,
 *                 FieldOp, AliasOp
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
 *   Field access: through OpRegion address-space handlers (SystemIO,
 *                 SystemMemory, PCI_Config — others reported as
 *                 BAD_OPCODE so callers can detect unsupported HW)
 *
 * Out of scope for this audit (deferred to ACPICA/uACPI follow-up):
 *   Mutex/Event synchronisation primitives beyond stub
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
