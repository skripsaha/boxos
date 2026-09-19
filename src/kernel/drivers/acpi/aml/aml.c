#include "aml.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"
#include "pci.h"
#include "touch.h"
#include "hourglass.h"
#include "atomics.h"

/* =====================================================================
 * AML interpreter — production subset of ACPI 6.5 §20.
 *
 * Coverage:
 *   * Namespace build from DSDT + every SSDT
 *   * Named objects: Name, Scope, Method, Device, Processor, PowerRes,
 *     ThermalZone, OpRegion, Field, IndexField, BankField, Mutex,
 *     Event, Alias. Every other DEFINED extended opcode is stepped
 *     over exactly, never rescanned byte by byte.
 *   * Data types: Integer (Zero/One/Ones + Byte/Word/DWord/QWord prefix),
 *     String, Buffer, Package
 *   * Lookup with C-string paths (`\_SB.PCI0._INI`) including relative
 *     forms with parent-prefix walk
 *   * OpRegion handlers for SystemIO, SystemMemory, PCI_Config — all
 *     three actually drive hardware
 *   * Integer-value retrieval for callers that just need a Name (shutdown
 *     reads \_S5 this way)
 *   * _PIC method presence detection
 *
 * The integer-valued method executor lives at the bottom of this file.
 * Out of scope (acknowledged in the header) — buffer fields, generic
 * conversions, Notify dispatch, and the object/reference model a
 * CondRefOf destination would need. An opcode outside that subset
 * stops the method and says which byte stopped it; it is never stepped
 * over one byte at a time and reported as success.
 * ===================================================================== */

#define AML_MAX_NAMESPACE_OBJ   4096
#define AML_MAX_SCOPE_DEPTH     32
#define AML_MAX_BUFFER_BYTES    (1u << 20)
#define AML_MAX_PACKAGE_LEN     1024

/* AML opcodes — ACPI 6.5 §20.2.5. */
#define ZeroOp           0x00
#define OneOp            0x01
#define AliasOp          0x06
#define NameOp           0x08
#define BytePrefix       0x0A
#define WordPrefix       0x0B
#define DWordPrefix      0x0C
#define StringPrefix     0x0D
#define QWordPrefix      0x0E
#define ScopeOp          0x10
#define BufferOp         0x11
#define PackageOp        0x12
#define VarPackageOp     0x13
#define MethodOp         0x14
#define DualNamePrefix   0x2E
#define MultiNamePrefix  0x2F
#define ExtOpPrefix      0x5B
#define RootChar         0x5C
#define ParentPrefixChar 0x5E
#define OnesOp           0xFF

/* Extended opcodes — the byte that follows ExtOpPrefix (ACPI 6.5 §20.2.3
 * Table 20.2). The set is closed, which is what lets an unimplemented one
 * be stepped over exactly instead of rescanned byte by byte. */
#define ExtMutexOp       0x01
#define ExtEventOp       0x02
#define ExtCondRefOfOp   0x12
#define ExtCreateFieldOp 0x13
#define ExtLoadTableOp   0x1F
#define ExtLoadOp        0x20
#define ExtStallOp       0x21
#define ExtSleepOp       0x22
#define ExtAcquireOp     0x23
#define ExtSignalOp      0x24
#define ExtWaitOp        0x25
#define ExtResetOp       0x26
#define ExtReleaseOp     0x27
#define ExtFromBCDOp     0x28
#define ExtToBCDOp       0x29
#define ExtUnloadOp      0x2A
#define ExtRevisionOp    0x30
#define ExtDebugOp       0x31
#define ExtFatalOp       0x32
#define ExtTimerOp       0x33
#define ExtOpRegionOp    0x80
#define ExtFieldOp       0x81
#define ExtDeviceOp      0x82
#define ExtProcessorOp   0x83
#define ExtPowerResOp    0x84
#define ExtThermalZoneOp 0x85
#define ExtIndexFieldOp  0x86
#define ExtBankFieldOp   0x87
#define ExtDataRegionOp  0x88

/* Executable opcodes (single-byte). */
#define StoreOp          0x70
#define RefOfOp          0x71
#define AddOp            0x72
#define ConcatOp         0x73
#define SubtractOp       0x74
#define IncrementOp      0x75
#define DecrementOp      0x76
#define MultiplyOp       0x77
#define DivideOp         0x78
#define ShiftLeftOp      0x79
#define ShiftRightOp     0x7A
#define AndOp            0x7B
#define NandOp           0x7C
#define OrOp             0x7D
#define NorOp            0x7E
#define XorOp            0x7F
#define NotOp            0x80
#define DerefOfOp        0x83
#define ModOp            0x85
#define SizeOfOp         0x87
#define IndexOp          0x88
#define LAndOp           0x90
#define LOrOp            0x91
#define LNotOp           0x92
#define LEqualOp         0x93
#define LGreaterOp       0x94
#define LLessOp          0x95
#define ContinueOp       0x9F
#define IfOp             0xA0
#define ElseOp           0xA1
#define WhileOp          0xA2
#define ReturnOp         0xA4
#define BreakOp          0xA5

/* Address spaces. */
#define REGION_SYSTEM_MEMORY  0
#define REGION_SYSTEM_IO      1
#define REGION_PCI_CONFIG     2

/* ===========================================================
 * Namespace data structures
 * =========================================================== */

struct aml_object {
    char        name[5];
    aml_obj_type_t type;
    struct aml_object* parent;
    struct aml_object* child;
    struct aml_object* sibling;

    union {
        uint64_t integer;
        struct { const uint8_t* bytes; uint32_t len; } buffer;
        struct {
            uint8_t  space;
            uint64_t base;
            uint64_t len;
            /* MMIO cache: first SystemMemory access maps the region UC
             * once; every subsequent read/write is a direct load/store
             * with zero VMM calls. NULL until the first miss. */
            volatile uint8_t* mapped_virt;
            /* False when RegionOffset/RegionLen were a TermArg this
             * one-pass builder cannot evaluate, or when the declared
             * length is zero. Such a region has no address — it must
             * refuse every access rather than answer for address 0. */
            bool usable;
            /* An out-of-range access has already been named on the
             * console for this region; do not repeat it per bit. */
            bool refused;
        } region;
        struct {
            const uint8_t* aml;
            uint32_t       aml_len;
            uint8_t        flags;
        } method;
        struct { const char* str; uint32_t len; } string;
        struct { uint32_t count; uint64_t first_int; } package;
        /* One shape for the three FieldList producers of §20.2.5.2.
         * DefField binds `region`; DefIndexField binds `index`/`data`
         * and has no region of its own; DefBankField binds `region`
         * plus the `bank` selector that must carry `bank_value` before
         * the data is touched. */
        struct {
            struct aml_object* region;
            struct aml_object* index;
            struct aml_object* data;
            struct aml_object* bank;
            uint64_t bank_value;
            uint32_t bit_offset;
            uint32_t bit_width;
            uint8_t  access;
        } field;
        struct { struct aml_object* target; } alias;
    } v;
};

static aml_object_t  g_root_obj;
static aml_object_t  g_pool[AML_MAX_NAMESPACE_OBJ];
static uint32_t      g_pool_used = 0;
static bool          g_loaded    = false;

static aml_object_t* obj_alloc(void) {
    if (g_pool_used >= AML_MAX_NAMESPACE_OBJ) return NULL;
    aml_object_t* o = &g_pool[g_pool_used++];
    memset(o, 0, sizeof(*o));
    return o;
}

static aml_object_t* obj_create_child(aml_object_t* parent, const char seg[4],
                                       aml_obj_type_t type) {
    aml_object_t* o = obj_alloc();
    if (!o) return NULL;
    o->name[0] = seg[0]; o->name[1] = seg[1];
    o->name[2] = seg[2]; o->name[3] = seg[3]; o->name[4] = 0;
    o->type = type;
    o->parent = parent;
    o->sibling = parent->child;
    parent->child = o;
    return o;
}

static aml_object_t* obj_find_child(aml_object_t* parent, const char seg[4]) {
    for (aml_object_t* c = parent->child; c; c = c->sibling) {
        if (c->name[0] == seg[0] && c->name[1] == seg[1] &&
            c->name[2] == seg[2] && c->name[3] == seg[3]) {
            if (c->type == AML_OBJ_ALIAS && c->v.alias.target)
                return c->v.alias.target;
            return c;
        }
    }
    return NULL;
}

/* ===========================================================
 * NameString resolution
 * =========================================================== */

static aml_object_t* resolve_name(const uint8_t** p, const uint8_t* end,
                                    aml_object_t* scope, bool create,
                                    char leaf_out[4]) {
    if (*p >= end) return NULL;
    aml_object_t* base = scope;
    if (**p == RootChar) { base = &g_root_obj; (*p)++; }
    while (*p < end && **p == ParentPrefixChar) {
        if (base->parent) base = base->parent;
        (*p)++;
    }
    if (*p >= end) return base;

    int segs = 1;
    uint8_t op = **p;
    if (op == 0) { (*p)++; return base; }
    if (op == DualNamePrefix) { (*p)++; segs = 2; }
    else if (op == MultiNamePrefix) {
        (*p)++;
        if (*p >= end) return NULL;
        segs = **p; (*p)++;
        if (segs > 16) return NULL;
    }
    if (*p + (uint32_t)segs * 4 > end) return NULL;

    aml_object_t* cur = base;
    for (int i = 0; i < segs; i++) {
        char seg[4] = { (char)(*p)[0], (char)(*p)[1], (char)(*p)[2], (char)(*p)[3] };
        (*p) += 4;
        if (i == segs - 1 && create) {
            if (leaf_out) { leaf_out[0]=seg[0]; leaf_out[1]=seg[1];
                            leaf_out[2]=seg[2]; leaf_out[3]=seg[3]; }
            return cur;
        }
        aml_object_t* next = obj_find_child(cur, seg);
        if (!next && !create) {
            if (i == 0 && cur == scope) {
                for (aml_object_t* up = scope->parent; up; up = up->parent) {
                    next = obj_find_child(up, seg);
                    if (next) break;
                }
            }
        }
        if (!next) {
            if (create && i < segs - 1) {
                next = obj_create_child(cur, seg, AML_OBJ_SCOPE);
                if (!next) return NULL;
            } else return NULL;
        }
        cur = next;
    }
    return cur;
}

/* ===========================================================
 * Stream helpers
 * =========================================================== */

static uint32_t decode_pkglen(const uint8_t** p, const uint8_t* end,
                              uint32_t* consumed) {
    if (*p >= end) { *consumed = 0; return 0; }
    uint8_t lead = **p;
    uint8_t follow = (uint8_t)((lead >> 6) & 0x3);
    if (*p + 1 + follow > end) { *consumed = 0; return 0; }
    uint32_t v;
    if (follow == 0) v = lead & 0x3F;
    else {
        v = lead & 0x0F;
        for (uint8_t i = 0; i < follow; i++)
            v |= ((uint32_t)(*p)[1 + i]) << (4 + 8 * i);
    }
    *consumed = 1u + follow;
    return v;
}

static uint64_t read_integer(const uint8_t** p, const uint8_t* end) {
    if (*p >= end) return 0;
    uint8_t op = **p;
    switch (op) {
        case ZeroOp:    (*p)++; return 0;
        case OneOp:     (*p)++; return 1;
        case OnesOp:    (*p)++; return ~0ULL;
        case BytePrefix:
            if (*p + 2 > end) return 0;
            { uint64_t v = (*p)[1]; (*p) += 2; return v; }
        case WordPrefix:
            if (*p + 3 > end) return 0;
            { uint64_t v = (uint64_t)(*p)[1] | ((uint64_t)(*p)[2] << 8);
              (*p) += 3; return v; }
        case DWordPrefix:
            if (*p + 5 > end) return 0;
            { uint64_t v = (uint64_t)(*p)[1]       |
                           ((uint64_t)(*p)[2] << 8)  |
                           ((uint64_t)(*p)[3] << 16) |
                           ((uint64_t)(*p)[4] << 24);
              (*p) += 5; return v; }
        case QWordPrefix:
            if (*p + 9 > end) return 0;
            { uint64_t v = 0;
              for (int i = 0; i < 8; i++)
                  v |= (uint64_t)(*p)[1 + i] << (8 * i);
              (*p) += 9; return v; }
        default:
            return 0;
    }
}

/*
 * read_integer cannot tell "the constant zero" from "this is not a
 * constant at all" — it answers 0 for both and, in the second case,
 * does not advance. Every caller that must not fabricate a value asks
 * through here instead.
 */
static bool read_const_integer(const uint8_t** p, const uint8_t* end,
                                uint64_t* out) {
    if (*p >= end) return false;
    switch (**p) {
        case ZeroOp: case OneOp: case OnesOp:
        case BytePrefix: case WordPrefix:
        case DWordPrefix: case QWordPrefix:
            break;
        default:
            return false;
    }
    const uint8_t* before = *p;
    uint64_t v = read_integer(p, end);
    if (*p == before) return false;     /* truncated encoding */
    *out = v;
    return true;
}

static bool aml_is_lead_name_char(uint8_t c) {
    /* ACPI 6.5 §20.2.2: LeadNameChar := 'A'-'Z' | '_'. A digit may not
     * start a NameSeg, which is what keeps the ASCII-digit sub-opcodes
     * (RevisionOp 0x30 .. TimerOp 0x33) out of the name decoder. */
    return (c >= 'A' && c <= 'Z') || c == '_';
}

static bool aml_is_name_string_lead(uint8_t c) {
    return c == RootChar || c == ParentPrefixChar ||
           c == DualNamePrefix || c == MultiNamePrefix ||
           c == 0x00 /* NullName */ || aml_is_lead_name_char(c);
}

/* Step over a NameString (§20.2.2) without touching the namespace. */
static bool skip_name_string(const uint8_t** p, const uint8_t* end) {
    if (*p >= end) return false;
    if (**p == RootChar) (*p)++;
    while (*p < end && **p == ParentPrefixChar) (*p)++;
    if (*p >= end) return false;
    uint8_t lead = **p;
    uint32_t segs;
    if (lead == 0x00) { (*p)++; return true; }          /* NullName */
    if (lead == DualNamePrefix)       { (*p)++; segs = 2; }
    else if (lead == MultiNamePrefix) {
        (*p)++;
        if (*p >= end) return false;
        segs = **p; (*p)++;
    } else if (aml_is_lead_name_char(lead)) segs = 1;
    else return false;
    if ((size_t)(end - *p) < (size_t)segs * 4u) return false;
    *p += (size_t)segs * 4u;
    return true;
}

/* Step over an op that carries a PkgLength, *p on the PkgLength byte.
 * §20.2.4: the PkgLength counts its own encoding bytes, which is what
 * makes the jump exact. */
static bool skip_pkg_block(const uint8_t** p, const uint8_t* end) {
    uint32_t cbytes;
    uint32_t pklen = decode_pkglen(p, end, &cbytes);
    if (!cbytes || pklen < cbytes) return false;
    const uint8_t* body_end = *p + pklen;
    if (body_end > end) return false;
    *p = body_end;
    return true;
}

#define AML_MAX_SKIP_DEPTH 16

static bool skip_term_arg(const uint8_t** p, const uint8_t* end,
                           aml_object_t* scope, int depth);

/* Target := SuperName | NullName (§20.2.2). */
static bool skip_super_name(const uint8_t** p, const uint8_t* end,
                             aml_object_t* scope, int depth) {
    if (*p >= end) return false;
    uint8_t op = **p;
    if (op == 0x00) { (*p)++; return true; }              /* NullName    */
    if (op >= 0x60 && op <= 0x6E) { (*p)++; return true; }/* Local / Arg */
    if (op == ExtOpPrefix) {
        /* The only SuperName under the prefix is DebugObj. */
        if ((size_t)(end - *p) < 2 || (*p)[1] != ExtDebugOp) return false;
        *p += 2;
        return true;
    }
    if (op == RefOfOp || op == DerefOfOp || op == IndexOp)
        return skip_term_arg(p, end, scope, depth);
    if (!aml_is_name_string_lead(op)) return false;
    return skip_name_string(p, end);
}

/*
 * Step over one TermArg (§20.2.3) whose value this pass does not need.
 * The namespace builder has no executor frame, so an operand it cannot
 * measure is a refusal — never a guess that leaves *p inside the
 * operand bytes for the next TermObj scan to chew on.
 */
static bool skip_term_arg(const uint8_t** p, const uint8_t* end,
                           aml_object_t* scope, int depth) {
    if (depth > AML_MAX_SKIP_DEPTH || *p >= end) return false;
    uint8_t op = **p;

    switch (op) {
        case ZeroOp: case OneOp: case OnesOp:
        case BytePrefix: case WordPrefix:
        case DWordPrefix: case QWordPrefix: {
            const uint8_t* before = *p;
            read_integer(p, end);
            return *p != before;
        }
        case StringPrefix:
            (*p)++;
            while (*p < end && **p) (*p)++;
            if (*p >= end) return false;
            (*p)++;                        /* the NUL */
            return true;
        case BufferOp: case PackageOp: case VarPackageOp:
            (*p)++;
            return skip_pkg_block(p, end);
        default:
            break;
    }

    if (op >= 0x60 && op <= 0x6E) { (*p)++; return true; }   /* Local/Arg */

    if (aml_is_name_string_lead(op) && op != 0x00) {
        /* A NameString in TermArg position is either a data reference or
         * a method invocation; only the namespace can say which, and a
         * method drags its arguments along behind it. */
        const uint8_t* probe = *p;
        aml_object_t* o = resolve_name(&probe, end, scope, false, NULL);
        if (!skip_name_string(p, end)) return false;
        if (o && o->type == AML_OBJ_METHOD) {
            uint8_t argc = (uint8_t)(o->v.method.flags & 0x7);
            for (uint8_t i = 0; i < argc; i++)
                if (!skip_term_arg(p, end, scope, depth + 1)) return false;
        }
        return true;
    }

    if (op == ExtOpPrefix) {
        /* CondRefOf / Acquire / Timer ... in TermArg position. */
        if ((size_t)(end - *p) < 2) return false;
        uint8_t sub = (*p)[1];
        *p += 2;
        switch (sub) {
            case ExtCondRefOfOp:
                return skip_super_name(p, end, scope, depth + 1) &&
                       skip_super_name(p, end, scope, depth + 1);
            case ExtAcquireOp:
                if (!skip_super_name(p, end, scope, depth + 1)) return false;
                if ((size_t)(end - *p) < 2) return false;
                *p += 2;                   /* Timeout := WordData */
                return true;
            case ExtWaitOp:
                return skip_super_name(p, end, scope, depth + 1) &&
                       skip_term_arg(p, end, scope, depth + 1);
            case ExtFromBCDOp: case ExtToBCDOp:
                return skip_term_arg(p, end, scope, depth + 1) &&
                       skip_super_name(p, end, scope, depth + 1);
            case ExtRevisionOp: case ExtDebugOp: case ExtTimerOp:
                return true;               /* no operands */
            default:
                return false;
        }
    }

    /* Expression opcodes: n TermArgs then t Targets (§20.2.5.4). */
    uint8_t nargs, ntargets;
    switch (op) {
        case AddOp: case SubtractOp: case MultiplyOp: case ModOp:
        case AndOp: case NandOp: case OrOp: case NorOp: case XorOp:
        case ShiftLeftOp: case ShiftRightOp: case ConcatOp: case IndexOp:
            nargs = 2; ntargets = 1; break;
        case DivideOp:
            nargs = 2; ntargets = 2; break;
        case NotOp: case StoreOp:
            nargs = 1; ntargets = 1; break;
        case LAndOp: case LOrOp: case LEqualOp:
        case LGreaterOp: case LLessOp:
            nargs = 2; ntargets = 0; break;
        case LNotOp: case DerefOfOp:
            nargs = 1; ntargets = 0; break;
        case RefOfOp: case SizeOfOp: case IncrementOp: case DecrementOp:
            nargs = 0; ntargets = 1; break;
        default:
            return false;
    }
    (*p)++;
    for (uint8_t i = 0; i < nargs; i++)
        if (!skip_term_arg(p, end, scope, depth + 1)) return false;
    for (uint8_t i = 0; i < ntargets; i++)
        if (!skip_super_name(p, end, scope, depth + 1)) return false;
    return true;
}

/*
 * Step over one extended opcode, *p already past ExtOpPrefix and the
 * sub-opcode byte. The operand shapes are from ACPI 6.5 §20.2.5.2-4;
 * the set of sub-opcodes is closed (Table 20.2), so every DEFINED one
 * can be measured exactly. `false` means the byte after the prefix is
 * not a defined opcode at all, or its operands ran off the end.
 */
static bool skip_ext_op(uint8_t sub, const uint8_t** p, const uint8_t* end,
                         aml_object_t* scope) {
    switch (sub) {
        case ExtMutexOp:                  /* NameString SyncFlags */
            if (!skip_name_string(p, end)) return false;
            if (*p >= end) return false;
            (*p)++;
            return true;
        case ExtEventOp:                  /* NameString */
            return skip_name_string(p, end);
        case ExtCondRefOfOp:              /* SuperName Target */
            return skip_super_name(p, end, scope, 0) &&
                   skip_super_name(p, end, scope, 0);
        case ExtCreateFieldOp:            /* TermArg x3 NameString */
            return skip_term_arg(p, end, scope, 0) &&
                   skip_term_arg(p, end, scope, 0) &&
                   skip_term_arg(p, end, scope, 0) &&
                   skip_name_string(p, end);
        case ExtLoadTableOp:              /* TermArg x6 */
            for (int i = 0; i < 6; i++)
                if (!skip_term_arg(p, end, scope, 0)) return false;
            return true;
        case ExtLoadOp:                   /* NameString SuperName */
            return skip_name_string(p, end) &&
                   skip_super_name(p, end, scope, 0);
        case ExtStallOp: case ExtSleepOp: /* TermArg */
            return skip_term_arg(p, end, scope, 0);
        case ExtAcquireOp:                /* SuperName WordData */
            if (!skip_super_name(p, end, scope, 0)) return false;
            if ((size_t)(end - *p) < 2) return false;
            *p += 2;
            return true;
        case ExtSignalOp: case ExtResetOp:
        case ExtReleaseOp: case ExtUnloadOp:      /* SuperName */
            return skip_super_name(p, end, scope, 0);
        case ExtWaitOp:                   /* SuperName TermArg */
            return skip_super_name(p, end, scope, 0) &&
                   skip_term_arg(p, end, scope, 0);
        case ExtFromBCDOp: case ExtToBCDOp:       /* TermArg Target */
            return skip_term_arg(p, end, scope, 0) &&
                   skip_super_name(p, end, scope, 0);
        case ExtRevisionOp: case ExtDebugOp: case ExtTimerOp:
            return true;                  /* no operands */
        case ExtFatalOp:                  /* ByteData DWordData TermArg */
            if ((size_t)(end - *p) < 5) return false;
            *p += 5;
            return skip_term_arg(p, end, scope, 0);
        case ExtOpRegionOp:               /* NameString RegionSpace TermArg x2 */
            if (!skip_name_string(p, end)) return false;
            if (*p >= end) return false;
            (*p)++;
            return skip_term_arg(p, end, scope, 0) &&
                   skip_term_arg(p, end, scope, 0);
        case ExtFieldOp: case ExtDeviceOp: case ExtProcessorOp:
        case ExtPowerResOp: case ExtThermalZoneOp:
        case ExtIndexFieldOp: case ExtBankFieldOp:
            return skip_pkg_block(p, end);        /* PkgLength-bearing */
        case ExtDataRegionOp:             /* NameString TermArg x3 */
            if (!skip_name_string(p, end)) return false;
            for (int i = 0; i < 3; i++)
                if (!skip_term_arg(p, end, scope, 0)) return false;
            return true;
        default:
            return false;
    }
}

/* ===========================================================
 * Parser — populate namespace from a TermList byte range
 * =========================================================== */

static aml_status_t parse_term_list(const uint8_t* aml, uint32_t len,
                                     aml_object_t* scope, int depth);

static aml_status_t handle_buffer(const uint8_t** p, const uint8_t* end,
                                   aml_object_t* obj) {
    (*p)++;     /* BufferOp */
    uint32_t pklen_bytes;
    uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
    if (!pklen_bytes) return AML_ERR_BOUNDS;
    const uint8_t* body_start = *p + pklen_bytes;
    const uint8_t* body_end   = *p + pklen;
    if (body_end > end) return AML_ERR_BOUNDS;

    const uint8_t* bp = body_start;
    uint64_t size = read_integer(&bp, body_end);
    if (size > AML_MAX_BUFFER_BYTES) return AML_ERR_BOUNDS;

    obj->type = AML_OBJ_BUFFER;
    obj->v.buffer.bytes = bp;
    obj->v.buffer.len   = (uint32_t)size;
    *p = body_end;
    return AML_OK;
}

static aml_status_t handle_package(const uint8_t** p, const uint8_t* end,
                                    aml_object_t* obj) {
    (*p)++;     /* PackageOp */
    uint32_t pklen_bytes;
    uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
    if (!pklen_bytes) return AML_ERR_BOUNDS;
    const uint8_t* body_start = *p + pklen_bytes;
    const uint8_t* body_end   = *p + pklen;
    if (body_end > end || body_start >= body_end) return AML_ERR_BOUNDS;

    /* PackageOp NumElements is a single byte — 0..255 fits any value
     * we'd care to track. (VarPackageOp would use a TermArg and need a
     * cap; that path is rare enough we currently parse only fixed-size
     * Packages.) */
    uint8_t num = body_start[0];
    obj->type = AML_OBJ_PACKAGE;
    obj->v.package.count = num;
    /* Capture the first element's integer value if it is one. We keep
     * the storage minimal — most callers only need _S5[0]/_S5[1]. */
    const uint8_t* bp = body_start + 1;
    if (bp < body_end) {
        const uint8_t* before = bp;
        uint64_t v = read_integer(&bp, body_end);
        if (bp != before) obj->v.package.first_int = v;
    }
    *p = body_end;
    return AML_OK;
}

static aml_status_t handle_named_data(const uint8_t** p, const uint8_t* end,
                                       aml_object_t* obj) {
    /* DataRefObject following a NameOp NameString. */
    if (*p >= end) return AML_ERR_BOUNDS;
    uint8_t op = **p;
    if (op == BufferOp)        return handle_buffer(p, end, obj);
    if (op == PackageOp ||
        op == VarPackageOp)    return handle_package(p, end, obj);
    if (op == StringPrefix) {
        (*p)++;
        const char* s = (const char*)*p;
        uint32_t slen = 0;
        while (*p < end && **p) { (*p)++; slen++; }
        if (*p < end) (*p)++;
        obj->type = AML_OBJ_STRING;
        obj->v.string.str = s;
        obj->v.string.len = slen;
        return AML_OK;
    }
    obj->type = AML_OBJ_INTEGER;
    obj->v.integer = read_integer(p, end);
    return AML_OK;
}

static aml_status_t handle_scope_method(const uint8_t** p, const uint8_t* end,
                                         aml_object_t* scope, int depth,
                                         bool is_method) {
    (*p)++;     /* op byte */
    uint32_t pklen_bytes;
    uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
    if (!pklen_bytes) return AML_ERR_BOUNDS;
    const uint8_t* body_start = *p + pklen_bytes;
    const uint8_t* body_end   = *p + pklen;
    if (body_end > end) return AML_ERR_BOUNDS;

    char leaf[4];
    const uint8_t* np = body_start;
    aml_object_t* parent = resolve_name(&np, body_end, scope, true, leaf);
    if (!parent) { *p = body_end; return AML_ERR_BOUNDS; }

    aml_obj_type_t t = is_method ? AML_OBJ_METHOD : AML_OBJ_SCOPE;
    aml_object_t* target = obj_find_child(parent, leaf);
    if (!target) target = obj_create_child(parent, leaf, t);
    if (!target) { *p = body_end; return AML_ERR_NOMEM; }
    target->type = t;

    if (is_method) {
        if (np >= body_end) { *p = body_end; return AML_OK; }
        target->v.method.flags  = *np++;
        target->v.method.aml    = np;
        target->v.method.aml_len = (uint32_t)(body_end - np);
    } else {
        parse_term_list(np, (uint32_t)(body_end - np), target, depth + 1);
    }
    *p = body_end;
    return AML_OK;
}

/* Where the fields of one FieldList get their bits from. Field binds a
 * region; IndexField an Index/Data register pair; BankField a region
 * plus the selector that chooses which bank answers. */
typedef struct AmlFieldSite {
    aml_object_t* region;
    aml_object_t* index;
    aml_object_t* data;
    aml_object_t* bank;
    uint64_t      bank_value;
    uint8_t       flags;        /* FieldFlags byte — §20.2.5.2 */
} AmlFieldSite;

/*
 * FieldList — ACPI 6.5 §20.2.5.2:
 *
 *   FieldElement        := NamedField | ReservedField | AccessField |
 *                          ExtendedAccessField | ConnectField
 *   NamedField          := NameSeg PkgLength
 *   ReservedField       := 0x00 PkgLength
 *   AccessField         := 0x01 AccessType AccessAttrib
 *   ExtendedAccessField := 0x03 AccessType ExtendedAccessAttrib AccessLength
 *   ConnectField        := <0x02 NameString> | <0x02 BufferData>
 *
 * The last three carry no name and contribute no bits: they change the
 * access rules for the elements that follow. Eating one of them as the
 * first byte of a NameSeg puts every later field at the wrong bit
 * offset — and a field at the wrong offset is a store to the wrong
 * register. Byte counts match ACPICA's AcpiPsGetNextField.
 */
static void parse_field_list(const uint8_t* np, const uint8_t* body_end,
                              aml_object_t* scope, const AmlFieldSite* site) {
    uint32_t bit_cursor = 0;
    uint8_t  access     = site->flags;

    while (np < body_end) {
        uint8_t tag = *np;

        if (tag == 0x00) {                       /* ReservedField */
            np++;
            uint32_t cbytes;
            uint32_t bits = decode_pkglen(&np, body_end, &cbytes);
            if (!cbytes) break;
            np += cbytes;
            bit_cursor += bits;
            continue;
        }
        if (tag == 0x01) {                       /* AccessField */
            if (np + 3 > body_end) break;
            access = np[1];                      /* AccessType */
            np += 3;
            continue;
        }
        if (tag == 0x03) {                       /* ExtendedAccessField */
            if (np + 4 > body_end) break;
            access = np[1];
            np += 4;
            continue;
        }
        if (tag == 0x02) {                       /* ConnectField */
            np++;
            if (np >= body_end) break;
            if (*np == BufferOp) {
                np++;
                if (!skip_pkg_block(&np, body_end)) break;
            } else if (!skip_name_string(&np, body_end)) {
                break;
            }
            continue;
        }
        if (!aml_is_lead_name_char(tag)) {
            kprintf("[AML] FieldList in %s: 0x%02x is not a field element — "
                    "rest of the block dropped\n", scope->name, tag);
            break;
        }

        if (np + 4 > body_end) break;
        char seg[4] = { (char)np[0], (char)np[1], (char)np[2], (char)np[3] };
        np += 4;
        uint32_t cbytes;
        uint32_t bits = decode_pkglen(&np, body_end, &cbytes);
        if (!cbytes) break;
        np += cbytes;

        aml_object_t* f = obj_create_child(scope, seg, AML_OBJ_FIELD);
        if (!f) break;
        f->v.field.region     = site->region;
        f->v.field.index      = site->index;
        f->v.field.data       = site->data;
        f->v.field.bank       = site->bank;
        f->v.field.bank_value = site->bank_value;
        f->v.field.bit_offset = bit_cursor;
        f->v.field.bit_width  = bits;
        f->v.field.access     = access;
        bit_cursor += bits;
    }
}

static aml_status_t handle_extended(const uint8_t** p, const uint8_t* end,
                                     aml_object_t* scope, int depth) {
    (*p) += 2;     /* ExtOp + opcode */
    /* Sub-opcode is at (*p - 1). */
    uint8_t sub = (*p)[-1];

    if (sub == ExtOpRegionOp) {
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent || *p >= end) return AML_ERR_BOUNDS;
        uint8_t space = **p; (*p)++;
        /* DefOpRegion := OpRegionOp NameString RegionSpace RegionOffset
         * RegionLen, and §20.2.5.2 makes both RegionOffset and RegionLen
         * TermArgs — a NameSeg naming a Field is as legal there as a
         * literal, and AMD's shipping AGESA FCH ASL uses exactly that at
         * namespace level. This pass has no executor frame and no field
         * to read from yet, so the non-constant form is stepped over
         * exactly and the region is marked unusable. Registering it at
         * address 0 would aim its fields at the legacy I/O ports or at
         * physical page 0. */
        uint64_t base = 0, len = 0;
        bool have_base = read_const_integer(p, end, &base);
        bool measured  = have_base || skip_term_arg(p, end, scope, 0);
        bool have_len  = measured && read_const_integer(p, end, &len);
        if (measured && !have_len) measured = skip_term_arg(p, end, scope, 0);

        aml_object_t* o = obj_create_child(parent, leaf, AML_OBJ_REGION);
        if (!o) return AML_ERR_NOMEM;
        o->v.region.space = space;
        o->v.region.base  = base;
        o->v.region.len   = len;
        o->v.region.usable = have_base && have_len && len != 0;
        if (!o->v.region.usable)
            kprintf("[AML] OperationRegion %s: %s — every access refused\n",
                    o->name,
                    (!have_base || !have_len)
                        ? "offset/length is not a constant this pass can evaluate"
                        : "declared length is zero");
        if (!measured) {
            kprintf("[AML] OperationRegion %s: its operand bytes cannot be "
                    "measured by this pass — TermList resyncing\n", o->name);
            return AML_ERR_BAD_OPCODE;
        }
        return AML_OK;
    }

    if (sub == ExtDeviceOp || sub == ExtProcessorOp ||
        sub == ExtPowerResOp || sub == ExtThermalZoneOp) {
        uint32_t pklen_bytes;
        uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
        if (!pklen_bytes) return AML_ERR_BOUNDS;
        const uint8_t* body_start = *p + pklen_bytes;
        const uint8_t* body_end   = *p + pklen;
        if (body_end > end) return AML_ERR_BOUNDS;

        char leaf[4];
        const uint8_t* np = body_start;
        aml_object_t* parent = resolve_name(&np, body_end, scope, true, leaf);
        if (!parent) { *p = body_end; return AML_ERR_BOUNDS; }
        aml_obj_type_t t = (sub == ExtDeviceOp)     ? AML_OBJ_DEVICE :
                            (sub == ExtProcessorOp) ? AML_OBJ_PROCESSOR :
                            (sub == ExtPowerResOp)  ? AML_OBJ_POWER_RES :
                                                      AML_OBJ_THERMAL;
        aml_object_t* sub_obj = obj_find_child(parent, leaf);
        if (!sub_obj) sub_obj = obj_create_child(parent, leaf, t);
        if (!sub_obj) { *p = body_end; return AML_ERR_NOMEM; }
        sub_obj->type = t;
        if (sub == ExtProcessorOp) { if (np + 6 <= body_end) np += 6; }
        if (sub == ExtPowerResOp)  { if (np + 3 <= body_end) np += 3; }
        parse_term_list(np, (uint32_t)(body_end - np), sub_obj, depth + 1);
        *p = body_end;
        return AML_OK;
    }

    if (sub == ExtFieldOp || sub == ExtIndexFieldOp || sub == ExtBankFieldOp) {
        /* DefField      := FieldOp      PkgLength NameString FieldFlags FieldList
         * DefIndexField := IndexFieldOp PkgLength NameString NameString
         *                                         FieldFlags FieldList
         * DefBankField  := BankFieldOp  PkgLength NameString NameString
         *                               BankValue FieldFlags FieldList  */
        uint32_t pklen_bytes;
        uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
        if (!pklen_bytes) return AML_ERR_BOUNDS;
        const uint8_t* body_end = *p + pklen;
        const uint8_t* np = *p + pklen_bytes;
        if (body_end > end) return AML_ERR_BOUNDS;

        AmlFieldSite site = { NULL, NULL, NULL, NULL, 0, 0 };
        aml_object_t* first = resolve_name(&np, body_end, scope, false, NULL);
        if (!first) { *p = body_end; return AML_OK; }

        if (sub == ExtFieldOp) {
            site.region = first;
        } else {
            aml_object_t* second = resolve_name(&np, body_end, scope,
                                                false, NULL);
            if (!second) { *p = body_end; return AML_OK; }
            if (sub == ExtIndexFieldOp) {
                /* Index and Data must be plain region-backed fields: an
                 * Index register that is itself indexed would make the
                 * access protocol recurse without a bottom. */
                if (first->type != AML_OBJ_FIELD ||
                    second->type != AML_OBJ_FIELD ||
                    !first->v.field.region || !second->v.field.region) {
                    kprintf("[AML] IndexField: %s/%s are not both region "
                            "Fields — block dropped\n",
                            first->name, second->name);
                    *p = body_end;
                    return AML_OK;
                }
                site.index = first;
                site.data  = second;
            } else {
                /* Same bottom for the selector: it must be a plain
                 * region field, not one that selects a bank itself. */
                if (second->type != AML_OBJ_FIELD ||
                    !second->v.field.region || second->v.field.bank) {
                    kprintf("[AML] BankField: %s is not a plain region "
                            "Field — block dropped\n", second->name);
                    *p = body_end;
                    return AML_OK;
                }
                site.region = first;
                site.bank   = second;
                if (!read_const_integer(&np, body_end, &site.bank_value)) {
                    kprintf("[AML] BankField %s: BankValue is not a constant "
                            "this pass can evaluate — block dropped\n",
                            first->name);
                    *p = body_end;
                    return AML_OK;
                }
            }
        }

        if (np >= body_end) { *p = body_end; return AML_OK; }
        site.flags = *np++;
        parse_field_list(np, body_end, scope, &site);
        *p = body_end;
        return AML_OK;
    }

    if (sub == ExtMutexOp || sub == ExtEventOp) {
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent) return AML_ERR_BOUNDS;
        obj_create_child(parent, leaf,
            sub == ExtMutexOp ? AML_OBJ_MUTEX : AML_OBJ_EVENT);
        if (sub == ExtMutexOp && *p < end) (*p)++;
        return AML_OK;
    }

    /* Everything else under the prefix: step over it EXACTLY. 0x5B is a
     * prefix, so the stream may never be re-entered at the sub-opcode
     * byte — parse_term_list's one-byte resync would walk the operands
     * as if each were a TermObj, and a stray 0x08/0x10/0x14 among them
     * fabricates namespace objects that can swallow real declarations. */
    if (skip_ext_op(sub, p, end, scope)) return AML_OK;
    kprintf("[AML] extended opcode 0x5B 0x%02x carries operands this pass "
            "cannot measure — resyncing\n", sub);
    return AML_ERR_BAD_OPCODE;
}

static aml_status_t parse_one(const uint8_t** p, const uint8_t* end,
                               aml_object_t* scope, int depth) {
    if (*p >= end) return AML_ERR_BOUNDS;
    uint8_t op = **p;

    if (op == NameOp) {
        (*p)++;
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent) return AML_ERR_BOUNDS;
        aml_object_t* o = obj_create_child(parent, leaf, AML_OBJ_NAME);
        if (!o) return AML_ERR_NOMEM;
        return handle_named_data(p, end, o);
    }
    if (op == ScopeOp)   return handle_scope_method(p, end, scope, depth, false);
    if (op == MethodOp)  return handle_scope_method(p, end, scope, depth, true);
    if (op == AliasOp) {
        (*p)++;
        aml_object_t* src = resolve_name(p, end, scope, false, NULL);
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent) return AML_ERR_BOUNDS;
        aml_object_t* a = obj_create_child(parent, leaf, AML_OBJ_ALIAS);
        if (a) a->v.alias.target = src;
        return AML_OK;
    }
    if (op == ExtOpPrefix) return handle_extended(p, end, scope, depth);

    return AML_ERR_BAD_OPCODE;
}

static aml_status_t parse_term_list(const uint8_t* aml, uint32_t len,
                                     aml_object_t* scope, int depth) {
    if (depth > AML_MAX_SCOPE_DEPTH) return AML_ERR_DEEP_NEST;
    const uint8_t* p   = aml;
    const uint8_t* end = aml + len;
    while (p < end) {
        const uint8_t* before = p;
        aml_status_t s = parse_one(&p, end, scope, depth);
        if (s == AML_ERR_DEEP_NEST || s == AML_ERR_NOMEM) return s;
        if (p == before || s == AML_ERR_BAD_OPCODE) p = before + 1;
    }
    return AML_OK;
}

/* ===========================================================
 * SSDT iteration callback (must be module-level for parse_term_list)
 * =========================================================== */
static bool ssdt_cb(acpi_sdt_header_t* h, void* u) {
    (void)u;
    uint32_t hdr = (uint32_t)sizeof(acpi_sdt_header_t);
    if (h->length <= hdr) return true;
    parse_term_list((uint8_t*)h + hdr, h->length - hdr, &g_root_obj, 0);
    return true;
}

/* ===========================================================
 * OpRegion access
 * =========================================================== */

/*
 * A region answers only for the address space it was actually given.
 * ACPICA's AcpiExSetupRegion refuses an access whose byte offset plus
 * access width runs past the declared length; without that check a
 * field left at a wrong bit offset — or a region whose address was a
 * TermArg this build never evaluated — reaches straight into the
 * machine: ports 0x00-0x7F, or physical page 0.
 */
static bool region_access_ok(aml_object_t* region, uint64_t offset,
                              uint8_t bytes) {
    if (!region || region->type != AML_OBJ_REGION) return false;
    if (!region->v.region.usable) return false;   /* named at parse time */
    if (offset + bytes > region->v.region.len) {
        if (!region->v.region.refused) {
            region->v.region.refused = true;
            kprintf("[AML] region %s: access at +%lu for %u bytes runs past "
                    "its %lu bytes — refused\n", region->name,
                    (unsigned long)offset, (unsigned)bytes,
                    (unsigned long)region->v.region.len);
        }
        return false;
    }
    return true;
}

uint64_t aml_region_read(aml_object_t* region, uint64_t offset, uint8_t bits) {
    uint8_t bytes = (uint8_t)((bits + 7) / 8);
    if (!region_access_ok(region, offset, bytes)) return 0;
    uint64_t addr = region->v.region.base + offset;
    switch (region->v.region.space) {
        case REGION_SYSTEM_IO:
            if (bytes == 1) return inb((uint16_t)addr);
            if (bytes == 2) return inw((uint16_t)addr);
            if (bytes == 4) return inl((uint16_t)addr);
            return 0;
        case REGION_SYSTEM_MEMORY: {
            /* O(1) hot path: first access maps the whole region UC and
             * caches the VA; subsequent reads dereference directly with
             * zero VMM calls. */
            if (!region->v.region.mapped_virt) {
                region->v.region.mapped_virt = (volatile uint8_t*)
                    vmm_map_mmio((uintptr_t)region->v.region.base,
                                  region->v.region.len ? region->v.region.len
                                                       : 4096,
                                  VMM_FLAGS_KERNEL_RW);
                if (!region->v.region.mapped_virt) return 0;
            }
            volatile uint8_t* p = region->v.region.mapped_virt + offset;
            if (bytes == 1) return *(volatile uint8_t*)p;
            if (bytes == 2) return *(volatile uint16_t*)p;
            if (bytes == 4) return *(volatile uint32_t*)p;
            if (bytes == 8) return *(volatile uint64_t*)p;
            return 0;
        }
        case REGION_PCI_CONFIG: {
            uint16_t seg = (uint16_t)((region->v.region.base >> 48) & 0xFFFFu);
            uint8_t  bus = (uint8_t)((region->v.region.base >> 24) & 0xFFu);
            uint8_t  dev = (uint8_t)((region->v.region.base >> 19) & 0x1Fu);
            uint8_t  fn  = (uint8_t)((region->v.region.base >> 16) & 0x07u);
            uint16_t off = (uint16_t)offset;
            if (pci_has_ecam()) {
                if (bytes == 1) return pci_ecam_read_byte(seg, bus, dev, fn, off);
                if (bytes == 2) return pci_ecam_read_word(seg, bus, dev, fn, off);
                if (bytes == 4) return pci_ecam_read_dword(seg, bus, dev, fn, off);
            } else if (seg == 0 && off <= 0xFC) {
                if (bytes == 1) return pci_config_read_byte(bus, dev, fn, (uint8_t)off);
                if (bytes == 2) return pci_config_read_word(bus, dev, fn, (uint8_t)off);
                if (bytes == 4) return pci_config_read_dword(bus, dev, fn, (uint8_t)off);
            }
            return 0;
        }
        default: return 0;
    }
}

void aml_region_write(aml_object_t* region, uint64_t offset, uint8_t bits,
                       uint64_t value) {
    uint8_t bytes = (uint8_t)((bits + 7) / 8);
    if (!region_access_ok(region, offset, bytes)) return;
    uint64_t addr = region->v.region.base + offset;
    switch (region->v.region.space) {
        case REGION_SYSTEM_IO:
            if (bytes == 1) outb((uint16_t)addr, (uint8_t)value);
            if (bytes == 2) outw((uint16_t)addr, (uint16_t)value);
            if (bytes == 4) outl((uint16_t)addr, (uint32_t)value);
            return;
        case REGION_SYSTEM_MEMORY: {
            if (!region->v.region.mapped_virt) {
                region->v.region.mapped_virt = (volatile uint8_t*)
                    vmm_map_mmio((uintptr_t)region->v.region.base,
                                  region->v.region.len ? region->v.region.len
                                                       : 4096,
                                  VMM_FLAGS_KERNEL_RW);
                if (!region->v.region.mapped_virt) return;
            }
            volatile uint8_t* p = region->v.region.mapped_virt + offset;
            if (bytes == 1) *(volatile uint8_t*)p  = (uint8_t)value;
            if (bytes == 2) *(volatile uint16_t*)p = (uint16_t)value;
            if (bytes == 4) *(volatile uint32_t*)p = (uint32_t)value;
            if (bytes == 8) *(volatile uint64_t*)p = value;
            return;
        }
        case REGION_PCI_CONFIG: {
            uint16_t seg = (uint16_t)((region->v.region.base >> 48) & 0xFFFFu);
            uint8_t  bus = (uint8_t)((region->v.region.base >> 24) & 0xFFu);
            uint8_t  dev = (uint8_t)((region->v.region.base >> 19) & 0x1Fu);
            uint8_t  fn  = (uint8_t)((region->v.region.base >> 16) & 0x07u);
            uint16_t off = (uint16_t)offset;
            if (pci_has_ecam()) {
                if (bytes == 1) pci_ecam_write_byte(seg, bus, dev, fn, off, (uint8_t)value);
                if (bytes == 2) pci_ecam_write_word(seg, bus, dev, fn, off, (uint16_t)value);
                if (bytes == 4) pci_ecam_write_dword(seg, bus, dev, fn, off, (uint32_t)value);
            } else if (seg == 0 && off <= 0xFC) {
                if (bytes == 1) pci_config_write_byte(bus, dev, fn, (uint8_t)off, (uint8_t)value);
                if (bytes == 2) pci_config_write_word(bus, dev, fn, (uint8_t)off, (uint16_t)value);
                if (bytes == 4) pci_config_write_dword(bus, dev, fn, (uint8_t)off, (uint32_t)value);
            }
            return;
        }
    }
}

/* ===========================================================
 * Public API
 * =========================================================== */

aml_status_t aml_init(void) {
    if (g_loaded) return AML_OK;
    memset(&g_root_obj, 0, sizeof(g_root_obj));
    g_root_obj.type = AML_OBJ_SCOPE;
    g_root_obj.name[0] = '\\';
    g_pool_used = 0;

    if (!g_acpi.initialized || !g_acpi.fadt) return AML_ERR_NOT_LOADED;
    uint64_t dsdt_addr =
        (g_acpi.fadt->header.length >= offsetof(acpi_fadt_t, x_dsdt) + 8 &&
         g_acpi.fadt->x_dsdt)
            ? g_acpi.fadt->x_dsdt
            : (uint64_t)g_acpi.fadt->dsdt;
    if (!dsdt_addr) return AML_ERR_NOT_LOADED;

    acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_map_physical(
        (uintptr_t)dsdt_addr, sizeof(acpi_sdt_header_t));
    if (!hdr) return AML_ERR_NOT_LOADED;
    acpi_sdt_header_t* dsdt = (acpi_sdt_header_t*)acpi_map_physical(
        (uintptr_t)dsdt_addr, hdr->length);
    if (!dsdt) return AML_ERR_NOT_LOADED;

    parse_term_list((uint8_t*)dsdt + sizeof(acpi_sdt_header_t),
                    dsdt->length - sizeof(acpi_sdt_header_t),
                    &g_root_obj, 0);

    /* Every SSDT contributes additional namespace. */
    acpi_for_each_table("SSDT", ssdt_cb, NULL);

    g_loaded = true;
    debug_printf("[AML] namespace loaded: %u objects across DSDT + SSDTs\n",
                 g_pool_used);
    /* Daemons that need the AML namespace (power manager, EC subscriber,
     * thermal monitor) wait on this tag. The payload is the object
     * count so subscribers can size their caches. */
    uint32_t cnt = g_pool_used;
    TouchPublish("aml:ready", &cnt, sizeof(cnt));
    return AML_OK;
}

/* Convert a C path like "\_SB.PCI0._INI" to an AML NameString byte
 * stream so we can reuse resolve_name(). */
aml_status_t aml_find(const char* path, aml_object_t** out) {
    if (!g_loaded || !path) return AML_ERR_NOT_FOUND;
    uint8_t buf[128];
    uint32_t bi = 0;
    const char* s = path;
    if (*s == '\\') { buf[bi++] = RootChar; s++; }
    while (*s == '^' && bi < sizeof(buf)) { buf[bi++] = ParentPrefixChar; s++; }

    /* Count segments. */
    int segs = 0;
    if (*s) segs = 1;
    for (const char* q = s; *q; q++) if (*q == '.') segs++;
    if (segs == 0) return AML_ERR_NOT_FOUND;
    if (segs == 2) buf[bi++] = DualNamePrefix;
    else if (segs > 2) {
        buf[bi++] = MultiNamePrefix;
        buf[bi++] = (uint8_t)segs;
    }

    while (*s && bi + 4 <= sizeof(buf)) {
        for (int i = 0; i < 4; i++) {
            if (*s && *s != '.') buf[bi++] = (uint8_t)*s++;
            else                  buf[bi++] = '_';
        }
        while (*s && *s != '.') s++;
        if (*s == '.') s++;
    }

    const uint8_t* p = buf;
    aml_object_t* o = resolve_name(&p, buf + bi, &g_root_obj, false, NULL);
    if (!o) return AML_ERR_NOT_FOUND;
    if (out) *out = o;
    return AML_OK;
}

aml_status_t aml_read_integer(const char* path, uint64_t* out) {
    aml_object_t* o = NULL;
    aml_status_t s = aml_find(path, &o);
    if (s != AML_OK) return s;
    if (o->type == AML_OBJ_INTEGER) {
        if (out) *out = o->v.integer;
        return AML_OK;
    }
    if (o->type == AML_OBJ_PACKAGE) {
        if (out) *out = o->v.package.first_int;
        return AML_OK;
    }
    return AML_ERR_TYPE;
}

aml_status_t aml_call_pic(uint32_t mode) {
    if (!g_loaded) return AML_ERR_NOT_LOADED;
    uint64_t arg = mode;
    uint64_t ret = 0;
    aml_status_t s = aml_call_int("\\_PIC", &arg, 1, &ret);
    if (s == AML_OK) {
        debug_printf("[AML] \\_PIC(%u) -> %lu\n", mode, (unsigned long)ret);
    }
    return s;
}

aml_status_t aml_eval(const char* path, aml_object_t** args, int argc,
                       aml_object_t** ret) {
    (void)args; (void)argc;
    aml_object_t* o = NULL;
    aml_status_t s = aml_find(path, &o);
    if (s != AML_OK) return s;
    if (o->type == AML_OBJ_INTEGER || o->type == AML_OBJ_STRING ||
        o->type == AML_OBJ_BUFFER  || o->type == AML_OBJ_PACKAGE) {
        if (ret) *ret = o;
        return AML_OK;
    }
    return AML_ERR_NOT_LOADED;
}

aml_status_t aml_eval_crs(const char* path, void* buf, size_t buf_len,
                           size_t* used) {
    (void)path; (void)buf; (void)buf_len;
    if (used) *used = 0;
    return AML_ERR_NOT_LOADED;
}

/* =====================================================================
 * AML method execution engine.
 *
 * Reduced to integer-valued semantics, sufficient for the methods every
 * production firmware drives during OS init: _STA, _INI, _PTS, _BFS,
 * _WAK, _PIC, _OSI, _SUN, _ADR, _CID, _UID. The executor:
 *   - walks the method's TermList byte stream
 *   - keeps Locals[8] and Args[7] as 64-bit integers
 *   - dispatches arithmetic/logical/control/store/method-invoke
 *   - reads/writes Field operands through aml_region_read/write
 *
 * Non-integer result types from a method get reported as 0 — callers
 * that need the full object type system are deferred to a future
 * ACPICA/uACPI-grade rewrite.
 * ===================================================================== */

#define EXEC_MAX_DEPTH    16
#define EXEC_MAX_WHILE    (1u << 22)

typedef struct exec_frame {
    aml_object_t*  scope;             /* current namespace scope */
    uint64_t       locals[8];
    uint64_t       args[7];
    uint8_t        argc;
    uint64_t       ret_value;
    bool           returned;
    bool           break_flag;
    bool           continue_flag;
} exec_frame_t;

uint64_t aml_region_read(aml_object_t* r, uint64_t off, uint8_t bits);
void     aml_region_write(aml_object_t* r, uint64_t off, uint8_t bits,
                            uint64_t v);

static aml_status_t exec_term(const uint8_t** p, const uint8_t* end,
                               exec_frame_t* fr, uint64_t* out, int depth);

static aml_status_t exec_termlist(const uint8_t* aml, uint32_t len,
                                   exec_frame_t* fr, int depth);

/* Evaluate a single TermArg, returning its integer reduction. Advances
 * the stream past the consumed bytes. */
static aml_status_t exec_termarg(const uint8_t** p, const uint8_t* end,
                                  exec_frame_t* fr, uint64_t* out, int depth) {
    return exec_term(p, end, fr, out, depth);
}

static uint64_t field_read(aml_object_t* f);
static void     field_write(aml_object_t* f, uint64_t v);

/* An IndexField moves its data through the Data register one datum at a
 * time, and the datum is exactly as wide as that register was declared.
 * A Data register of some other width is not something to guess at. */
static uint32_t index_field_datum_bits(aml_object_t* f) {
    uint32_t w = f->v.field.data ? f->v.field.data->v.field.bit_width : 0;
    return (w == 8 || w == 16 || w == 32 || w == 64) ? w : 0;
}

/* UpdateRule — FieldFlags bits 5:6 (§20.2.5.2): 0 Preserve, 1
 * WriteAsOnes, 2 WriteAsZeros. It decides what a partial write does to
 * the bits of the datum the field does not own. */
static uint64_t field_update_base(uint8_t flags, uint64_t preserved) {
    switch ((flags >> 5) & 0x3) {
        case 1:  return ~0ULL;
        case 2:  return 0ULL;
        default: return preserved;
    }
}

/*
 * IndexField access — ACPI 6.5 §20.2.5.2, and ACPICA's
 * AcpiExFieldDatumIo for ACPI_TYPE_LOCAL_INDEX_FIELD: the byte offset
 * of the datum goes into the Index register, then the datum itself
 * moves through the Data register. The Index register keeps its value
 * across the read of Data, so one write of Index serves a
 * read-modify-write of the same datum.
 */
static uint64_t index_field_gather(aml_object_t* f, uint32_t bits) {
    uint32_t dbits = index_field_datum_bits(f);
    if (!dbits) return 0;
    uint32_t dbytes  = dbits / 8;
    uint32_t bit_off = f->v.field.bit_offset;
    uint64_t v   = 0;
    uint32_t got = 0;
    while (got < bits) {
        uint32_t abs   = bit_off + got;
        uint64_t doff  = (uint64_t)(abs / dbits) * dbytes;
        uint32_t shift = abs % dbits;
        uint32_t take  = dbits - shift;
        if (take > bits - got) take = bits - got;
        uint64_t mask = (take >= 64) ? ~0ULL : ((1ULL << take) - 1);

        field_write(f->v.field.index, doff);
        uint64_t d = field_read(f->v.field.data);
        v |= ((d >> shift) & mask) << got;
        got += take;
    }
    return v;
}

static void index_field_scatter(aml_object_t* f, uint32_t bits, uint64_t v) {
    uint32_t dbits = index_field_datum_bits(f);
    if (!dbits) return;
    uint32_t dbytes  = dbits / 8;
    uint32_t bit_off = f->v.field.bit_offset;
    uint32_t done = 0;
    while (done < bits) {
        uint32_t abs   = bit_off + done;
        uint64_t doff  = (uint64_t)(abs / dbits) * dbytes;
        uint32_t shift = abs % dbits;
        uint32_t take  = dbits - shift;
        if (take > bits - done) take = bits - done;
        uint64_t mask  = (take >= 64) ? ~0ULL : ((1ULL << take) - 1);
        uint64_t piece = (v >> done) & mask;

        field_write(f->v.field.index, doff);
        uint64_t datum;
        if (take == dbits) {
            datum = piece;
        } else {
            datum = field_update_base(f->v.field.access,
                                      field_read(f->v.field.data));
            datum = (datum & ~(mask << shift)) | (piece << shift);
        }
        field_write(f->v.field.data, datum);
        done += take;
    }
}

/* BankField — §20.2.5.2: the selector chooses which bank answers at the
 * region's addresses, so it must carry this block's BankValue before
 * the data is touched. ACPICA writes it ahead of every datum access. */
static void field_select_bank(aml_object_t* f) {
    if (f->v.field.bank)
        field_write(f->v.field.bank, f->v.field.bank_value);
}

/* Read one Field, with proper bit-level alignment, via OpRegion. */
static uint64_t field_read(aml_object_t* f) {
    if (!f || f->type != AML_OBJ_FIELD) return 0;
    uint32_t bits = f->v.field.bit_width;
    if (bits == 0) return 0;
    if (bits > 64) bits = 64;
    if (f->v.field.index) return index_field_gather(f, bits);
    if (!f->v.field.region) return 0;
    field_select_bank(f);

    uint32_t bit_off = f->v.field.bit_offset;
    /* For aligned byte/word/dword/qword we can read in one shot. */
    if ((bit_off & 7) == 0 && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
        return aml_region_read(f->v.field.region, bit_off / 8, (uint8_t)bits);
    }
    /* Slow path: gather bits. */
    uint64_t v = 0;
    for (uint32_t i = 0; i < bits; i++) {
        uint64_t byte_off = (bit_off + i) / 8;
        uint8_t  shift    = (bit_off + i) % 8;
        uint64_t b = aml_region_read(f->v.field.region, byte_off, 8);
        if ((b >> shift) & 1) v |= (1ULL << i);
    }
    return v;
}

/* The unaligned path over a region preserves the bits around the field,
 * i.e. it implements the Preserve update rule; WriteAsOnes and
 * WriteAsZeros over a region are not honoured here. */
static void field_write(aml_object_t* f, uint64_t v) {
    if (!f || f->type != AML_OBJ_FIELD) return;
    uint32_t bits = f->v.field.bit_width;
    if (bits == 0) return;
    if (bits > 64) bits = 64;
    if (f->v.field.index) { index_field_scatter(f, bits, v); return; }
    if (!f->v.field.region) return;
    field_select_bank(f);

    uint32_t bit_off = f->v.field.bit_offset;
    if ((bit_off & 7) == 0 && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
        aml_region_write(f->v.field.region, bit_off / 8, (uint8_t)bits, v);
        return;
    }
    for (uint32_t i = 0; i < bits; i++) {
        uint64_t byte_off = (bit_off + i) / 8;
        uint8_t  shift    = (bit_off + i) % 8;
        uint64_t cur = aml_region_read(f->v.field.region, byte_off, 8);
        if ((v >> i) & 1) cur |= (1ULL << shift);
        else              cur &= ~(1ULL << shift);
        aml_region_write(f->v.field.region, byte_off, 8, cur);
    }
}

/* resolve_target kinds. 0/1/2/3 are load-bearing at every call site;
 * the last two are the two ways a Target can carry no writable slot. */
#define AML_TGT_NULL   0   /* NullName — the value is discarded          */
#define AML_TGT_LOCAL  1
#define AML_TGT_ARG    2
#define AML_TGT_NAME   3
#define AML_TGT_DEBUG  4   /* DebugObj — the value goes to the log only  */
#define AML_TGT_ERROR  5   /* not a Target at all: the stream is lost    */

/* Resolve a SuperName / SimpleName operand into an aml_object_t* +
 * "kind" so Store can write back. We return NULL for things we can't
 * write to and fall back to discarding the value. */
static aml_object_t* resolve_target(const uint8_t** p, const uint8_t* end,
                                     exec_frame_t* fr, uint8_t* kind_out) {
    if (*p >= end) return NULL;
    uint8_t op = **p;

    /* SuperName := SimpleName | DebugObj | ReferenceTypeOpcode, and the
     * only one of those under ExtOpPrefix is DebugObj (§20.2.2). Left to
     * the NamePath arm below, `Store (x, Debug)` = 70 <arg> 5B 31 would
     * have 0x5B 0x31 and two bytes of the NEXT statement read as a
     * NameSeg — and that spelling is everywhere in production DSDTs. */
    if (op == ExtOpPrefix) {
        if ((size_t)(end - *p) >= 2 && (*p)[1] == ExtDebugOp) {
            *p += 2;
            if (kind_out) *kind_out = AML_TGT_DEBUG;
            return NULL;
        }
        if (kind_out) *kind_out = AML_TGT_ERROR;
        return NULL;
    }
    if (op == 0x60 || op == 0x61 || op == 0x62 || op == 0x63 ||
        op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67) {
        /* Local0..Local7 */
        if (kind_out) *kind_out = 1;
        (*p)++;
        /* Stash local index in dummy object. */
        static aml_object_t locals_slots[8];
        locals_slots[op - 0x60].type = AML_OBJ_INTEGER;
        locals_slots[op - 0x60].v.integer = fr->locals[op - 0x60];
        return &locals_slots[op - 0x60];
    }
    if (op >= 0x68 && op <= 0x6E) {
        if (kind_out) *kind_out = 2;
        (*p)++;
        static aml_object_t args_slots[7];
        args_slots[op - 0x68].type = AML_OBJ_INTEGER;
        args_slots[op - 0x68].v.integer = fr->args[op - 0x68];
        return &args_slots[op - 0x68];
    }
    if (op == 0x00 /* ZeroOp -> nil target */) {
        (*p)++;
        if (kind_out) *kind_out = 0;
        return NULL;
    }
    /* NamePath. */
    if (kind_out) *kind_out = 3;
    return resolve_name(p, end, fr->scope, false, NULL);
}

/*
 * Store a value back through a previously-resolved target.
 *
 * For Local/Arg the executor handles the write inline at the call
 * site (it knows the opcode index, which we don't have here), so kind
 * 1/2 are never passed in. This helper covers the NamePath case —
 * Name objects, raw Integer slots, and OpRegion-backed Fields.
 */
static void exec_store(uint64_t value, aml_object_t* target,
                        exec_frame_t* fr, uint8_t kind) {
    (void)fr;
    if (!target || kind == 0) return;
    if (target->type == AML_OBJ_NAME)    { target->v.integer = value; return; }
    if (target->type == AML_OBJ_INTEGER) { target->v.integer = value; return; }
    if (target->type == AML_OBJ_FIELD)   { field_write(target, value); return; }
}

/*
 * Burn `us` microseconds of real time for Stall and Sleep. BoxOS bounds
 * every wait with HourGlass, which turns against the best ruler this
 * machine has proven. When it cannot turn there is no clock to honour
 * the firmware's delay with, and an invented spin count is not a
 * substitute for one — so the method stops and says so.
 */
static aml_status_t exec_delay_us(uint64_t us) {
    if (us == 0) return AML_OK;
    HourGlass g;
    if (!HourGlassTurn(&g, us)) {
        kprintf("[AML] Sleep/Stall(%lu us): no time ruler on this machine — "
                "the firmware's delay cannot be honoured; method aborted\n",
                (unsigned long)us);
        return AML_ERR_ABORT;
    }
    while (!HourGlassRunOut(&g)) cpu_pause();
    return AML_OK;
}

/* FromBCD / ToBCD — ACPI 6.5 §19.6.45, §19.6.141. Sixteen BCD digits is
 * the whole of a 64-bit integer's BCD range; anything that does not fit
 * is an overflow, which ACPICA answers with AE_AML_NUMERIC_OVERFLOW. */
static bool bcd_to_integer(uint64_t bcd, uint64_t* out) {
    uint64_t v = 0, scale = 1;
    for (int i = 0; i < 16; i++) {
        uint64_t digit = (bcd >> (i * 4)) & 0xF;
        if (digit > 9) return false;
        v += digit * scale;
        if (i < 15) scale *= 10;
    }
    *out = v;
    return true;
}

static bool integer_to_bcd(uint64_t v, uint64_t* out) {
    uint64_t bcd = 0;
    for (int i = 0; i < 16; i++) {
        bcd |= (v % 10) << (i * 4);
        v /= 10;
    }
    if (v != 0) return false;
    *out = bcd;
    return true;
}

/*
 * Extended opcodes in executable position. Before this arm existed,
 * exec_term dispatched on a single byte, so every 0x5B statement fell
 * to the unknown-opcode tail: the prefix was skipped, the sub-opcode
 * and all of its operands were left in the stream, and exec_termlist
 * ran them as top-level TermObjs — with AML_OK reported to the caller.
 */
static aml_status_t exec_ext_term(uint8_t sub, const uint8_t** p,
                                   const uint8_t* end, exec_frame_t* fr,
                                   uint64_t* out, int depth) {
    aml_status_t s;
    *out = 0;

    switch (sub) {
    case ExtStallOp:
    case ExtSleepOp: {
        uint64_t t;
        s = exec_termarg(p, end, fr, &t, depth + 1);
        if (s != AML_OK) return s;
        if (sub == ExtSleepOp) {
            if (t > (~0ULL) / 1000ULL) {
                kprintf("[AML] Sleep(%lu ms) does not fit a microsecond "
                        "span — method aborted\n", (unsigned long)t);
                return AML_ERR_ABORT;
            }
            t *= 1000ULL;
        }
        return exec_delay_us(t);
    }
    case ExtAcquireOp:
        /* DefAcquire := AcquireOp MutexObject Timeout, and Timeout is
         * raw WordData rather than a TermArg. §19.6.1: the result is
         * True when the acquisition TIMED OUT. Nothing in this tree
         * evaluates AML on two cores — the only entry points are the
         * sleep path and \_PIC — so the mutex cannot be contended and
         * the acquisition always succeeds. */
        if (!skip_super_name(p, end, fr->scope, 0)) return AML_ERR_BAD_OPCODE;
        if ((size_t)(end - *p) < 2) return AML_ERR_BOUNDS;
        *p += 2;
        return AML_OK;

    case ExtReleaseOp:
    case ExtSignalOp:
    case ExtResetOp:
        if (!skip_super_name(p, end, fr->scope, 0)) return AML_ERR_BAD_OPCODE;
        return AML_OK;

    case ExtWaitOp: {
        /* §19.6.150: True means the wait timed out. No path in this
         * kernel signals an AML Event, so a Wait can only time out, and
         * saying so at once is the honest answer. */
        uint64_t timeout;
        if (!skip_super_name(p, end, fr->scope, 0)) return AML_ERR_BAD_OPCODE;
        s = exec_termarg(p, end, fr, &timeout, depth + 1);
        if (s != AML_OK) return s;
        *out = ~0ULL;
        return AML_OK;
    }
    case ExtCondRefOfOp: {
        /* §19.6.19 asks whether the object EXISTS and must NOT evaluate
         * it. Left to the NamePath arm, its NameString resolved to a
         * Field and the interpreter performed the very OpRegion read
         * the ASL was asking it to avoid. The Destination receives an
         * object reference, which this integer-only executor cannot
         * represent, so it is left untouched rather than filled with a
         * number that is not a reference. */
        const uint8_t* probe = *p;
        bool exists = (*p < end && **p == ExtOpPrefix)   /* DebugObj */
                    ? true
                    : resolve_name(&probe, end, fr->scope, false, NULL) != NULL;
        if (!skip_super_name(p, end, fr->scope, 0)) return AML_ERR_BAD_OPCODE;
        if (!skip_super_name(p, end, fr->scope, 0)) return AML_ERR_BAD_OPCODE;
        *out = exists ? ~0ULL : 0ULL;
        return AML_OK;
    }
    case ExtFromBCDOp:
    case ExtToBCDOp: {
        uint64_t a, r;
        s = exec_termarg(p, end, fr, &a, depth + 1);
        if (s != AML_OK) return s;
        if (!((sub == ExtFromBCDOp) ? bcd_to_integer(a, &r)
                                     : integer_to_bcd(a, &r))) {
            kprintf("[AML] %s: 0x%lx does not convert — method aborted\n",
                    (sub == ExtFromBCDOp) ? "FromBCD" : "ToBCD",
                    (unsigned long)a);
            return AML_ERR_ABORT;
        }
        uint8_t kind = 0;
        const uint8_t* tgt_p = *p;
        uint8_t tgt_op = (tgt_p < end) ? *tgt_p : 0;
        aml_object_t* tgt = resolve_target(p, end, fr, &kind);
        if (kind == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        if (kind == AML_TGT_LOCAL)    fr->locals[tgt_op - 0x60] = r;
        else if (kind == AML_TGT_ARG) fr->args[tgt_op - 0x68]   = r;
        else if (tgt)                 exec_store(r, tgt, fr, kind);
        *out = r;
        return AML_OK;
    }
    case ExtFatalOp: {
        /* DefFatal := FatalOp FatalType FatalCode FatalArg (§19.6.46) —
         * the firmware telling the OS it must not carry on. */
        if ((size_t)(end - *p) < 5) return AML_ERR_BOUNDS;
        uint8_t  type = (*p)[0];
        uint32_t code = (uint32_t)(*p)[1]        | ((uint32_t)(*p)[2] << 8) |
                        ((uint32_t)(*p)[3] << 16) | ((uint32_t)(*p)[4] << 24);
        *p += 5;
        uint64_t arg;
        s = exec_termarg(p, end, fr, &arg, depth + 1);
        if (s != AML_OK) return s;
        kprintf("[AML] firmware Fatal: type 0x%02x code 0x%08x arg 0x%lx\n",
                (unsigned)type, (unsigned)code, (unsigned long)arg);
        return AML_ERR_ABORT;
    }
    default:
        break;
    }

    /* Defined, but this executor has no honest value to give for it.
     * Step over its operands EXACTLY — so what follows is still a
     * TermList and not a walk through operand bytes — and then refuse
     * out loud instead of handing back a number nobody computed. */
    if (!skip_ext_op(sub, p, end, fr->scope)) return AML_ERR_BAD_OPCODE;
    kprintf("[AML] extended opcode 0x5B 0x%02x is not implemented by this "
            "executor — method aborted\n", sub);
    return AML_ERR_ABORT;
}

/* exec_term — read one expression, push integer result into *out.
 * Returns AML_OK on success. Recursive for compound exprs. */
static aml_status_t exec_term(const uint8_t** p, const uint8_t* end,
                               exec_frame_t* fr, uint64_t* out, int depth) {
    if (depth > EXEC_MAX_DEPTH) return AML_ERR_DEEP_NEST;
    if (*p >= end) return AML_ERR_BOUNDS;
    uint8_t op = **p;

    /* Local/Arg references as expressions read the value. */
    if (op >= 0x60 && op <= 0x67) { (*p)++; *out = fr->locals[op - 0x60]; return AML_OK; }
    if (op >= 0x68 && op <= 0x6E) { (*p)++; *out = fr->args[op - 0x68];   return AML_OK; }

    /* Integer literals. */
    if (op == 0x00 || op == 0x01 || op == 0xFF ||
        op == BytePrefix || op == WordPrefix ||
        op == DWordPrefix || op == QWordPrefix) {
        *out = read_integer(p, end);
        return AML_OK;
    }

    /* Binary arithmetic — most opcodes share the same shape:
     *   OP arg1 arg2 target  → result. */
#define BIN_OP(opcode_, expr)                                                 \
    if (op == (opcode_)) {                                                    \
        (*p)++;                                                               \
        uint64_t a, b;                                                        \
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);             \
        if (s != AML_OK) return s;                                            \
        s = exec_termarg(p, end, fr, &b, depth + 1);                          \
        if (s != AML_OK) return s;                                            \
        uint8_t tgt_kind = 0;                                                 \
        const uint8_t* tgt_p = *p;                                            \
        uint8_t tgt_op = (tgt_p < end) ? *tgt_p : 0;                          \
        aml_object_t* tgt = resolve_target(p, end, fr, &tgt_kind);            \
        if (tgt_kind == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;             \
        uint64_t r = (expr);                                                  \
        *out = r;                                                             \
        if (tgt_kind == 1) fr->locals[tgt_op - 0x60] = r;                     \
        else if (tgt_kind == 2) fr->args[tgt_op - 0x68] = r;                  \
        else if (tgt) exec_store(r, tgt, fr, tgt_kind);                       \
        return AML_OK;                                                        \
    }

    BIN_OP(AddOp,        a + b)
    BIN_OP(SubtractOp,   a - b)
    BIN_OP(MultiplyOp,   a * b)
    BIN_OP(ModOp,        b ? a % b : 0)
    BIN_OP(AndOp,        a & b)
    BIN_OP(OrOp,         a | b)
    BIN_OP(XorOp,        a ^ b)
    BIN_OP(NandOp,       ~(a & b))
    BIN_OP(NorOp,        ~(a | b))
    BIN_OP(ShiftLeftOp,  a << (b & 63))
    BIN_OP(ShiftRightOp, a >> (b & 63))

    /*
     * Divide is the one arithmetic op that does NOT fit BIN_OP:
     * DefDivide := DivideOp Dividend Divisor Remainder Quotient
     * (§20.2.5.4) — TWO Targets, the remainder first. Run through
     * BIN_OP it wrote the quotient into the Remainder slot, left the
     * Quotient operand in the stream, and exec_termlist then executed
     * that leftover Target as a statement: a NameString there would be
     * resolved and, if it named a Field, read from real hardware the
     * method never asked to touch. In the commonest ASL spelling,
     * `Divide (X, Y, , Local0)`, the NullName placeholder was eaten as
     * the only Target and the quotient was dropped altogether.
     */
    if (op == DivideOp) {
        (*p)++;
        uint64_t a, b;
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);
        if (s != AML_OK) return s;
        s = exec_termarg(p, end, fr, &b, depth + 1);
        if (s != AML_OK) return s;
        if (b == 0) {
            /* §19.6.32 makes divide-by-zero fatal, and ACPICA's
             * AcpiUtDivide answers AE_AML_DIVIDE_BY_ZERO. A quiet 0
             * would be a value this machine invented. */
            kprintf("[AML] Divide by zero — method aborted\n");
            return AML_ERR_ABORT;
        }
        uint64_t rem = a % b;
        uint64_t quo = a / b;

        uint8_t rk = 0;
        const uint8_t* rp = *p;
        uint8_t rop = (rp < end) ? *rp : 0;
        aml_object_t* rtgt = resolve_target(p, end, fr, &rk);
        if (rk == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        if (rk == AML_TGT_LOCAL)    fr->locals[rop - 0x60] = rem;
        else if (rk == AML_TGT_ARG) fr->args[rop - 0x68]   = rem;
        else if (rtgt)              exec_store(rem, rtgt, fr, rk);

        uint8_t qk = 0;
        const uint8_t* qp = *p;
        uint8_t qop = (qp < end) ? *qp : 0;
        aml_object_t* qtgt = resolve_target(p, end, fr, &qk);
        if (qk == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        if (qk == AML_TGT_LOCAL)    fr->locals[qop - 0x60] = quo;
        else if (qk == AML_TGT_ARG) fr->args[qop - 0x68]   = quo;
        else if (qtgt)              exec_store(quo, qtgt, fr, qk);

        *out = quo;          /* the expression's value is the quotient */
        return AML_OK;
    }

    /* Unary. */
    if (op == NotOp) {
        (*p)++;
        uint64_t a;
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);
        if (s != AML_OK) return s;
        uint8_t kind = 0;
        const uint8_t* tgt_p = *p;
        uint8_t tgt_op = (tgt_p < end) ? *tgt_p : 0;
        aml_object_t* tgt = resolve_target(p, end, fr, &kind);
        if (kind == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        uint64_t r = ~a;
        *out = r;
        if (kind == 1) fr->locals[tgt_op - 0x60] = r;
        else if (kind == 2) fr->args[tgt_op - 0x68] = r;
        else if (tgt) exec_store(r, tgt, fr, kind);
        return AML_OK;
    }
    if (op == IncrementOp || op == DecrementOp) {
        (*p)++;
        uint8_t kind = 0;
        const uint8_t* tgt_p = *p;
        uint8_t tgt_op = (tgt_p < end) ? *tgt_p : 0;
        aml_object_t* tgt = resolve_target(p, end, fr, &kind);
        if (kind == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        uint64_t v = (kind == 1) ? fr->locals[tgt_op - 0x60] :
                     (kind == 2) ? fr->args[tgt_op - 0x68] :
                     (tgt ? tgt->v.integer : 0);
        v = (op == IncrementOp) ? v + 1 : v - 1;
        if (kind == 1) fr->locals[tgt_op - 0x60] = v;
        else if (kind == 2) fr->args[tgt_op - 0x68] = v;
        else if (tgt) exec_store(v, tgt, fr, kind);
        *out = v;
        return AML_OK;
    }

    /* Logical comparisons. */
#define CMP_OP(opcode_, expr)                                                 \
    if (op == (opcode_)) {                                                    \
        (*p)++;                                                               \
        uint64_t a, b;                                                        \
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);             \
        if (s != AML_OK) return s;                                            \
        s = exec_termarg(p, end, fr, &b, depth + 1);                          \
        if (s != AML_OK) return s;                                            \
        *out = (expr) ? ~0ULL : 0ULL;                                         \
        return AML_OK;                                                        \
    }
    CMP_OP(LEqualOp,   a == b)
    CMP_OP(LGreaterOp, a >  b)
    CMP_OP(LLessOp,    a <  b)

    if (op == LAndOp || op == LOrOp) {
        (*p)++;
        uint64_t a, b;
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);
        if (s != AML_OK) return s;
        s = exec_termarg(p, end, fr, &b, depth + 1);
        if (s != AML_OK) return s;
        *out = ((op == LAndOp) ? (a && b) : (a || b)) ? ~0ULL : 0ULL;
        return AML_OK;
    }
    if (op == LNotOp) {
        (*p)++;
        uint64_t a;
        aml_status_t s = exec_termarg(p, end, fr, &a, depth + 1);
        if (s != AML_OK) return s;
        *out = (!a) ? ~0ULL : 0ULL;
        return AML_OK;
    }

    /* Store(value, target). */
    if (op == StoreOp) {
        (*p)++;
        uint64_t v;
        aml_status_t s = exec_termarg(p, end, fr, &v, depth + 1);
        if (s != AML_OK) return s;
        uint8_t kind = 0;
        const uint8_t* tgt_p = *p;
        uint8_t tgt_op = (tgt_p < end) ? *tgt_p : 0;
        aml_object_t* tgt = resolve_target(p, end, fr, &kind);
        if (kind == AML_TGT_ERROR) return AML_ERR_BAD_OPCODE;
        if (kind == 1) fr->locals[tgt_op - 0x60] = v;
        else if (kind == 2) fr->args[tgt_op - 0x68] = v;
        else if (kind == AML_TGT_DEBUG)
            debug_printf("[AML] Store -> Debug: 0x%lx\n", (unsigned long)v);
        else if (tgt) exec_store(v, tgt, fr, kind);
        *out = v;
        return AML_OK;
    }

    if (op == SizeOfOp) {
        (*p)++;
        aml_object_t* o = resolve_name(p, end, fr->scope, false, NULL);
        *out = (o && o->type == AML_OBJ_BUFFER)  ? o->v.buffer.len :
               (o && o->type == AML_OBJ_STRING)  ? o->v.string.len :
               (o && o->type == AML_OBJ_PACKAGE) ? o->v.package.count : 0;
        return AML_OK;
    }

    /* If / While / Return. */
    if (op == IfOp) {
        (*p)++;
        uint32_t pklen_bytes;
        uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
        if (!pklen_bytes) return AML_ERR_BOUNDS;
        const uint8_t* body = *p + pklen_bytes;
        const uint8_t* body_end = *p + pklen;
        if (body_end > end) return AML_ERR_BOUNDS;
        const uint8_t* cur = body;
        uint64_t cond;
        aml_status_t s = exec_termarg(&cur, body_end, fr, &cond, depth + 1);
        if (s != AML_OK) { *p = body_end; return s; }
        if (cond) {
            uint32_t rest = (uint32_t)(body_end - cur);
            s = exec_termlist(cur, rest, fr, depth + 1);
            if (s != AML_OK) { *p = body_end; return s; }
        }
        *p = body_end;
        /* Optional Else. */
        if (*p < end && **p == ElseOp) {
            (*p)++;
            uint32_t epk_bytes;
            uint32_t epk = decode_pkglen(p, end, &epk_bytes);
            if (!epk_bytes) return AML_OK;
            const uint8_t* ebody = *p + epk_bytes;
            const uint8_t* eend  = *p + epk;
            if (eend > end) return AML_OK;
            if (!cond) {
                s = exec_termlist(ebody, (uint32_t)(eend - ebody), fr,
                                  depth + 1);
                if (s != AML_OK) { *p = eend; return s; }
            }
            *p = eend;
        }
        *out = 0;
        return AML_OK;
    }
    if (op == WhileOp) {
        (*p)++;
        uint32_t pklen_bytes;
        uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
        if (!pklen_bytes) return AML_ERR_BOUNDS;
        const uint8_t* body = *p + pklen_bytes;
        const uint8_t* body_end = *p + pklen;
        if (body_end > end) return AML_ERR_BOUNDS;
        uint32_t iters = 0;
        aml_status_t s = AML_OK;
        while (iters++ < EXEC_MAX_WHILE) {
            const uint8_t* cur = body;
            uint64_t cond;
            s = exec_termarg(&cur, body_end, fr, &cond, depth + 1);
            if (s != AML_OK || !cond) break;
            fr->continue_flag = false;
            fr->break_flag    = false;
            s = exec_termlist(cur, (uint32_t)(body_end - cur), fr, depth + 1);
            if (s != AML_OK || fr->returned || fr->break_flag) break;
        }
        /* §19.6.8: Break ends the INNERMOST While and execution resumes
         * with the statement that follows it. The flag must not outlive
         * the loop it terminated. Left standing it was also what
         * exec_termlist tests, so everything after the loop — the
         * Return included — was skipped, and every enclosing While in
         * the same frame ended too. iASL compiles every ASL Switch into
         * While(One){ ... Break }, so a Break at the end of a While is
         * the ordinary shape of compiled firmware, not an error path. */
        fr->break_flag    = false;
        fr->continue_flag = false;
        *p = body_end;
        *out = 0;
        return s;
    }
    if (op == ReturnOp) {
        (*p)++;
        uint64_t v;
        aml_status_t s = exec_termarg(p, end, fr, &v, depth + 1);
        if (s != AML_OK) return s;
        fr->ret_value = v;
        fr->returned  = true;
        *out = v;
        return AML_OK;
    }
    if (op == BreakOp)    { (*p)++; fr->break_flag = true; *out = 0; return AML_OK; }
    /* DefContinue := ContinueOp (§20.2.5.3): the rest of this iteration
     * is skipped and the While re-tests its predicate. */
    if (op == ContinueOp) { (*p)++; fr->continue_flag = true; *out = 0; return AML_OK; }

    /* Extended opcodes — §20.2.3: the opcode is the two-byte sequence
     * ExtOpPrefix + sub-opcode, so the stream may never be re-entered
     * at the sub-opcode byte. */
    if (op == ExtOpPrefix) {
        if ((size_t)(end - *p) < 2) return AML_ERR_BOUNDS;
        uint8_t sub = (*p)[1];
        *p += 2;
        return exec_ext_term(sub, p, end, fr, out, depth);
    }

    /* NamePath / method invocation. §20.2.2: LeadNameChar := 'A'-'Z' |
     * '_'. A digit may NOT lead a NameSeg — accepting one here let the
     * ASCII-digit sub-opcodes (RevisionOp 0x30 .. TimerOp 0x33) be read
     * as a name, swallowing three bytes of the next statement with it. */
    if (op == RootChar || op == ParentPrefixChar ||
        op == DualNamePrefix || op == MultiNamePrefix ||
        (op >= 'A' && op <= 'Z') || op == '_') {
        aml_object_t* o = resolve_name(p, end, fr->scope, false, NULL);
        if (!o) { *out = 0; return AML_OK; }
        if (o->type == AML_OBJ_INTEGER) { *out = o->v.integer; return AML_OK; }
        if (o->type == AML_OBJ_FIELD)   { *out = field_read(o); return AML_OK; }
        if (o->type == AML_OBJ_NAME)    { *out = o->v.integer; return AML_OK; }
        if (o->type == AML_OBJ_METHOD) {
            uint8_t argc = (uint8_t)(o->v.method.flags & 0x7);
            exec_frame_t inner = {0};
            inner.scope = o->parent ? o->parent : fr->scope;
            inner.argc  = argc;
            for (uint8_t i = 0; i < argc; i++) {
                uint64_t v;
                aml_status_t s = exec_termarg(p, end, fr, &v, depth + 1);
                if (s != AML_OK) return s;
                inner.args[i] = v;
            }
            aml_status_t ms = exec_termlist(o->v.method.aml,
                                            o->v.method.aml_len,
                                            &inner, depth + 1);
            if (ms != AML_OK) return ms;
            *out = inner.ret_value;
            return AML_OK;
        }
        *out = 0;
        return AML_OK;
    }

    /* Unknown opcode. The stream is no longer a TermList this executor
     * can trust: advancing one byte and answering AML_OK resumed inside
     * the operands, which is how an unimplemented opcode turned into a
     * hardware access. Stop, and name the byte that stopped us — the
     * old comment said "and report" and nothing ever was. */
    kprintf("[AML] opcode 0x%02x is not implemented by this executor — "
            "method aborted\n", op);
    return AML_ERR_BAD_OPCODE;
}

static aml_status_t exec_termlist(const uint8_t* aml, uint32_t len,
                                   exec_frame_t* fr, int depth) {
    const uint8_t* p   = aml;
    const uint8_t* end = aml + len;
    while (p < end && !fr->returned && !fr->break_flag && !fr->continue_flag) {
        const uint8_t* before = p;
        uint64_t discard;
        aml_status_t s = exec_term(&p, end, fr, &discard, depth);
        if (s != AML_OK) return s;
        if (p == before) p++;
    }
    return AML_OK;
}

aml_status_t aml_call_int(const char* path, uint64_t* args, int argc,
                           uint64_t* ret_value) {
    if (!g_loaded) return AML_ERR_NOT_LOADED;
    aml_object_t* m = NULL;
    aml_status_t s = aml_find(path, &m);
    if (s != AML_OK) return s;
    if (m->type != AML_OBJ_METHOD) {
        /* Reading a Name yields its integer value directly. */
        if (m->type == AML_OBJ_INTEGER || m->type == AML_OBJ_NAME) {
            if (ret_value) *ret_value = m->v.integer;
            return AML_OK;
        }
        return AML_ERR_TYPE;
    }
    exec_frame_t fr = {0};
    fr.scope = m->parent ? m->parent : &g_root_obj;
    fr.argc  = (uint8_t)((argc < 7) ? argc : 7);
    for (int i = 0; i < fr.argc; i++) fr.args[i] = args[i];
    /* A method that stopped short has no return value worth handing
     * back: the caller must see the refusal, not a zero that reads
     * exactly like a firmware answer of zero. */
    aml_status_t es = exec_termlist(m->v.method.aml, m->v.method.aml_len,
                                    &fr, 0);
    if (es != AML_OK) return es;
    if (ret_value) *ret_value = fr.ret_value;
    return AML_OK;
}
