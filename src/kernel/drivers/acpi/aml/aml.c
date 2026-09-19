#include "aml.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "klib.h"
#include "touch.h"

/* =====================================================================
 * AML table reader — the namespace half of ACPI 6.5 §20, and only that.
 *
 * There is no executor here and none is missing; see aml.h for why.
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
 *
 * A Method is RECORDED — its flags and its TermList byte range are kept
 * so the object is a faithful picture of what the firmware declared —
 * and it is never run. Same for an OperationRegion and the Fields cut
 * out of it: this file writes down the address space, the base, the
 * length and every field's bit offset and width, and touches none of
 * them. Nothing in this file performs an I/O, MMIO or PCI-config access.
 * ===================================================================== */

/* How many namespace objects come out of one allocation.
 *
 * What stood here was AML_MAX_NAMESPACE_OBJ = 4096 backing a single static
 * array, and it is not a theoretical ceiling: the DSDT + SSDTs of an ASRock
 * B365M Pro4-F build 4615 objects. The 519th object past the wall got NULL
 * from obj_alloc, obj_create_child passed the NULL up, the caller answered
 * AML_ERR_NOMEM, and parse_term_list resynchronised and carried on — so the
 * tail of that firmware's namespace simply did not exist, silently, and any
 * method naming something in it resolved to nothing.
 *
 * The namespace is as big as the firmware makes it, so the pool is a CHAIN of
 * chunks rather than an array with a number on it. Chunks are never resized
 * and never moved: every aml_object_t holds parent/child/sibling pointers
 * into the pool, so an object's address must outlive every later allocation.
 * The first chunk is static, which keeps the DSDT walk working whatever state
 * the heap is in; the rest come from kmalloc, and running out of heap is said
 * out loud instead of quietly truncating the machine's namespace. */
#define AML_POOL_CHUNK_OBJS     512
#define AML_MAX_SCOPE_DEPTH     32
#define AML_MAX_BUFFER_BYTES    (1u << 20)

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

/* Executable opcodes (single-byte). Nothing here executes them: they are
 * the shapes skip_term_arg has to MEASURE, because an executable term can
 * stand in a namespace declaration's operand (a RegionOffset, a BankValue)
 * and the TermList scan has to step over it exactly. */
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

typedef struct AmlPoolChunk {
    struct AmlPoolChunk* next;
    uint32_t             used;
    aml_object_t         obj[AML_POOL_CHUNK_OBJS];
} AmlPoolChunk;

static aml_object_t  g_root_obj;
static AmlPoolChunk  g_pool_first;          /* static: no heap needed to start */
static AmlPoolChunk* g_pool_tail      = NULL;
static uint32_t      g_pool_used      = 0;  /* objects across the whole chain */
static bool          g_pool_exhausted = false;
static bool          g_loaded         = false;

/* Drop every heap chunk and start the chain over at the static one. Called
 * once per aml_init, so a build that failed part-way does not hand its
 * half-built objects to the next attempt. */
static void pool_reset(void) {
    AmlPoolChunk* c = g_pool_first.next;
    while (c) {
        AmlPoolChunk* next = c->next;
        kfree(c);
        c = next;
    }
    g_pool_first.next = NULL;
    g_pool_first.used = 0;
    g_pool_tail       = &g_pool_first;
    g_pool_used       = 0;
    g_pool_exhausted  = false;
}

static aml_object_t* obj_alloc(void) {
    if (!g_pool_tail) pool_reset();

    if (g_pool_tail->used >= AML_POOL_CHUNK_OBJS) {
        AmlPoolChunk* c = (AmlPoolChunk*)kmalloc(sizeof(AmlPoolChunk));
        if (!c) {
            /* Once, not per object: a firmware whose namespace does not fit
             * would otherwise say this thousands of times. kprintf, because
             * everything the truncated tail declares is missing for the rest
             * of the machine's life and nothing downstream can tell that from
             * a firmware that never declared it. */
            if (!g_pool_exhausted) {
                g_pool_exhausted = true;
                kprintf("[AML] the namespace reached %u objects and the heap "
                        "has no room for the next %u — every object past this "
                        "point is LOST, and a method that names one will not "
                        "find it\n",
                        g_pool_used, (unsigned)AML_POOL_CHUNK_OBJS);
            }
            return NULL;
        }
        c->next = NULL;
        c->used = 0;
        g_pool_tail->next = c;
        g_pool_tail       = c;
    }

    aml_object_t* o = &g_pool_tail->obj[g_pool_tail->used++];
    g_pool_used++;
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
 * first byte of a NameSeg puts every later field at the wrong bit offset,
 * so the namespace ends up recording a register layout the firmware never
 * declared, and the walk can run past body_end fabricating names out of
 * operand bytes. Byte counts match ACPICA's AcpiPsGetNextField.
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
         * namespace level. This pass has no frame to evaluate one in, so
         * the non-constant form is stepped over EXACTLY — the measurement
         * is what keeps the rest of the TermList aligned — and the object
         * records the base it could read, which is zero when the firmware
         * named none. */
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
 * Public API
 * =========================================================== */

aml_status_t aml_init(void) {
    if (g_loaded) return AML_OK;
    memset(&g_root_obj, 0, sizeof(g_root_obj));
    g_root_obj.type = AML_OBJ_SCOPE;
    g_root_obj.name[0] = '\\';
    pool_reset();

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
    /* kprintf: the object count is the first thing to compare against the
     * same firmware's namespace on another OS when a method cannot be found,
     * and it is how a truncated build is spotted from a photograph of the
     * screen. */
    kprintf("[AML] namespace loaded: %u objects across DSDT + SSDTs%s\n",
            g_pool_used,
            g_pool_exhausted ? " (TRUNCATED — the heap ran out, see above)"
                             : "");
    /* Announced on the tag bus with the object count as payload. There is
     * no subscriber today, and no reader of the namespace at all — see the
     * header for why the executor that used to be the reader is gone. The
     * announcement is what a later reader binds to; it is not evidence that
     * one exists. */
    uint32_t cnt = g_pool_used;
    TouchPublish("aml:ready", &cnt, sizeof(cnt));
    return AML_OK;
}
