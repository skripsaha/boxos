#include "aml.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "klib.h"
#include "touch.h"
/* irqchip.h, for one question and one only: which interrupt model this kernel
 * is actually running. ACPI 6.5 §5.8.1 makes that the argument to \_PIC, and
 * the honest answer is the controller's own, not the order of calls in
 * main(). Nothing else in this file looks at the interrupt system. */
#include "irqchip.h"

/* =====================================================================
 * AML table reader — the namespace half of ACPI 6.5 §20, plus the narrow
 * evaluator that asks it where a PCI interrupt goes.
 *
 * There is no general executor here and none is missing; see aml.h for why,
 * and for the boundary the evaluator refuses to cross.
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
        /* A Package keeps the BYTES of its element list, not a summary of
         * them. What stood here was `first_int` — the value of element zero,
         * captured on the chance a caller wanted _S5[0] — and no caller ever
         * did; _S5 is read by the standalone scanner in acpi_dsdt.c. A
         * routing table is a package of packages, so the reader needs the
         * elements themselves, and `body == NULL` says the element count
         * could not be read, which is a refusal rather than a count of
         * zero. */
        struct {
            const uint8_t* body;        /* first element, past NumElements */
            const uint8_t* body_end;
            uint32_t       count;
        } package;
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
        /* ACPI 6.5 §20.2.2: MultiNamePath's SegCount is ByteData, and "SegCount
         * can be from 1 to 255". Sixteen was a ceiling nobody chose, on the
         * path that resolves every _PRT Source name. What actually bounds the
         * walk is the data: a SegCount claiming more four-character segments
         * than the body holds is refused on the next line. */
        segs = **p; (*p)++;
        if (segs == 0) return NULL;
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

    /* ‼ THE BUFFER IS AS LONG AS ITS BYTES, not as long as it claims.
     *
     * ACPI 6.5 §20.2.5.4: DefBuffer := BufferOp PkgLength BufferSize
     * ByteList. BufferSize is the buffer's LOGICAL length and the ByteList
     * may be shorter — "Buffer(256){}" is legal AML whose tail is zero. The
     * length recorded here is the length that may be READ, which is what the
     * ByteList actually holds; storing the declared size let every reader of
     * such a buffer walk past the end of the table.
     *
     * This also retires AML_MAX_BUFFER_BYTES, an invented 1 MiB with nothing
     * behind it. The bound that exists is the one above: the package length
     * the firmware itself wrote, inside the table whose own header says how
     * long it is. */
    uint64_t have = (uint64_t)(body_end - bp);
    if (size > have) size = have;

    obj->type = AML_OBJ_BUFFER;
    obj->v.buffer.bytes = bp;
    obj->v.buffer.len   = (uint32_t)size;
    *p = body_end;
    return AML_OK;
}

static aml_status_t handle_package(const uint8_t** p, const uint8_t* end,
                                    aml_object_t* obj, bool is_var) {
    (*p)++;     /* PackageOp / VarPackageOp */
    uint32_t pklen_bytes;
    uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
    if (!pklen_bytes) return AML_ERR_BOUNDS;
    const uint8_t* body_start = *p + pklen_bytes;
    const uint8_t* body_end   = *p + pklen;
    if (body_end > end || body_start >= body_end) return AML_ERR_BOUNDS;

    obj->type = AML_OBJ_PACKAGE;

    /* §20.2.5.4: DefPackage's NumElements is one ByteData, DefVarPackage's is
     * a TermArg. Both were read as a byte here, which put a VarPackage's
     * elements at the wrong offset and its count at whatever the first byte
     * of the count expression happened to be — a routing table read that way
     * would be entries invented out of opcode bytes. A count this pass cannot
     * read leaves `body` NULL, and every reader treats that as a refusal. */
    const uint8_t* bp = body_start;
    uint64_t num = 0;
    bool counted = is_var ? read_const_integer(&bp, body_end, &num)
                          : (bp < body_end ? (num = *bp++, true) : false);
    if (!counted) {
        obj->v.package.body     = NULL;
        obj->v.package.body_end = NULL;
        obj->v.package.count    = 0;
        *p = body_end;
        return AML_OK;
    }

    obj->v.package.body     = bp;
    obj->v.package.body_end = body_end;
    obj->v.package.count    = (uint32_t)num;
    *p = body_end;
    return AML_OK;
}

static aml_status_t handle_named_data(const uint8_t** p, const uint8_t* end,
                                       aml_object_t* obj) {
    /* DataRefObject following a NameOp NameString. */
    if (*p >= end) return AML_ERR_BOUNDS;
    uint8_t op = **p;
    if (op == BufferOp)        return handle_buffer(p, end, obj);
    if (op == PackageOp)       return handle_package(p, end, obj, false);
    if (op == VarPackageOp)    return handle_package(p, end, obj, true);
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

/* =====================================================================
 * THE NARROW EVALUATOR — enough AML to ask the firmware where a PCI
 * interrupt goes, and NOTHING that touches hardware.
 *
 * ‼ WHAT THIS IS ALLOWED TO DO, and why the boundary is where it is.
 *
 * An executor lived in this file once and was removed with the ACPI S1-S4
 * sleep path it served — all of it, including the OperationRegion and Field
 * access path, which is the only way AML ever reaches a register. That
 * removal stands. What comes back here is a READER: If/Else, Return, Store
 * into a local, an argument or a namespace Name, the logical operators, and
 * the reading of Packages and Buffers. Exactly the shape a board writes when
 * it answers "where does this interrupt go":
 *
 *     Method (_PRT, 0) { If (PICM) { Return (AR00) } Return (PR00) }
 *
 * The moment a term needs a value out of an OperationRegion, the evaluator
 * REFUSES ALOUD and abandons the method half-run rather than inventing a
 * number for it. That is not a gap to be filled later by widening this file;
 * it is the boundary, and crossing it is a question for the owner.
 *
 * ‼ AND IT NEVER ANSWERS ZERO INSTEAD OF REFUSING. Every function here that
 * cannot produce a value returns false with a sentence on the console naming
 * what it could not do. A fabricated 0 in this path is a GSI, and a
 * fabricated GSI is a device wired to an interrupt nobody delivers.
 *
 * ‼ HOW DEEP IT CAN GO — by construction, not by a counter.
 *   * Statements: no C recursion at all. A method body is walked with an
 *     explicit block stack that grows on the heap with the nesting the
 *     firmware actually wrote; what ends the growth is kmalloc saying no,
 *     and that is said out loud.
 *   * Expressions: the same. AML is prefix notation, so an explicit stack of
 *     pending operators folds it without the C stack ever descending.
 *   * Methods: ONE frame exists at a time, because a NameString that resolves
 *     to a Method is REFUSED in expression position. The evaluator runs the
 *     method it was asked for and no other, so there is no chain to bound and
 *     no cycle to detect.
 * There is therefore no AML_MAX_EVAL_DEPTH in this file, and there is no
 * place one could be needed.
 *
 * Single-threaded by the only thing that calls it: the routing table is read
 * once, on the BSP, during device bring-up, before any other core arms an
 * interrupt.
 * ===================================================================== */

/* Statement opcodes — ACPI 6.5 §20.2.5.3. */
#define IfOp             0xA0
#define ElseOp           0xA1
#define WhileOp          0xA2
#define NoopOp           0xA3
#define ReturnOp         0xA4

/* ACPI 6.5 §20.2.6.2 gives the locals as Local0..Local7 and §20.2.6.1 the
 * arguments as Arg0..Arg6. Both counts are the specification's; the opcode
 * ranges 0x60..0x67 and 0x68..0x6E are Table 20.2's. */
#define Local0Op         0x60
#define Local7Op         0x67
#define Arg0Op           0x68
#define Arg6Op           0x6E
#define AML_LOCAL_COUNT  8
#define AML_ARG_COUNT    7

typedef struct AmlValue {
    aml_obj_type_t type;
    uint64_t       integer;
    const uint8_t *bytes;        /* STRING/BUFFER contents; PACKAGE elements */
    const uint8_t *bytes_end;
    uint32_t       count;        /* PACKAGE: the element count it declares */
    aml_object_t  *scope;        /* where a NameString inside it resolves */
    aml_object_t  *node;         /* set when the operand WAS a name, not its value */
} AmlValue;

typedef struct AmlFrame {
    AmlValue      local[AML_LOCAL_COUNT];
    AmlValue      arg[AML_ARG_COUNT];
    aml_object_t *scope;         /* the method object — names resolve from here */
    const char   *what;          /* its name, printed in every refusal */
} AmlFrame;

/* One pending prefix operator and the operands collected for it so far. */
typedef struct AmlPending {
    uint8_t  op;
    uint8_t  need;
    uint8_t  have;
    AmlValue got[2];
} AmlPending;

/* One TermList still being walked: a method body, or the body of an If or an
 * Else that was entered. */
typedef struct AmlBlock {
    const uint8_t *p;
    const uint8_t *end;
} AmlBlock;

/*
 * Make room for `want` items, copying what is already there. There is no
 * ceiling: the array grows with the nesting the firmware wrote, and the only
 * thing that stops it is the heap refusing — which the caller says aloud.
 * Doubling is an allocation step, not a limit; nothing here decides how deep
 * a board is allowed to nest.
 */
static void *AmlStackRoom(void *base, uint32_t *room, uint32_t want,
                           uint32_t item) {
    if (want <= *room) return base;
    uint32_t bigger = *room ? *room * 2u : 1u;
    while (bigger < want) bigger *= 2u;
    void *fresh = kmalloc((size_t)bigger * item);
    if (!fresh) return NULL;
    if (base) {
        memcpy(fresh, base, (size_t)(*room) * item);
        kfree(base);
    }
    *room = bigger;
    return fresh;
}

static const char *AmlTypeName(aml_obj_type_t t) {
    switch (t) {
    case AML_OBJ_INTEGER:   return "an integer";
    case AML_OBJ_STRING:    return "a string";
    case AML_OBJ_BUFFER:    return "a buffer";
    case AML_OBJ_PACKAGE:   return "a package";
    case AML_OBJ_DEVICE:    return "a device";
    case AML_OBJ_METHOD:    return "a method";
    case AML_OBJ_REGION:    return "an operation region";
    case AML_OBJ_FIELD:     return "a field of an operation region";
    case AML_OBJ_SCOPE:     return "a scope";
    case AML_OBJ_MUTEX:     return "a mutex";
    case AML_OBJ_EVENT:     return "an event";
    case AML_OBJ_POWER_RES: return "a power resource";
    case AML_OBJ_PROCESSOR: return "a processor";
    case AML_OBJ_THERMAL:   return "a thermal zone";
    case AML_OBJ_ALIAS:     return "an alias";
    case AML_OBJ_NAME:      return "a name with no value";
    case AML_OBJ_UNINIT:    break;
    }
    return "nothing this pass recorded";
}

static aml_object_t *AmlChild(aml_object_t *parent, const char *seg) {
    if (!parent) return NULL;
    char s[4] = { seg[0], seg[1], seg[2], seg[3] };
    return obj_find_child(parent, s);
}

/* The value a namespace object HOLDS. A Method is not one of them: reading a
 * method means running it, and that decision belongs to the caller, never to
 * a dereference buried inside an expression. */
static bool AmlValueOfNode(aml_object_t *node, const char *what,
                            AmlValue *out) {
    memset(out, 0, sizeof(*out));
    out->node = node;
    switch (node->type) {
    case AML_OBJ_INTEGER:
        out->type    = AML_OBJ_INTEGER;
        out->integer = node->v.integer;
        return true;
    case AML_OBJ_STRING:
        out->type      = AML_OBJ_STRING;
        out->bytes     = (const uint8_t *)node->v.string.str;
        out->bytes_end = out->bytes + node->v.string.len;
        return true;
    case AML_OBJ_BUFFER:
        out->type      = AML_OBJ_BUFFER;
        out->bytes     = node->v.buffer.bytes;
        out->bytes_end = node->v.buffer.bytes + node->v.buffer.len;
        return true;
    case AML_OBJ_PACKAGE:
        if (!node->v.package.body) {
            kprintf("[AML] %s: package %s was declared with an element count "
                    "this pass could not read, so its elements have no known "
                    "start — refused rather than guessed\n", what, node->name);
            return false;
        }
        out->type      = AML_OBJ_PACKAGE;
        out->bytes     = node->v.package.body;
        out->bytes_end = node->v.package.body_end;
        out->count     = node->v.package.count;
        out->scope     = node->parent;
        return true;
    default:
        kprintf("[AML] %s: %s is %s, which holds no value this evaluator can "
                "read\n", what, node->name, AmlTypeName(node->type));
        return false;
    }
}

/*
 * One operand, read from the stream.
 *
 * `fr` may be NULL — then Local and Arg are refused, which is what reading a
 * Package's elements outside any method should do.
 * `deref` says what a NameString means here: in an expression it means the
 * VALUE the name holds; in a _PRT entry's Source field it means the object
 * ITSELF, and dereferencing it would throw away the very thing being asked
 * for. The node is recorded in both cases.
 */
static bool AmlReadOperand(const uint8_t **p, const uint8_t *end,
                            aml_object_t *scope, AmlFrame *fr, bool deref,
                            const char *what, AmlValue *out) {
    if (*p >= end) {
        kprintf("[AML] %s: an operand was expected and the body ended\n", what);
        return false;
    }
    memset(out, 0, sizeof(*out));
    uint8_t op = **p;

    switch (op) {
    case ZeroOp: case OneOp: case OnesOp:
    case BytePrefix: case WordPrefix: case DWordPrefix: case QWordPrefix: {
        uint64_t v = 0;
        if (!read_const_integer(p, end, &v)) {
            kprintf("[AML] %s: integer opcode 0x%02x runs past the end of the "
                    "body\n", what, op);
            return false;
        }
        out->type = AML_OBJ_INTEGER;
        out->integer = v;
        return true;
    }
    case StringPrefix: {
        (*p)++;
        const uint8_t *s = *p;
        while (*p < end && **p) (*p)++;
        if (*p >= end) {
            kprintf("[AML] %s: a string runs past the end of the body\n", what);
            return false;
        }
        out->type      = AML_OBJ_STRING;
        out->bytes     = s;
        out->bytes_end = *p;
        (*p)++;                              /* the NUL */
        return true;
    }
    case BufferOp: {
        (*p)++;
        uint32_t cb = 0;
        uint32_t pklen = decode_pkglen(p, end, &cb);
        if (!cb || pklen < cb) {
            kprintf("[AML] %s: a buffer's package length is malformed\n", what);
            return false;
        }
        const uint8_t *body_start = *p + cb;
        const uint8_t *body_end   = *p + pklen;
        if (body_end > end) {
            kprintf("[AML] %s: a buffer runs past the end of the body\n", what);
            return false;
        }
        const uint8_t *q = body_start;
        uint64_t size = 0;
        if (!read_const_integer(&q, body_end, &size)) {
            kprintf("[AML] %s: a buffer's size is not a constant this "
                    "evaluator can read\n", what);
            return false;
        }
        /* The same rule handle_buffer applies, and for the same reason:
         * §20.2.5.4 makes BufferSize the LOGICAL length and lets the
         * ByteList be shorter, the tail being zero. What may be read is what
         * is there, so the end is the smaller of the two. Not a
         * malformation and not clamped silently — this IS the encoding. */
        out->type      = AML_OBJ_BUFFER;
        out->bytes     = q;
        out->bytes_end = ((uint64_t)(body_end - q) < size) ? body_end
                                                           : q + (size_t)size;
        *p = body_end;
        return true;
    }
    case PackageOp: case VarPackageOp: {
        (*p)++;
        uint32_t cb = 0;
        uint32_t pklen = decode_pkglen(p, end, &cb);
        if (!cb || pklen < cb) {
            kprintf("[AML] %s: a package's length is malformed\n", what);
            return false;
        }
        const uint8_t *body_start = *p + cb;
        const uint8_t *body_end   = *p + pklen;
        if (body_end > end) {
            kprintf("[AML] %s: a package runs past the end of the body\n",
                    what);
            return false;
        }
        const uint8_t *q = body_start;
        uint64_t n = 0;
        if (op == PackageOp) {
            /* §20.2.5.4: DefPackage's NumElements is one ByteData. */
            if (q >= body_end) {
                kprintf("[AML] %s: a package declares no element count\n",
                        what);
                return false;
            }
            n = *q++;
        } else if (!read_const_integer(&q, body_end, &n)) {
            /* §20.2.5.4: DefVarPackage's NumElements is a TermArg, and one
             * that is not a constant would need a frame to evaluate. */
            kprintf("[AML] %s: a variable package's element count is not a "
                    "constant — its elements have no known start\n", what);
            return false;
        }
        out->type      = AML_OBJ_PACKAGE;
        out->count     = (uint32_t)n;
        out->bytes     = q;
        out->bytes_end = body_end;
        out->scope     = scope;
        *p = body_end;
        return true;
    }
    default:
        break;
    }

    if (op >= Local0Op && op <= Local7Op) {
        if (!fr) {
            kprintf("[AML] %s: Local%u stands outside any method body\n",
                    what, (unsigned)(op - Local0Op));
            return false;
        }
        *out = fr->local[op - Local0Op];
        (*p)++;
        return true;
    }
    if (op >= Arg0Op && op <= Arg6Op) {
        if (!fr) {
            kprintf("[AML] %s: Arg%u stands outside any method body\n",
                    what, (unsigned)(op - Arg0Op));
            return false;
        }
        *out = fr->arg[op - Arg0Op];
        (*p)++;
        return true;
    }

    if (aml_is_name_string_lead(op) && op != 0x00) {
        if (!scope) {
            kprintf("[AML] %s: a name stands here with no scope to resolve it "
                    "against\n", what);
            return false;
        }
        aml_object_t *o = resolve_name(p, end, scope, false, NULL);
        if (!o) {
            kprintf("[AML] %s: a name in this term is not in the namespace — "
                    "no value is invented for it\n", what);
            return false;
        }
        if (o->type == AML_OBJ_METHOD) {
            if (!deref) { out->node = o; return true; }
            kprintf("[AML] %s: taking the value of %s means RUNNING it, and "
                    "this evaluator runs only the method it was asked for — "
                    "refused\n", what, o->name);
            return false;
        }
        if (!deref) {
            out->node = o;
            return true;
        }
        return AmlValueOfNode(o, what, out);
    }

    kprintf("[AML] %s: opcode 0x%02x in operand position is not one this "
            "evaluator reads\n", what, op);
    return false;
}

/* The logical operators, and only they. §20.2.5.4 gives each one's operand
 * count; anything else in operator position is refused by name. */
static bool AmlLogicalArity(uint8_t op, uint8_t *need) {
    switch (op) {
    case LNotOp:     *need = 1; return true;
    case LAndOp: case LOrOp: case LEqualOp:
    case LGreaterOp: case LLessOp: *need = 2; return true;
    default: return false;
    }
}

static bool AmlFold(const AmlPending *n, const char *what, AmlValue *out) {
    for (uint8_t i = 0; i < n->need; i++) {
        if (n->got[i].type != AML_OBJ_INTEGER) {
            kprintf("[AML] %s: logical opcode 0x%02x was handed %s, and this "
                    "evaluator compares integers only\n",
                    what, n->op, AmlTypeName(n->got[i].type));
            return false;
        }
    }
    uint64_t a = n->got[0].integer;
    uint64_t b = (n->need == 2) ? n->got[1].integer : 0;
    bool r;
    switch (n->op) {
    case LNotOp:     r = (a == 0);            break;
    case LAndOp:     r = (a != 0 && b != 0);  break;
    case LOrOp:      r = (a != 0 || b != 0);  break;
    case LEqualOp:   r = (a == b);            break;
    case LGreaterOp: r = (a >  b);            break;
    case LLessOp:    r = (a <  b);            break;
    default:         return false;
    }
    memset(out, 0, sizeof(*out));
    out->type = AML_OBJ_INTEGER;
    /* §20.2.5.4: a logical operator yields Ones for true, Zero for false. */
    out->integer = r ? ~0ULL : 0ULL;
    return true;
}

/*
 * One TermArg, folded with an explicit stack. AML is prefix notation, so the
 * operators arrive before their operands: each one is pushed with the number
 * of operands it needs, each finished value is handed to the top of the
 * stack, and a node whose operands are all in folds into a value of its own.
 * The C stack never descends, whatever the firmware nests.
 */
static bool AmlEvalTermArg(const uint8_t **p, const uint8_t *end,
                            AmlFrame *fr, AmlValue *out) {
    AmlPending *st = NULL;
    uint32_t    room = 0, n = 0;
    AmlValue    v;
    bool        have = false, ok = false;

    memset(&v, 0, sizeof(v));
    for (;;) {
        if (!have) {
            if (*p >= end) {
                kprintf("[AML] %s: a term ends where an operand was "
                        "expected\n", fr->what);
                break;
            }
            uint8_t op = **p;
            uint8_t need = 0;
            if (AmlLogicalArity(op, &need)) {
                AmlPending *grown = AmlStackRoom(st, &room, n + 1, sizeof(*st));
                if (!grown) {
                    kprintf("[AML] %s: the heap has no room to hold this "
                            "expression's operators — the term is refused, "
                            "not guessed\n", fr->what);
                    break;
                }
                st = grown;
                st[n].op   = op;
                st[n].need = need;
                st[n].have = 0;
                n++;
                (*p)++;
                continue;
            }
            if (!AmlReadOperand(p, end, fr->scope, fr, true, fr->what, &v))
                break;
            have = true;
        }
        if (n == 0) { ok = true; break; }

        AmlPending *top = &st[n - 1];
        top->got[top->have++] = v;
        have = false;
        if (top->have == top->need) {
            if (!AmlFold(top, fr->what, &v)) break;
            n--;
            have = true;
        }
    }

    if (st) kfree(st);
    if (ok) *out = v;
    return ok;
}

static bool AmlTruth(const AmlValue *v, const char *what, bool *out) {
    if (v->type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s: an If was handed %s to decide on, and only an "
                "integer decides anything here\n", what, AmlTypeName(v->type));
        return false;
    }
    *out = (v->integer != 0);
    return true;
}

/* Store's Target — §20.2.5.4. A local, an argument, the Debug object (which
 * discards, and touches nothing), or a namespace Name holding an integer. A
 * store into a Field or an OperationRegion is where AML reaches hardware, and
 * that is the boundary this evaluator does not cross. */
static bool AmlStoreTo(const uint8_t **p, const uint8_t *end, AmlFrame *fr,
                        const AmlValue *v) {
    if (*p >= end) {
        kprintf("[AML] %s: a Store names no target\n", fr->what);
        return false;
    }
    uint8_t t = **p;

    if (t >= Local0Op && t <= Local7Op) {
        (*p)++;
        fr->local[t - Local0Op] = *v;
        return true;
    }
    if (t >= Arg0Op && t <= Arg6Op) {
        (*p)++;
        fr->arg[t - Arg0Op] = *v;
        return true;
    }
    if (t == 0x00) { (*p)++; return true; }              /* NullName */
    if (t == ExtOpPrefix) {
        if ((size_t)(end - *p) >= 2 && (*p)[1] == ExtDebugOp) {
            *p += 2;
            return true;                                 /* Debug: discarded */
        }
        kprintf("[AML] %s: a Store names an extended target this evaluator "
                "does not write\n", fr->what);
        return false;
    }
    if (!aml_is_name_string_lead(t)) {
        kprintf("[AML] %s: opcode 0x%02x in target position is not a name\n",
                fr->what, t);
        return false;
    }

    aml_object_t *o = resolve_name(p, end, fr->scope, false, NULL);
    if (!o) {
        kprintf("[AML] %s: a Store names something this namespace does not "
                "hold\n", fr->what);
        return false;
    }
    if (o->type == AML_OBJ_FIELD || o->type == AML_OBJ_REGION) {
        kprintf("[AML] %s: stores into %s, which is %s — this evaluator does "
                "not touch hardware, so the method is abandoned here rather "
                "than half-run\n", fr->what, o->name, AmlTypeName(o->type));
        return false;
    }
    if (o->type != AML_OBJ_INTEGER && o->type != AML_OBJ_NAME &&
        o->type != AML_OBJ_UNINIT) {
        kprintf("[AML] %s: stores into %s, which is %s — overwriting it is a "
                "conversion this evaluator does not perform\n",
                fr->what, o->name, AmlTypeName(o->type));
        return false;
    }
    if (v->type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s: stores %s into %s, and only an integer store is "
                "written here\n", fr->what, AmlTypeName(v->type), o->name);
        return false;
    }
    o->type      = AML_OBJ_INTEGER;
    o->v.integer = v->integer;
    return true;
}

/*
 * Walk one method body. No C recursion: the block stack below holds every
 * If or Else body that was entered, and it grows on the heap with the nesting
 * the firmware wrote.
 *
 * `*returned` distinguishes "ran to its end" from "returned a value" — a
 * method that falls off its end has produced nothing, and saying it produced
 * zero is exactly the class of lie this file refuses to tell.
 */
static bool AmlRunBody(const uint8_t *body, uint32_t len, AmlFrame *fr,
                        AmlValue *result, bool *returned) {
    AmlBlock *blk  = NULL;
    uint32_t  room = 0, depth = 0;
    bool      ok   = true;

    *returned = false;
    blk = AmlStackRoom(blk, &room, 1, sizeof(*blk));
    if (!blk) {
        kprintf("[AML] %s: the heap has no room to walk this method's body\n",
                fr->what);
        return false;
    }
    blk[0].p   = body;
    blk[0].end = body + len;
    depth      = 1;

    while (depth > 0) {
        AmlBlock *b = &blk[depth - 1];
        if (b->p >= b->end) { depth--; continue; }
        uint8_t op = *b->p;

        if (op == NoopOp) { b->p++; continue; }

        if (op == ReturnOp) {
            b->p++;
            AmlValue v;
            if (!AmlEvalTermArg(&b->p, b->end, fr, &v)) { ok = false; break; }
            *result   = v;
            *returned = true;
            break;
        }

        if (op == StoreOp) {
            b->p++;
            AmlValue v;
            if (!AmlEvalTermArg(&b->p, b->end, fr, &v)) { ok = false; break; }
            if (!AmlStoreTo(&b->p, b->end, fr, &v))     { ok = false; break; }
            continue;
        }

        if (op == IfOp) {
            /* DefIfElse := IfOp PkgLength Predicate TermList DefElse — the
             * predicate is INSIDE the package, and the Else, when there is
             * one, follows the package. §20.2.5.3. */
            const uint8_t *q = b->p + 1;
            uint32_t cb = 0;
            uint32_t pklen = decode_pkglen(&q, b->end, &cb);
            if (!cb || pklen < cb) {
                kprintf("[AML] %s: an If has a malformed package length\n",
                        fr->what);
                ok = false; break;
            }
            const uint8_t *if_end = q + pklen;
            const uint8_t *pred   = q + cb;
            if (if_end > b->end) {
                kprintf("[AML] %s: an If runs past the end of its block\n",
                        fr->what);
                ok = false; break;
            }
            const uint8_t *pp = pred;
            AmlValue pv;
            if (!AmlEvalTermArg(&pp, if_end, fr, &pv)) { ok = false; break; }
            bool truth = false;
            if (!AmlTruth(&pv, fr->what, &truth))      { ok = false; break; }

            const uint8_t *take_start = truth ? pp : NULL;
            const uint8_t *take_end   = truth ? if_end : NULL;
            const uint8_t *after      = if_end;
            if (if_end < b->end && *if_end == ElseOp) {
                const uint8_t *e = if_end + 1;
                uint32_t ecb = 0;
                uint32_t epk = decode_pkglen(&e, b->end, &ecb);
                if (!ecb || epk < ecb) {
                    kprintf("[AML] %s: an Else has a malformed package "
                            "length\n", fr->what);
                    ok = false; break;
                }
                const uint8_t *e_end = e + epk;
                if (e_end > b->end) {
                    kprintf("[AML] %s: an Else runs past the end of its "
                            "block\n", fr->what);
                    ok = false; break;
                }
                if (!truth) { take_start = e + ecb; take_end = e_end; }
                after = e_end;
            }

            /* Before the stack may move under us. */
            b->p = after;
            if (take_start) {
                AmlBlock *grown = AmlStackRoom(blk, &room, depth + 1,
                                               sizeof(*blk));
                if (!grown) {
                    kprintf("[AML] %s: the heap has no room for this method's "
                            "nesting — the method is abandoned, not "
                            "guessed\n", fr->what);
                    ok = false; break;
                }
                blk = grown;
                blk[depth].p   = take_start;
                blk[depth].end = take_end;
                depth++;
            }
            continue;
        }

        kprintf("[AML] %s: opcode 0x%02x at byte %u is not one this evaluator "
                "runs%s — the method is abandoned rather than half-run\n",
                fr->what, op, (unsigned)(b->p - body),
                op == WhileOp ? " (a While loop)" : "");
        ok = false;
        break;
    }

    kfree(blk);
    return ok;
}

/* Run one method. `out` may be NULL when the caller wants no value — \_PIC
 * returns nothing and must not be judged for it. */
static bool AmlRunMethod(aml_object_t *m, const AmlValue *args, uint8_t argc,
                          AmlValue *out) {
    uint8_t want = (uint8_t)(m->v.method.flags & 0x7);
    if (want != argc) {
        kprintf("[AML] %s takes %u arguments and was offered %u — not run\n",
                m->name, want, argc);
        return false;
    }
    if (!m->v.method.aml || m->v.method.aml_len == 0) {
        kprintf("[AML] %s has an empty body\n", m->name);
        return false;
    }

    AmlFrame fr;
    memset(&fr, 0, sizeof(fr));
    fr.scope = m;
    fr.what  = m->name;
    for (uint8_t i = 0; i < argc; i++) fr.arg[i] = args[i];

    AmlValue r;
    bool returned = false;
    memset(&r, 0, sizeof(r));
    if (!AmlRunBody(m->v.method.aml, m->v.method.aml_len, &fr, &r, &returned))
        return false;
    if (out) {
        if (!returned) {
            kprintf("[AML] %s ran to its end without returning a value, and "
                    "no value is invented for it\n", m->name);
            return false;
        }
        *out = r;
    }
    return true;
}

/* The value of a named object, running it first if it is a no-argument
 * method. This is how _PRT, _CRS, _BBN and _SEG are asked: every one of them
 * is a Name on some boards and a Method on others. */
static bool AmlEvalNamed(aml_object_t *node, AmlValue *out) {
    if (node->type == AML_OBJ_METHOD)
        return AmlRunMethod(node, NULL, 0, out);
    return AmlValueOfNode(node, node->name, out);
}

/* =====================================================================
 * Root bridges, interrupt links and _PRT
 * ===================================================================== */

typedef struct AmlPrtEntry {
    uint16_t      device;        /* PCI device number: Address bits 31:16 */
    uint8_t       pin;           /* 0..3 — INTA..INTD, as _PRT numbers them */
    bool          direct;        /* Source was the integer zero */
    uint32_t      source_index;
    aml_object_t *link;          /* the PNP0C0F device, when not direct */
    bool          asked;         /* the link has been consulted once */
    bool          answered;
    AmlRoute      route;
} AmlPrtEntry;

typedef struct AmlPrtTable {
    struct AmlPrtTable *next;
    aml_object_t       *owner;   /* the Device whose _PRT this is */
    uint32_t            count;
    AmlPrtEntry        *entry;
} AmlPrtTable;

typedef struct AmlRootBridge {
    struct AmlRootBridge *next;
    aml_object_t         *node;
    uint16_t              segment;
    uint8_t               bus;
} AmlRootBridge;

static AmlRootBridge *g_root_bridges  = NULL;
static AmlPrtTable   *g_prt_tables    = NULL;
static bool           g_routing_tried = false;
static bool           g_pic_answered  = false;

/* Depth-first walk of the namespace with the pointers the objects already
 * carry — no stack of its own, and no recursion. */
static aml_object_t *AmlWalkNext(aml_object_t *node, aml_object_t *root) {
    if (node->child) return node->child;
    while (node && node != root) {
        if (node->sibling) return node->sibling;
        node = node->parent;
    }
    return NULL;
}

/*
 * The seven characters an EISA id integer spells. ACPI 6.5 §19.6.30 defines
 * EISAID as a compressed form: three five-bit letters biased by '@' followed
 * by four hexadecimal digits, laid out so the whole thing reads as a
 * little-endian DWord. Decoding it is how "PNP0A03" is recognised without a
 * magic constant standing in for the string.
 */
static void AmlEisaIdText(uint64_t id, char out[8]) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t b0 = (uint8_t)(id & 0xFFu);
    uint8_t b1 = (uint8_t)((id >> 8) & 0xFFu);
    uint8_t b2 = (uint8_t)((id >> 16) & 0xFFu);
    uint8_t b3 = (uint8_t)((id >> 24) & 0xFFu);
    out[0] = (char)(0x40 + ((b0 >> 2) & 0x1F));
    out[1] = (char)(0x40 + (uint8_t)(((b0 & 0x03u) << 3) | ((b1 >> 5) & 0x07u)));
    out[2] = (char)(0x40 + (b1 & 0x1F));
    out[3] = hex[(b2 >> 4) & 0x0F];
    out[4] = hex[b2 & 0x0F];
    out[5] = hex[(b3 >> 4) & 0x0F];
    out[6] = hex[b3 & 0x0F];
    out[7] = 0;
}

static bool AmlTextIs(const char *a, const char *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        if (a[i] != b[i]) return false;
    return true;
}

/* One id value against the seven characters of `id`. ACPI 6.5 §6.1.5 allows
 * both forms — the compressed EISA id integer and the string. */
static bool AmlIdTextIs(const AmlValue *v, const char *id) {
    if (v->type == AML_OBJ_INTEGER) {
        char text[8];
        AmlEisaIdText(v->integer, text);
        return AmlTextIs(text, id, 7);
    }
    if (v->type == AML_OBJ_STRING && v->bytes &&
        (uint64_t)(v->bytes_end - v->bytes) == 7)
        return AmlTextIs((const char *)v->bytes, id, 7);
    return false;
}

/* Does `dev`'s `seg` child name `id`? Read as the STATIC value §6.1.5
 * requires _HID and _CID to be; a method form is not run, because this
 * question is asked of EVERY Device in the namespace to find the handful
 * that are bridges, and running a method per device for it is a cost, and a
 * risk, out of all proportion to a form no shipping firmware uses. */
static bool AmlNamedIdIs(aml_object_t *dev, const char *seg, const char *id) {
    aml_object_t *n = AmlChild(dev, seg);
    if (!n) return false;
    if (n->type != AML_OBJ_INTEGER && n->type != AML_OBJ_STRING &&
        n->type != AML_OBJ_PACKAGE)
        return false;

    AmlValue v;
    if (!AmlValueOfNode(n, n->name, &v)) return false;
    if (v.type != AML_OBJ_PACKAGE) return AmlIdTextIs(&v, id);

    /* ACPI 6.5 §6.1.2 lets _CID hold one id or a package of them, listed
     * most specific first. Any of them naming the id asked for is a match. */
    const uint8_t *q = v.bytes;
    for (uint32_t i = 0; i < v.count && q && q < v.bytes_end; i++) {
        AmlValue e;
        if (!AmlReadOperand(&q, v.bytes_end, v.scope, NULL, false, n->name,
                            &e))
            return false;
        if (AmlIdTextIs(&e, id)) return true;
    }
    return false;
}

/* ‼ _CID COUNTS, AND ASKING ONLY _HID MISSED BRIDGES.
 *
 * ACPI 6.5 §6.1.2: a device whose _HID is vendor-specific declares what it
 * is COMPATIBLE with in _CID, and a root bridge carrying a vendor _HID with
 * _CID EISAID("PNP0A03") is an ordinary shape on shipping firmware. Read
 * through _HID alone, such a bridge is simply not found: every function
 * under it falls back to the byte at 0x3C, and the refusal printed says "no
 * ACPI root bridge describes segment 0 bus N" — which names the board as
 * silent when in fact this kernel did not recognise what it said. */
static bool AmlHidIs(aml_object_t *dev, const char *id) {
    return AmlNamedIdIs(dev, "_HID", id) || AmlNamedIdIs(dev, "_CID", id);
}

/*
 * An optional integer child. Absent is not an error and leaves `*out`
 * untouched — the caller states the default and why. Present but unreadable
 * IS an error: a device that answers "I cannot tell you my bus number" must
 * not be filed under bus zero.
 */
static bool AmlOptionalInteger(aml_object_t *dev, const char *seg,
                                uint64_t *out, bool *present) {
    *present = false;
    aml_object_t *n = AmlChild(dev, seg);
    if (!n) return true;
    AmlValue v;
    if (!AmlEvalNamed(n, &v)) return false;
    if (v.type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s.%s answered %s where an integer was expected\n",
                dev->name, n->name, AmlTypeName(v.type));
        return false;
    }
    *out     = v.integer;
    *present = true;
    return true;
}

/*
 * The current interrupt setting of one PCI Interrupt Link device, read out of
 * the resource template its _CRS returns.
 *
 * ACPI 6.5 §6.4.2.1, IRQ Descriptor: the two mask bytes carry one bit per
 * 8259 interrupt, and the optional third byte is the Information byte —
 * "Bit[0] Interrupt Mode: 1 = Edge Triggered, 0 = Level Triggered", "Bit[3]
 * Interrupt Polarity: 0 = Active-High, 1 = Active-Low", "Bit[4] Interrupt
 * Sharing". The same section: "If byte 3 is not included, High true,
 * edge-sensitive, non-shareable is assumed."
 *
 * ACPI 6.5 §6.4.3.6, Extended Interrupt Descriptor: "Bit[1] Interrupt Mode:
 * 1 = Edge-Triggered, 0 = Level-Triggered", "Bit[2] Interrupt Polarity:
 * 0 = Active-High, 1 = Active-Low", "Bit[3] Interrupt Sharing", followed by
 * the Interrupt Table Length and that many 32-bit interrupt numbers.
 */
static bool AmlIrqFromResources(const uint8_t *p, const uint8_t *end,
                                 uint32_t index, const char *who,
                                 AmlRoute *out) {
    while (p < end) {
        uint8_t tag = *p;

        if (tag == 0x79) break;                       /* End Tag — §6.4.2.9 */

        if ((tag & 0x80u) == 0) {                     /* small descriptor */
            uint32_t len  = (uint32_t)(tag & 0x07u);
            uint8_t  name = (uint8_t)((tag >> 3) & 0x0Fu);
            if (p + 1 + len > end) break;
            if (name == 0x04 && len >= 2) {           /* IRQ descriptor */
                /* SourceIndex counts the interrupts this descriptor offers.
                 * A configured link offers exactly one and every _PRT entry
                 * naming it asks for number 0, which is where the two
                 * readings of §6.2.13 — "which descriptor" and "which
                 * interrupt in it" — say the same thing. */
                uint32_t mask = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
                uint32_t seen = 0;
                for (uint32_t bit = 0; bit < 16; bit++) {
                    if (!(mask & (1u << bit))) continue;
                    if (seen++ != index) continue;
                    out->gsi = bit;
                    if (len >= 3) {
                        uint8_t f = p[3];
                        out->level      = ((f & 0x01u) == 0);
                        out->active_low = ((f & 0x08u) != 0);
                        out->shareable  = ((f & 0x10u) != 0);
                    } else {
                        out->level      = false;
                        out->active_low = false;
                        out->shareable  = false;
                    }
                    out->wiring_known = true;
                    out->named_by     = "the link's own _CRS";
                    return true;
                }
                if (seen == 0) {
                    kprintf("[AML] interrupt link %s holds no interrupt in "
                            "its _CRS: it is not configured, and configuring "
                            "it means writing the chipset's routing register "
                            "through _SRS, which this evaluator does not do\n",
                            who);
                    return false;
                }
                kprintf("[AML] interrupt link %s: its _CRS offers %u "
                        "interrupts and the routing entry asks for number "
                        "%u\n", who, seen, index);
                return false;
            }
            p += 1 + len;
            continue;
        }

        /* Large descriptor: the two bytes after the tag are its length. */
        if (p + 3 > end) break;
        uint32_t len = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        if (p + 3 + len > end) break;
        if (tag == 0x89) {                            /* Extended Interrupt */
            if (len < 2) break;
            uint8_t flags = p[3];
            uint8_t count = p[4];
            if (index >= count) {
                kprintf("[AML] interrupt link %s: its extended interrupt "
                        "descriptor carries %u numbers and the routing entry "
                        "asks for number %u\n", who, count, index);
                return false;
            }
            uint32_t off = 5u + 4u * index;
            if (off + 4u > 3u + len) break;
            out->gsi = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
                       ((uint32_t)p[off + 2] << 16) |
                       ((uint32_t)p[off + 3] << 24);
            out->level        = ((flags & 0x02u) == 0);
            out->active_low   = ((flags & 0x04u) != 0);
            out->shareable    = ((flags & 0x08u) != 0);
            out->wiring_known = true;
            out->named_by     = "the link's own _CRS";
            return true;
        }
        p += 3 + len;
    }

    kprintf("[AML] interrupt link %s: its _CRS holds no interrupt descriptor "
            "this kernel recognises\n", who);
    return false;
}

static bool AmlLinkRoute(aml_object_t *link, uint32_t index, AmlRoute *out) {
    if (!AmlHidIs(link, "PNP0C0F")) {
        kprintf("[AML] a routing entry names %s as its interrupt source, but "
                "that object does not declare _HID PNP0C0F — it is not a PCI "
                "interrupt link, and this kernel reads no other kind of "
                "source\n", link->name);
        return false;
    }
    if (!g_pic_answered) {
        kprintf("[AML] interrupt link %s is refused: this firmware never "
                "took \\_PIC, so the table it handed over describes the 8259s "
                "and the link's current setting is an ISA interrupt, not a "
                "global system interrupt\n", link->name);
        return false;
    }
    aml_object_t *crs = AmlChild(link, "_CRS");
    if (!crs) {
        kprintf("[AML] interrupt link %s declares no _CRS, so nothing says "
                "which interrupt it currently carries\n", link->name);
        return false;
    }
    AmlValue v;
    if (!AmlEvalNamed(crs, &v)) {
        kprintf("[AML] interrupt link %s: its _CRS is not something this "
                "evaluator can read without touching hardware — the lines "
                "behind it fall back to the weaker source\n", link->name);
        return false;
    }
    if (v.type != AML_OBJ_BUFFER) {
        kprintf("[AML] interrupt link %s: its _CRS answered %s where a "
                "resource template was expected\n",
                link->name, AmlTypeName(v.type));
        return false;
    }
    return AmlIrqFromResources(v.bytes, v.bytes_end, index, link->name, out);
}

/* One _PRT element: Package { Address, Pin, Source, SourceIndex } —
 * ACPI 6.5 §6.2.13. */
static bool AmlPrtElement(const uint8_t **p, const uint8_t *end,
                           aml_object_t *scope, const char *who,
                           AmlPrtEntry *e) {
    AmlValue row;
    if (!AmlReadOperand(p, end, scope, NULL, false, who, &row)) return false;
    if (row.type != AML_OBJ_PACKAGE) {
        kprintf("[AML] %s._PRT: an entry is %s where a package of four was "
                "expected\n", who, AmlTypeName(row.type));
        return false;
    }
    if (row.count < 4) {
        kprintf("[AML] %s._PRT: an entry declares %u fields; §6.2.13 gives it "
                "four\n", who, row.count);
        return false;
    }

    const uint8_t *q = row.bytes;
    AmlValue f[4];
    for (uint32_t i = 0; i < 4; i++)
        if (!AmlReadOperand(&q, row.bytes_end, row.scope, NULL, false, who,
                            &f[i]))
            return false;

    if (f[0].type != AML_OBJ_INTEGER || f[1].type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s._PRT: an entry's Address or Pin is not an integer\n",
                who);
        return false;
    }
    if (f[1].integer > 3u) {
        kprintf("[AML] %s._PRT: an entry names pin %u; §6.2.13 numbers them "
                "0 to 3 for INTA# to INTD#\n", who, (unsigned)f[1].integer);
        return false;
    }

    memset(e, 0, sizeof(*e));
    e->device = (uint16_t)((f[0].integer >> 16) & 0xFFFFu);
    e->pin    = (uint8_t)f[1].integer;

    if (f[3].type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s._PRT: an entry's Source Index is not an integer\n",
                who);
        return false;
    }
    e->source_index = (uint32_t)f[3].integer;

    if (f[2].node) {
        e->direct = false;
        e->link   = f[2].node;
    } else if (f[2].type == AML_OBJ_INTEGER && f[2].integer == 0) {
        /* §6.2.13: "If this field is the integer zero, then the interrupt is
         * allocated from the global interrupt pool" and Source Index "is the
         * global system interrupt number". */
        e->direct = true;
    } else {
        kprintf("[AML] %s._PRT: an entry's Source is neither zero nor a name\n",
                who);
        return false;
    }
    return true;
}

static AmlPrtTable *AmlPrtTableFor(aml_object_t *dev) {
    for (AmlPrtTable *t = g_prt_tables; t; t = t->next)
        if (t->owner == dev) return t;

    AmlPrtTable *t = (AmlPrtTable *)kmalloc(sizeof(*t));
    if (!t) {
        kprintf("[AML] the heap has no room to hold %s's routing table\n",
                dev->name);
        return NULL;
    }
    t->owner = dev;
    t->count = 0;
    t->entry = NULL;
    t->next  = g_prt_tables;
    g_prt_tables = t;

    aml_object_t *prt = AmlChild(dev, "_PRT");
    if (!prt) return t;                      /* no _PRT here — a real answer */

    AmlValue v;
    if (!AmlEvalNamed(prt, &v)) {
        kprintf("[AML] %s._PRT could not be read — every line it would have "
                "described falls back to the weaker source\n", dev->name);
        return t;
    }
    if (v.type != AML_OBJ_PACKAGE) {
        kprintf("[AML] %s._PRT answered %s where a package of entries was "
                "expected\n", dev->name, AmlTypeName(v.type));
        return t;
    }
    if (v.count == 0) {
        kprintf("[AML] %s._PRT is empty: this firmware describes no interrupt "
                "routing under it\n", dev->name);
        return t;
    }

    /* One allocation, sized by the number of entries the FIRMWARE declares.
     * Nothing here decides how many a board may have. */
    t->entry = (AmlPrtEntry *)kmalloc((size_t)v.count * sizeof(AmlPrtEntry));
    if (!t->entry) {
        kprintf("[AML] the heap has no room for %s's %u routing entries\n",
                dev->name, v.count);
        return t;
    }

    const uint8_t *q = v.bytes;
    uint32_t got = 0;
    while (got < v.count && q < v.bytes_end) {
        if (!AmlPrtElement(&q, v.bytes_end, v.scope, dev->name,
                           &t->entry[got]))
            break;
        got++;
    }
    t->count = got;
    if (got != v.count) {
        kprintf("[AML] %s._PRT declares %u entries and %u of them were read; "
                "the rest are refused, not filled in\n",
                dev->name, v.count, got);
    } else {
        kprintf("[AML] %s._PRT: %u routing entries read\n", dev->name, got);
    }
    return t;
}

/*
 * `*named` separates the two ways this can fail, and the difference decides
 * what the caller does next. "No entry for this device and pin" means the
 * table is silent and the bus above may still answer. "There IS an entry and
 * its link could not be read" means the table has spoken and this kernel
 * cannot follow it — rotating the pin and asking the bus above would then
 * answer a question nobody asked, with a number belonging to someone else.
 */
static bool AmlPrtLookup(AmlPrtTable *t, uint8_t device, uint8_t pin0,
                          AmlRoute *out, bool *named) {
    *named = false;
    if (!t || !t->entry) return false;
    for (uint32_t i = 0; i < t->count; i++) {
        AmlPrtEntry *e = &t->entry[i];
        if (e->device != device || e->pin != pin0) continue;
        *named = true;

        if (e->direct) {
            /* ‼ THE \_PIC GATE APPLIES HERE TOO, and applying it only to
             * link-sourced entries was half a rule. ACPI 6.5 §5.8.1: a board
             * that was never told the interrupt model hands over the table
             * it prepared for PIC mode, and in that table a Source of zero
             * means "the interrupt is allocated from the global interrupt
             * pool" of the 8259s — SourceIndex is an ISA interrupt behind
             * the PIRQ router, not a global system interrupt. Believing it
             * as a GSI is exactly the mistake the sentence printed at boot
             * says this kernel avoids. Refused, and the line falls back to
             * the byte at 0x3C, which carries the same PIC-mode information
             * already marked as the weakest source there is. */
            if (!g_pic_answered) {
                kprintf("[AML] %s._PRT names GSI %u for pin %u of device "
                        "0x%04x, but this firmware never took \\_PIC — that "
                        "number is an 8259 interrupt in a PIC-mode table, "
                        "not a global system interrupt\n",
                        t->owner->name, e->source_index, pin0, device);
                return false;
            }
            memset(out, 0, sizeof(*out));
            out->gsi          = e->source_index;
            out->wiring_known = false;
            out->named_by     = "the ACPI routing table";
            return true;
        }
        if (!e->asked) {
            e->asked    = true;
            e->answered = AmlLinkRoute(e->link, e->source_index, &e->route);
        }
        if (!e->answered) return false;
        *out = e->route;
        return true;
    }
    return false;
}

/*
 * Tell the platform which interrupt model this kernel runs, and read what it
 * then says about routing. Once; every call after the first is a no-op.
 *
 * WHEN, and why it is not a line in a boot list: the argument to \_PIC is the
 * interrupt model, so this cannot run before the machine has chosen its
 * controller — and \_PIC must be told before this kernel asks the platform
 * for ANY interrupt, because what the platform reports afterwards depends on
 * the answer. Those two facts name one instant: after irqchip_init, before
 * the first platform line is registered. acpi_sci_register is that instant —
 * the SCI is the first interrupt this kernel takes from the platform — and it
 * calls here. AmlRouteForPciPin calls here too, so a caller that somehow
 * arrives earlier still finds the question already asked rather than assumed.
 */
void AmlRoutingAdopt(void) {
    if (g_routing_tried) return;
    g_routing_tried = true;

    if (!g_loaded) {
        kprintf("[AML] no namespace was built, so there is no routing table "
                "to read — PCI interrupt lines keep the byte the firmware "
                "wrote for the 8259\n");
        return;
    }

    /* ACPI 6.5 §5.8.1, \_PIC: "This optional object is a control method that
     * OSPM must call to indicate the interrupt model being used ... Arg0: 0 -
     * PIC mode, 1 - APIC mode, 2 - SAPIC mode ... If the platform does not
     * implement the \_PIC method, OSPM assumes the platform is in PIC mode."
     *
     * It must be told BEFORE _PRT is read: a board that has not been told
     * hands over the table describing the 8259s, whose numbers are ISA
     * interrupts behind a PIRQ router and mean nothing as GSIs. */
    uint64_t model = (irqchip_get_type() == IRQCHIP_APIC) ? 1u : 0u;
    aml_object_t *pic = AmlChild(&g_root_obj, "_PIC");
    if (!pic) {
        kprintf("[AML] this firmware declares no \\_PIC. Per ACPI 6.5 §5.8.1 "
                "its routing table therefore describes PIC mode, whose "
                "numbers are 8259 interrupts behind the PIRQ router — no "
                "entry in it is read as a global system interrupt\n");
    } else if (pic->type != AML_OBJ_METHOD) {
        kprintf("[AML] \\_PIC is %s, and §5.8.1 makes it a control method — "
                "the interrupt model was not announced\n",
                AmlTypeName(pic->type));
    } else {
        AmlValue arg;
        memset(&arg, 0, sizeof(arg));
        arg.type    = AML_OBJ_INTEGER;
        arg.integer = model;
        if (AmlRunMethod(pic, &arg, 1, NULL)) {
            g_pic_answered = true;
            kprintf("[AML] \\_PIC(%u) — this kernel runs its %s, and the "
                    "firmware has been told so before its routing table is "
                    "read\n", (unsigned)model,
                    model ? "IO-APIC" : "8259 pair");
        } else {
            kprintf("[AML] \\_PIC could not be run to the end; the routing "
                    "table is still built, but no entry in it is read as a "
                    "global system interrupt, because the mode it describes "
                    "is no longer known\n");
        }
    }

    for (aml_object_t *n = AmlWalkNext(&g_root_obj, &g_root_obj); n;
         n = AmlWalkNext(n, &g_root_obj)) {
        if (n->type != AML_OBJ_DEVICE) continue;
        /* ACPI 6.5 §6.1.5 / Table 6.6: PNP0A03 is a PCI host bridge and
         * PNP0A08 a PCI Express one. Both carry a _PRT. */
        if (!AmlHidIs(n, "PNP0A03") && !AmlHidIs(n, "PNP0A08")) continue;

        uint64_t seg = 0, bbn = 0;
        bool have_seg = false, have_bbn = false;
        /* Named, not dropped in silence. A bridge that cannot state its own
         * segment or bus number is refused — filing it under bus 0 would
         * answer other buses' lines with its table — and the only output
         * without this sentence was whatever the evaluator happened to say
         * about the term it choked on, which does not name the bridge. */
        if (!AmlOptionalInteger(n, "_SEG", &seg, &have_seg)) {
            kprintf("[AML] %[E]root bridge %s cannot state its own segment "
                    "number, so nothing it says about routing can be filed "
                    "against a bus — refused%[D]\n", n->name);
            continue;
        }
        if (!AmlOptionalInteger(n, "_BBN", &bbn, &have_bbn)) {
            kprintf("[AML] %[E]root bridge %s cannot state its own bus "
                    "number — refused, rather than filed under bus 0 where "
                    "it would answer for somebody else's lines%[D]\n",
                    n->name);
            continue;
        }
        if (seg > 0xFFFFu || bbn > 0xFFu) {
            kprintf("[AML] root bridge %s names segment %lu bus %lu, which no "
                    "PCI address can hold\n", n->name,
                    (unsigned long)seg, (unsigned long)bbn);
            continue;
        }

        bool clash = false;
        for (AmlRootBridge *b = g_root_bridges; b; b = b->next) {
            if (b->segment == (uint16_t)seg && b->bus == (uint8_t)bbn) {
                kprintf("[AML] %[E]root bridges %s and %s both claim segment "
                        "%u bus %u — the second is refused, because one bus "
                        "cannot have two routing tables%[D]\n",
                        b->node->name, n->name, (unsigned)seg, (unsigned)bbn);
                clash = true;
                break;
            }
        }
        if (clash) continue;

        AmlRootBridge *rb = (AmlRootBridge *)kmalloc(sizeof(*rb));
        if (!rb) {
            kprintf("[AML] the heap has no room to record root bridge %s\n",
                    n->name);
            continue;
        }
        rb->node    = n;
        rb->segment = (uint16_t)seg;
        rb->bus     = (uint8_t)bbn;
        rb->next    = g_root_bridges;
        g_root_bridges = rb;

        kprintf("[AML] PCI root bridge %s: segment %u, bus %u%s\n",
                n->name, (unsigned)seg, (unsigned)bbn,
                have_bbn ? "" : " (it declares no _BBN; §6.5.5 makes that "
                                "bus 0)");
    }

    if (!g_root_bridges) {
        kprintf("[AML] this firmware's namespace holds no PCI root bridge "
                "(PNP0A03 or PNP0A08), so there is no routing table to read\n");
    }
}

/* One namespace Device under `parent` whose _ADR names this function.
 * ACPI 6.5 §6.1.1: _ADR for a PCI device is the device number in the high
 * word and the function number in the low word; 0xFFFF in the low word means
 * every function. */
static aml_object_t *AmlDeviceByAdr(aml_object_t *parent, uint8_t device,
                                     uint8_t function) {
    if (!parent) return NULL;
    uint64_t want_exact = ((uint64_t)device << 16) | function;
    uint64_t want_any   = ((uint64_t)device << 16) | 0xFFFFu;
    for (aml_object_t *c = parent->child; c; c = c->sibling) {
        if (c->type != AML_OBJ_DEVICE) continue;
        aml_object_t *adr = AmlChild(c, "_ADR");
        if (!adr || adr->type != AML_OBJ_INTEGER) continue;
        if (adr->v.integer == want_exact || adr->v.integer == want_any)
            return c;
    }
    return NULL;
}

bool AmlRouteForPciPin(uint16_t segment, uint8_t root_bus,
                       const AmlPciStep *path, uint32_t depth,
                       uint8_t intx_pin, AmlRoute *out) {
    if (!out || !path || depth == 0) return false;
    if (intx_pin < 1 || intx_pin > 4) return false;

    AmlRoutingAdopt();

    AmlRootBridge *rb = NULL;
    for (AmlRootBridge *b = g_root_bridges; b; b = b->next) {
        if (b->segment == segment && b->bus == root_bus) { rb = b; break; }
    }
    if (!rb) {
        kprintf("[AML] no ACPI root bridge describes segment %u bus %u, so "
                "nothing here can say where its interrupts go\n",
                (unsigned)segment, (unsigned)root_bus);
        return false;
    }

    /* The namespace Device that owns the bus each step sits on. owner[0] is
     * the root bridge; owner[i] is the bridge declared for step i-1, when the
     * firmware declared one — a bridge it did not declare simply has no _PRT
     * of its own, and the swizzle below is then the answer. */
    aml_object_t **owner =
        (aml_object_t **)kmalloc((size_t)depth * sizeof(*owner));
    if (!owner) {
        kprintf("[AML] the heap has no room to walk %u bridge levels\n", depth);
        return false;
    }
    owner[0] = rb->node;
    for (uint32_t i = 1; i < depth; i++)
        owner[i] = AmlDeviceByAdr(owner[i - 1], path[i - 1].device,
                                  path[i - 1].function);

    /*
     * Up the hierarchy, one bus at a time.
     *
     * PCI-to-PCI Bridge Architecture Specification 1.2 §9.1, Table 9-1: the
     * interrupt pins of a device on a bridge's secondary bus are connected to
     * the bridge's own INTx# pins rotated by the device number — INTA# of
     * device 1 arrives as the bridge's INTB#, and so on. So a bus whose own
     * bridge declares no _PRT answers by rotating the pin and asking the bus
     * above; a bus whose bridge DOES declare one is answered there and the
     * walk stops, which is what makes a non-standard board's own statement
     * beat the rotation rule.
     */
    uint8_t pin0 = (uint8_t)(intx_pin - 1);
    bool answered = false, named = false;
    for (uint32_t i = depth; i-- > 0; ) {
        AmlPrtTable *t = owner[i] ? AmlPrtTableFor(owner[i]) : NULL;
        if (t && AmlPrtLookup(t, path[i].device, pin0, out, &named)) {
            answered = true;
            break;
        }
        /* The table named this line and could not be followed: it has had its
         * say, and the rotation rule is not a second opinion to fall back
         * on. The refusal was printed where it happened. */
        if (named) break;
        if (i == 0) break;
        pin0 = (uint8_t)((pin0 + path[i].device) & 0x3u);
    }

    kfree(owner);
    if (!answered && !named) {
        kprintf("[AML] the routing table under segment %u bus %u names no "
                "entry for device %u pin INT%c#\n",
                (unsigned)segment, (unsigned)root_bus,
                (unsigned)path[depth - 1].device, 'A' + (intx_pin - 1));
    }
    return answered;
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
    /* Announced on the tag bus with the object count as payload. The reader
     * that binds to this namespace is AmlRouteForPciPin below, and it asks
     * its question later — the first time a driver has a PCI interrupt line
     * to arm — because only by then has the machine chosen the interrupt
     * model \_PIC must be told about. */
    uint32_t cnt = g_pool_used;
    TouchPublish("aml:ready", &cnt, sizeof(cnt));
    return AML_OK;
}
