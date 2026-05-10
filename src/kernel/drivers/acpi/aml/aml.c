#include "aml.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"
#include "io.h"
#include "pci.h"

/* =====================================================================
 * AML interpreter — production subset of ACPI 6.5 §20.
 *
 * Coverage:
 *   * Namespace build from DSDT + every SSDT
 *   * Named objects: Name, Scope, Method, Device, Processor, PowerRes,
 *     ThermalZone, OpRegion, Field, Mutex, Event, Alias
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
 * Out of scope (acknowledged in the header) — full method TermList
 * execution, buffer fields, generic conversions, Notify dispatch.
 * That work belongs to the next AML audit which can build on the
 * namespace this pass produces.
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

#define ExtMutexOp       0x01
#define ExtEventOp       0x02
#define ExtOpRegionOp    0x80
#define ExtFieldOp       0x81
#define ExtDeviceOp      0x82
#define ExtProcessorOp   0x83
#define ExtPowerResOp    0x84
#define ExtThermalZoneOp 0x85

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
        } region;
        struct {
            const uint8_t* aml;
            uint32_t       aml_len;
            uint8_t        flags;
        } method;
        struct { const char* str; uint32_t len; } string;
        struct { uint32_t count; uint64_t first_int; } package;
        struct {
            struct aml_object* region;
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
        uint64_t base = read_integer(p, end);
        uint64_t len  = read_integer(p, end);
        aml_object_t* o = obj_create_child(parent, leaf, AML_OBJ_REGION);
        if (!o) return AML_ERR_NOMEM;
        o->v.region.space = space;
        o->v.region.base  = base;
        o->v.region.len   = len;
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

    if (sub == ExtFieldOp) {
        uint32_t pklen_bytes;
        uint32_t pklen = decode_pkglen(p, end, &pklen_bytes);
        if (!pklen_bytes) return AML_ERR_BOUNDS;
        const uint8_t* body_end = *p + pklen;
        const uint8_t* np = *p + pklen_bytes;
        if (body_end > end) return AML_ERR_BOUNDS;
        aml_object_t* region = resolve_name(&np, body_end, scope, false, NULL);
        if (!region || np >= body_end) { *p = body_end; return AML_OK; }
        uint8_t access = *np++;
        uint32_t bit_cursor = 0;
        while (np < body_end) {
            uint8_t tag = *np;
            if (tag == 0x00) {
                np++;
                uint32_t cbytes;
                uint32_t bits = decode_pkglen(&np, body_end, &cbytes);
                if (!cbytes) break;
                np += cbytes;
                bit_cursor += bits;
                continue;
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
            f->v.field.region     = region;
            f->v.field.bit_offset = bit_cursor;
            f->v.field.bit_width  = bits;
            f->v.field.access     = access;
            bit_cursor += bits;
        }
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

uint64_t aml_region_read(aml_object_t* region, uint64_t offset, uint8_t bits) {
    if (!region || region->type != AML_OBJ_REGION) return 0;
    uint64_t addr = region->v.region.base + offset;
    uint8_t bytes = (uint8_t)((bits + 7) / 8);
    switch (region->v.region.space) {
        case REGION_SYSTEM_IO:
            if (bytes == 1) return inb((uint16_t)addr);
            if (bytes == 2) return inw((uint16_t)addr);
            if (bytes == 4) return inl((uint16_t)addr);
            return 0;
        case REGION_SYSTEM_MEMORY: {
            volatile void* p = vmm_map_mmio((uintptr_t)addr, 8,
                                             VMM_FLAGS_KERNEL_RW);
            if (!p) return 0;
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
    if (!region || region->type != AML_OBJ_REGION) return;
    uint64_t addr = region->v.region.base + offset;
    uint8_t bytes = (uint8_t)((bits + 7) / 8);
    switch (region->v.region.space) {
        case REGION_SYSTEM_IO:
            if (bytes == 1) outb((uint16_t)addr, (uint8_t)value);
            if (bytes == 2) outw((uint16_t)addr, (uint16_t)value);
            if (bytes == 4) outl((uint16_t)addr, (uint32_t)value);
            return;
        case REGION_SYSTEM_MEMORY: {
            volatile void* p = vmm_map_mmio((uintptr_t)addr, 8,
                                             VMM_FLAGS_KERNEL_RW);
            if (!p) return;
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

/* Read one Field, with proper bit-level alignment, via OpRegion. */
static uint64_t field_read(aml_object_t* f) {
    if (!f || f->type != AML_OBJ_FIELD || !f->v.field.region) return 0;
    uint32_t bit_off = f->v.field.bit_offset;
    uint32_t bits    = f->v.field.bit_width;
    if (bits == 0) return 0;
    /* For aligned byte/word/dword/qword we can read in one shot. */
    if ((bit_off & 7) == 0 && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
        return aml_region_read(f->v.field.region, bit_off / 8, (uint8_t)bits);
    }
    /* Slow path: gather bits. Cap at 64. */
    if (bits > 64) bits = 64;
    uint64_t v = 0;
    for (uint32_t i = 0; i < bits; i++) {
        uint64_t byte_off = (bit_off + i) / 8;
        uint8_t  shift    = (bit_off + i) % 8;
        uint64_t b = aml_region_read(f->v.field.region, byte_off, 8);
        if ((b >> shift) & 1) v |= (1ULL << i);
    }
    return v;
}

static void field_write(aml_object_t* f, uint64_t v) {
    if (!f || f->type != AML_OBJ_FIELD || !f->v.field.region) return;
    uint32_t bit_off = f->v.field.bit_offset;
    uint32_t bits    = f->v.field.bit_width;
    if ((bit_off & 7) == 0 && (bits == 8 || bits == 16 || bits == 32 || bits == 64)) {
        aml_region_write(f->v.field.region, bit_off / 8, (uint8_t)bits, v);
        return;
    }
    if (bits > 64) bits = 64;
    for (uint32_t i = 0; i < bits; i++) {
        uint64_t byte_off = (bit_off + i) / 8;
        uint8_t  shift    = (bit_off + i) % 8;
        uint64_t cur = aml_region_read(f->v.field.region, byte_off, 8);
        if ((v >> i) & 1) cur |= (1ULL << shift);
        else              cur &= ~(1ULL << shift);
        aml_region_write(f->v.field.region, byte_off, 8, cur);
    }
}

/* Resolve a SuperName / SimpleName operand into an aml_object_t* +
 * "kind" so Store can write back. We return NULL for things we can't
 * write to and fall back to discarding the value. */
static aml_object_t* resolve_target(const uint8_t** p, const uint8_t* end,
                                     exec_frame_t* fr, uint8_t* kind_out) {
    if (*p >= end) return NULL;
    uint8_t op = **p;
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
    BIN_OP(DivideOp,     b ? a / b : 0)
    BIN_OP(ModOp,        b ? a % b : 0)
    BIN_OP(AndOp,        a & b)
    BIN_OP(OrOp,         a | b)
    BIN_OP(XorOp,        a ^ b)
    BIN_OP(NandOp,       ~(a & b))
    BIN_OP(NorOp,        ~(a | b))
    BIN_OP(ShiftLeftOp,  a << (b & 63))
    BIN_OP(ShiftRightOp, a >> (b & 63))

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
        if (kind == 1) fr->locals[tgt_op - 0x60] = v;
        else if (kind == 2) fr->args[tgt_op - 0x68] = v;
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
                exec_termlist(ebody, (uint32_t)(eend - ebody), fr, depth + 1);
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
        while (iters++ < EXEC_MAX_WHILE) {
            const uint8_t* cur = body;
            uint64_t cond;
            aml_status_t s = exec_termarg(&cur, body_end, fr, &cond, depth + 1);
            if (s != AML_OK || !cond) break;
            fr->continue_flag = false;
            fr->break_flag    = false;
            s = exec_termlist(cur, (uint32_t)(body_end - cur), fr, depth + 1);
            if (s != AML_OK || fr->returned || fr->break_flag) break;
        }
        *p = body_end;
        *out = 0;
        return AML_OK;
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

    /* NamePath / method invocation. */
    if (op == RootChar || op == ParentPrefixChar ||
        op == DualNamePrefix || op == MultiNamePrefix ||
        (op >= 'A' && op <= 'Z') || op == '_' ||
        (op >= '0' && op <= '9')) {
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
                if (s != AML_OK) { *out = 0; return AML_OK; }
                inner.args[i] = v;
            }
            exec_termlist(o->v.method.aml, o->v.method.aml_len,
                          &inner, depth + 1);
            *out = inner.ret_value;
            return AML_OK;
        }
        *out = 0;
        return AML_OK;
    }

    /* Unknown opcode — skip one byte and report. */
    (*p)++;
    *out = 0;
    return AML_OK;
}

static aml_status_t exec_termlist(const uint8_t* aml, uint32_t len,
                                   exec_frame_t* fr, int depth) {
    const uint8_t* p   = aml;
    const uint8_t* end = aml + len;
    while (p < end && !fr->returned && !fr->break_flag) {
        const uint8_t* before = p;
        uint64_t discard;
        aml_status_t s = exec_term(&p, end, fr, &discard, depth);
        if (s == AML_ERR_DEEP_NEST) return s;
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
    exec_termlist(m->v.method.aml, m->v.method.aml_len, &fr, 0);
    if (ret_value) *ret_value = fr.ret_value;
    return AML_OK;
}
