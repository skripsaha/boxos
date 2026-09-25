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
/* ‼ THE EVALUATOR OWNS NO DOOR OF ITS OWN. These three headers are here so
 * that an OperationRegion is reached through the subsystem that already
 * governs that address space, and never through a port write or a page table
 * entry this file made for itself:
 *   pci.h  — pci.c is the only holder of 0xCF8/0xCFC and of the ECAM window
 *   vmm.h  — vmm_map_mmio / vmm_unmap_mmio, read-only and for one field read
 *   io.h   — the x86 I/O instructions
 * See AmlRegionRead, which is the only function below that calls any of
 * them, and the window that is the only way to reach it. */
#include "pci.h"
#include "vmm.h"
#include "io.h"
/* e820.h, for one question and one only: does the firmware's OWN memory map
 * call a physical range ACPI NVS. That is the whole of what lets \_PIC's one
 * store land — see AmlNvsHolds — and nothing else in this file looks at the
 * memory map. */
#include "e820.h"

/* =====================================================================
 * AML table reader — the namespace half of ACPI 6.5 §20, plus the narrow
 * evaluator that asks it where a PCI interrupt goes.
 *
 * There is no general executor here and none is missing; see aml.h for the
 * boundary and the reasons, and the long note above THE NARROW EVALUATOR
 * below for how each half of it is enforced by construction.
 *
 * TWO PASSES, AND THE FIRST ONE TOUCHES NOTHING.
 *
 * Pass one — everything above the evaluator — builds the namespace:
 *   * DSDT + every SSDT
 *   * Named objects: Name, Scope, Method, Device, Processor, PowerRes,
 *     ThermalZone, OpRegion, Field, IndexField, BankField, Mutex,
 *     Event, Alias. Every other DEFINED extended opcode is stepped
 *     over exactly, never rescanned byte by byte.
 *   * Data types: Integer (Zero/One/Ones + Byte/Word/DWord/QWord prefix),
 *     String, Buffer, Package
 *   * Lookup with C-string paths (`\_SB.PCI0._INI`) including relative
 *     forms with parent-prefix walk
 * A Method is RECORDED here and not run: its flags and the byte range of
 * its TermList are kept so the object is a faithful picture of what the
 * firmware declared. An OperationRegion is recorded with its address space,
 * base and length, and each Field with its bit offset, width and access
 * rule. Nothing in this pass performs an I/O, MMIO or PCI-config access.
 *
 * Pass two — the evaluator — runs one of five objects, on demand, to answer
 * where a PCI interrupt pin goes, and it is the only part of this file that
 * reaches an address space. It READS three of them. It WRITES in one, and
 * only at an address this board's own memory map calls ACPI NVS, and only
 * inside a \_PIC that a dry walk proved can be run from beginning to end —
 * see THE NARROW GATE near the bottom of this file.
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

/* Statement opcodes — ACPI 6.5 §20.2.5.3. Both passes need them: the
 * evaluator RUNS these, and the namespace walk has to measure one standing at
 * TermObj level rather than resynchronise through its body. */
#define IfOp             0xA0
#define ElseOp           0xA1
#define WhileOp          0xA2
#define NoopOp           0xA3
#define ReturnOp         0xA4
#define BreakOp          0xA5
#define ContinueOp       0x9F
#define BreakPointOp     0xCC

/* ===========================================================
 * Namespace data structures
 * =========================================================== */

struct aml_object {
    char        name[5];
    aml_obj_type_t type;
    struct aml_object* parent;
    struct aml_object* child;
    struct aml_object* sibling;

    /* ‼ HAS AN OPERATOR ALREADY SAID WHAT THIS OBJECT IS, AND HAS A SECOND
     * ONE ALREADY BEEN TURNED AWAY.
     *
     * `declared` is set by the operators of §20.2.5.2 that give a name a
     * VALUE or a BODY — DefName and DefMethod. DefScope does not set it: it
     * opens a name rather than declaring one (§19.6.120), and so does the
     * placeholder resolve_name leaves for a path it has not seen declared
     * yet, so both stay open for the real declaration to fill.
     *
     * It exists because this pass walks BOTH sides of an If, and the two
     * sides may declare the same name. See parse_one's NameOp and
     * handle_scope_method for what that costs and which side wins.
     * `collided` keeps the refusal to one line per object. */
    bool        declared;
    bool        collided;

    union {
        uint64_t integer;
        struct { const uint8_t* bytes; uint32_t len; } buffer;
        struct {
            uint8_t  space;
            uint64_t base;
            uint64_t len;
            /* ‼ WHETHER THE FIRMWARE'S OWN NUMBERS ARE IN HAND YET.
             *
             * §20.2.5.2 makes RegionOffset and RegionLen TermArgs, and half
             * the boards in the world write a NAME there rather than a
             * literal: `OperationRegion (RCRB, SystemMemory, \\RCBA, 0x4000)`
             * is the Intel ICH/PCH form and AMD's AGESA writes its own. The
             * parse pass has no frame to evaluate a term in, so it keeps the
             * BYTES and leaves these two false — the number is still owed.
             *
             * It is a flag because the alternative is worse than a flag: a
             * base that is recorded as 0 because nobody could read it is
             * INDISTINGUISHABLE from a base the firmware really wrote as 0,
             * and reading it means physical page zero, I/O port zero or a
             * function's Vendor ID answering as an interrupt number, under
             * the routing table's own signature. AmlRegionArm turns the
             * bytes into the number, once, before the first access; a region
             * it cannot resolve is refused aloud and never read. */
            bool     base_known;
            bool     len_known;
            const uint8_t* base_term;
            const uint8_t* base_term_end;
            const uint8_t* len_term;
            const uint8_t* len_term_end;
            /* The scope those bytes resolve names against — where the
             * OperationRegion was DECLARED, which is not always where its
             * name landed: `OperationRegion (\\_SB.PCI0.P40C, ...)` declares
             * under PCI0 and resolves its operands here. */
            struct aml_object* declared_in;
            /* ‼ Said once, the first time anything reads through it. On a
             * board where the routing does not add up, the line this flag
             * gates is the only account of WHERE the firmware reached — see
             * AmlRegionRead. */
            bool     said;
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

/* Statement and expression terms that stood at TermList level, where §20.2.5.1
 * allows them and this pass declares nothing. Counted because the number is
 * how much of a firmware's tables is control flow rather than declaration, and
 * because it used to be the number of places the walk resynchronised one byte
 * at a time. Said once, with the number, at the end of the load. */
static uint32_t      g_parse_stmts    = 0;

/* How many of those statements had a body and did NOT have it walked, because
 * where the body begins was not known: a predicate this pass could not
 * measure, or one whose length it could only guess at. The summary at the end
 * of the load says both numbers, because "every body was walked" was a claim
 * this counter is the exception to. */
static uint32_t      g_parse_unwalked = 0;

/* How many terms were stepped over on a length that was ASSUMED rather than
 * measured — see AmlSkipRun's AML_SKIP_GUESS. Said in full once, counted
 * always, because a number that is only ever printed once is a number nobody
 * can compare between two boards. */
static uint32_t      g_parse_guessed  = 0;

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

/*
 * One NameString, resolved against `scope`.
 *
 * ‼ A NameString IS ALLOWED TO NAME NO LEAF, AND THAT WAS READ AS A LEAF.
 *
 * ACPI 6.5 §20.2.2 encodes it as
 *
 *     NameString  := <RootChar NamePath> | <PrefixPath NamePath>
 *     PrefixPath  := Nothing | <'^' PrefixPath>
 *     NamePath    := NameSeg | DualNamePath | MultiNamePath | NullName
 *     NullName    := 0x00
 *
 * so `Scope (\\)` is two bytes — RootChar then NullName — and names the root
 * itself with nothing under it. What stood here returned the base object for
 * that form and left `leaf_out` UNTOUCHED, so every caller that declares a
 * child went on to declare one named from four uninitialised bytes of its own
 * C stack, and parsed the whole block's TermList into it. Measured on the
 * i440fx firmware this desk boots: the root grew a junk object holding the
 * firmware's debug port (its OperationRegion, its field and DBUG) and a
 * second holding \\_S3, \\_S4 and \\_S5 — objects that exist, under a name
 * nothing can ever look up.
 *
 * So `leaf_out` is emptied FIRST and written only when a leaf was really
 * parsed. `leaf_out[0] == 0` on return means "the object returned is what
 * this NameString names"; a declaration that needs a leaf says so aloud.
 *
 * The upward walk is §5.3's, and it is narrower than walking to the root for
 * anything relative: "The namespace search rules only apply to single NameSeg
 * paths, which is a relative namespace path. For those relative name paths
 * that contain multiple NameSegs or Parent Prefixes, '^', the search rules do
 * not apply." A path opened with RootChar is absolute and gets no search
 * either — it is the path the firmware wrote or it is nothing.
 */
static aml_object_t* resolve_name(const uint8_t** p, const uint8_t* end,
                                    aml_object_t* scope, bool create,
                                    char leaf_out[4]) {
    if (leaf_out) { leaf_out[0] = 0; leaf_out[1] = 0;
                    leaf_out[2] = 0; leaf_out[3] = 0; }
    if (*p >= end) return NULL;
    aml_object_t* base = scope;
    bool rooted   = false;
    bool prefixed = false;
    if (**p == RootChar) { base = &g_root_obj; rooted = true; (*p)++; }
    while (*p < end && **p == ParentPrefixChar) {
        /* ACPI 6.5 §5.3: "An attempt to access names in the parent of the
         * root will result in the name not being found." Stopping at the
         * root instead — which is what stood here — resolves the rest of the
         * path against the root and can hand back a REAL object that is not
         * the one named, which is worse than not finding it. */
        if (!base->parent) {
            kprintf("[AML] a name path resolved from %s asks for the parent "
                    "of the root; §5.3 makes that a name not found\n",
                    scope ? scope->name : "the root");
            return NULL;
        }
        base = base->parent;
        prefixed = true;
        (*p)++;
    }
    if (*p >= end) return base;

    int segs = 1;
    uint8_t op = **p;
    if (op == 0) { (*p)++; return base; }          /* NullName — names `base` */
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

    /* §5.3, decided once for the whole path rather than per segment. */
    bool search_rules = (!rooted && !prefixed && segs == 1);

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
        if (!next && !create && search_rules) {
            for (aml_object_t* up = base->parent; up; up = up->parent) {
                next = obj_find_child(up, seg);
                if (next) break;
            }
        }
        if (!next) {
            if (create && i < segs - 1) {
                /* An intermediate segment this pass never saw declared.
                 *
                 * ACPI 6.5 §5.3 states the compiler's end of this: "Object
                 * XYZ must already exist for the ABCD object to be created.
                 * If XYZ does not exist, that will cause a fatal error ...
                 * If any other Nameseg along the Namepath does not exist, it
                 * is a fatal error." A LOADER stands at the other end, and
                 * this one has already admitted (aml.h) that a declaration
                 * nested behind control flow can be missed by this pass —
                 * so "XYZ does not exist" here means "this pass did not see
                 * it", not "the firmware never wrote it". Dropping the whole
                 * branch would lose objects the board really declared.
                 *
                 * What is created carries the name the FIRMWARE wrote, at
                 * the path the firmware wrote it: the leaf resolves there
                 * afterwards, and a Device or Method declaration for the
                 * same name later upgrades this placeholder in place. It is
                 * said aloud because a name that resolves through a
                 * placeholder is the first thing to know when the object
                 * under it turns out to hold nothing. */
                kprintf("[AML] a name path under %s passes through %c%c%c%c, "
                        "which this pass never saw declared — recorded as an "
                        "open scope so what the firmware hung beneath it can "
                        "still be found\n",
                        cur->name, seg[0], seg[1], seg[2], seg[3]);
                next = obj_create_child(cur, seg, AML_OBJ_SCOPE);
                if (!next) return NULL;
            } else return NULL;
        }
        cur = next;
    }
    return cur;
}

/*
 * A declaration must name something to declare. §20.2.2 lets a NameString
 * end in NullName, which names the scope it is resolved against — legal for
 * DefScope, which opens that scope, and for no other operator in §20.2.5.2.
 * Refused aloud rather than declaring an object under a name the firmware
 * never wrote. What the caller does with the stream afterwards depends on
 * whether it could measure the term: an operator carrying a PkgLength steps
 * past it and returns AML_REFUSED, and one without a length has nothing to
 * step past and leaves the TermList to resynchronise.
 */
static bool AmlLeafNamed(const char leaf[4], const char *what) {
    if (leaf[0]) return true;
    kprintf("[AML] a %s declaration's name ends in NullName, which names its "
            "own scope and nothing to declare — refused\n", what);
    return false;
}

/*
 * ‼ BOTH SIDES OF AN If MAY DECLARE THE SAME NAME, AND ONLY ONE OF THEM CAN
 *   BE THE ONE THE MACHINE FINDS.
 *
 * This pass has no frame to evaluate a predicate, so it walks both bodies —
 * which is how `If (PICM) { Name (_PRT, <APIC GSIs>) } Else { Name (_PRT,
 * <PIC link entries>) }`, the shape every Intel reference ASL writes, puts
 * TWO _PRTs under one device. What stood here let both land, and the LAST one
 * won twice over: obj_create_child PREPENDS and obj_find_child answers with
 * the first match, so the Else side shadowed the If side; and a _PRT or _CRS
 * declared as a METHOD on both sides did not even shadow — handle_scope_method
 * found the existing object and rewrote its body with the Else one. Either
 * way AmlChild(dev, "_PRT") handed back the PIC-mode table to a kernel that
 * had just told the firmware \_PIC(APIC), and every function on that bridge
 * would have been armed from 8259 line numbers standing where global system
 * interrupts belong.
 *
 * A static walk cannot choose a branch, so it must not pretend to: THE FIRST
 * DECLARATION WINS, and a second one for the same name in the same scope is
 * refused ALOUD. First is the If side, and for the shape that matters that is
 * a default with a reason behind it rather than a preference: the predicate
 * of `If (PICM)` is the value \_PIC was given, AmlRoutingAdopt gives it the
 * model the interrupt controller is ACTUALLY running, and on a machine
 * running its IO-APIC — which is every machine this kernel has been on that
 * has one — the If side is the branch the firmware itself would take. The
 * refusal names the object, so a board where the other side was the right one
 * says so on its own console instead of routing quietly from the wrong table.
 */
static bool AmlFirstDeclarationWins(aml_object_t* o, const char* what) {
    if (!o->declared) return true;
    if (!o->collided) {
        o->collided = true;
        kprintf("[AML] %[E]%s under %s is declared a second time, as a %s — "
                "which is what the two sides of an If look like to a pass "
                "that cannot evaluate the predicate. The FIRST declaration "
                "stands and this one is refused, because the first is the If "
                "side: where the firmware branches on \\_PIC's argument, this "
                "kernel is the one that supplies it, and it supplies the "
                "model its interrupt controller is running%[D]\n",
                o->name, o->parent ? o->parent->name : "the root", what);
    }
    return false;
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

/* The single-byte expression opcodes of §20.2.5.4. A TermObj may be one of
 * them — a method called for its effect, or an operator whose Target is the
 * point of it — and a DATA prefix may not. The distinction is what stops a
 * resync from swallowing a literal as though it were a statement. */
static bool aml_is_expression_op(uint8_t op) {
    switch (op) {
    case StoreOp: case RefOfOp: case AddOp: case ConcatOp: case SubtractOp:
    case IncrementOp: case DecrementOp: case MultiplyOp: case DivideOp:
    case ShiftLeftOp: case ShiftRightOp: case AndOp: case NandOp:
    case OrOp: case NorOp: case XorOp: case NotOp: case DerefOfOp:
    case ModOp: case SizeOfOp: case IndexOp: case LAndOp: case LOrOp:
    case LNotOp: case LEqualOp: case LGreaterOp: case LLessOp:
        return true;
    default:
        return false;
    }
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

/* Defined with the evaluator below. Both halves of this file grow their
 * stacks through the one function, so "it grows with what the firmware
 * wrote, and the heap is what ends it" is one mechanism and not two. */
static void *AmlStackRoom(void *base, uint32_t *room, uint32_t want,
                           uint32_t item);

/*
 * ‼ STEPPING OVER AN OPERAND DOES NOT DESCEND THE C STACK EITHER.
 *
 * What stood here was `if (depth > AML_MAX_SKIP_DEPTH)` — a 16 that no
 * specification names. Past it this pass gave up MEASURING an operand, and
 * the TermList then fell back to the byte-at-a-time resync that aml.h calls
 * a hazard by name: declarations fabricated out of misaligned operand bytes,
 * inside a block whose real declarations are then lost.
 *
 * Skipping is strictly left to right — every item is consumed where the
 * previous one ended — so what a nested operand needs is not a stack of
 * POSITIONS, only a list of what is still OWED: another TermArg, another
 * SuperName, or the two bytes of an Acquire's timeout. That list is one byte
 * per item, it grows on the heap with the nesting the firmware wrote, and it
 * is reused across the whole parse rather than allocated per operand. There
 * is no ceiling on it; what ends the growth is kmalloc saying no.
 */
typedef enum {
    AML_SKIP_TERM  = 0,     /* one TermArg   §20.2.5 */
    AML_SKIP_SUPER = 1,     /* one SuperName §20.2.2 */
    AML_SKIP_WORD  = 2,     /* two raw bytes: Acquire's Timeout */
} AmlSkipKind;

static uint8_t*  g_skip_owed = NULL;
static uint32_t  g_skip_room = 0;

static bool AmlSkipOwe(uint32_t* n, uint8_t kind) {
    uint8_t* grown = (uint8_t*)AmlStackRoom(g_skip_owed, &g_skip_room,
                                            *n + 1, 1);
    if (!grown) {
        kprintf("[AML] the heap has no room to measure an operand nested "
                "this deep — the term is refused rather than half-measured\n");
        return false;
    }
    g_skip_owed = grown;
    g_skip_owed[(*n)++] = kind;
    return true;
}

/* The parse pass is over; the list it grew is not the namespace. */
static void AmlSkipForget(void) {
    if (g_skip_owed) kfree(g_skip_owed);
    g_skip_owed = NULL;
    g_skip_room = 0;
}

/*
 * Step over one TermArg (§20.2.5) or one SuperName (§20.2.2) whose value
 * this pass does not need. The namespace builder has no executor frame, so
 * an operand it cannot measure is a refusal — never a guess that leaves *p
 * inside the operand bytes for the next TermObj scan to chew on.
 *
 * ‼ AND A GUESS SAYS SO INSTEAD OF PASSING FOR A MEASUREMENT. A NameString
 * in TermArg position is a method invocation when the namespace says it is,
 * and MethodInvocation drags argc TermArgs along behind it (§20.2.5). A name
 * this pass has not SEEN declared yet — a forward reference, which shipping
 * tables write freely — cannot be asked how many, so the walk below assumes
 * none and the cursor it leaves may stand in the middle of the term. That
 * cursor used to be handed back as success: the caller entered a body from a
 * byte inside a predicate, and the resync fabricated declarations out of
 * operand bytes. AML_SKIP_GUESS is that case, told apart from both the
 * measurement and the failure, so that a caller for whom the difference
 * decides something can see it — the note on skip_term_arg below says which
 * callers those are and which can still act on a guess.
 */
typedef enum {
    AML_SKIP_NO    = 0,      /* these bytes are not a term this pass reads  */
    AML_SKIP_YES   = 1,      /* measured exactly; *p stands past the term   */
    AML_SKIP_GUESS = 2,      /* a name in it is not in the namespace YET    */
} AmlSkipHow;

static AmlSkipHow AmlSkipRun(const uint8_t** p, const uint8_t* end,
                              aml_object_t* scope, uint8_t first) {
    uint32_t n = 0;
    bool     guessed = false;
    if (!AmlSkipOwe(&n, first)) return AML_SKIP_NO;

    while (n) {
        uint8_t kind = g_skip_owed[--n];

        if (kind == AML_SKIP_WORD) {           /* Timeout := WordData */
            if ((size_t)(end - *p) < 2) return AML_SKIP_NO;
            *p += 2;
            continue;
        }
        if (*p >= end) return AML_SKIP_NO;
        uint8_t op = **p;

        if (kind == AML_SKIP_SUPER) {
            /* Target := SuperName | NullName. */
            if (op == 0x00) { (*p)++; continue; }              /* NullName  */
            if (op >= 0x60 && op <= 0x6E) { (*p)++; continue; }/* Local/Arg */
            if (op == ExtOpPrefix) {
                /* The only SuperName under the prefix is DebugObj. */
                if ((size_t)(end - *p) < 2 || (*p)[1] != ExtDebugOp)
                    return AML_SKIP_NO;
                *p += 2;
                continue;
            }
            if (op != RefOfOp && op != DerefOfOp && op != IndexOp) {
                if (!aml_is_name_string_lead(op)) return AML_SKIP_NO;
                if (!skip_name_string(p, end)) return AML_SKIP_NO;
                continue;
            }
            /* RefOf, DerefOf and Index in SuperName position are TermArgs,
             * and the same byte is read again as one. */
        }

        switch (op) {
            case ZeroOp: case OneOp: case OnesOp:
            case BytePrefix: case WordPrefix:
            case DWordPrefix: case QWordPrefix: {
                const uint8_t* before = *p;
                read_integer(p, end);
                if (*p == before) return AML_SKIP_NO;
                continue;
            }
            case StringPrefix:
                (*p)++;
                while (*p < end && **p) (*p)++;
                if (*p >= end) return AML_SKIP_NO;
                (*p)++;                        /* the NUL */
                continue;
            case BufferOp: case PackageOp: case VarPackageOp:
                (*p)++;
                if (!skip_pkg_block(p, end)) return AML_SKIP_NO;
                continue;
            default:
                break;
        }

        if (op >= 0x60 && op <= 0x6E) { (*p)++; continue; }  /* Local/Arg */

        if (aml_is_name_string_lead(op) && op != 0x00) {
            /* A NameString in TermArg position is either a data reference or
             * a method invocation; only the namespace can say which, and a
             * method drags its arguments along behind it. */
            const uint8_t* probe = *p;
            aml_object_t*  o = resolve_name(&probe, end, scope, false, NULL);
            if (!skip_name_string(p, end)) return AML_SKIP_NO;
            if (o && o->type == AML_OBJ_METHOD) {
                uint8_t argc = (uint8_t)(o->v.method.flags & 0x7);
                for (uint8_t i = 0; i < argc; i++)
                    if (!AmlSkipOwe(&n, AML_SKIP_TERM)) return AML_SKIP_NO;
            } else if (!o) {
                /* Not in the namespace — yet. Nothing here can say whether
                 * it is a method with arguments still to come or a name with
                 * none, so what follows is an assumption and the whole
                 * measurement carries it out to the caller. */
                guessed = true;
            }
            continue;
        }

        if (op == ExtOpPrefix) {
            /* CondRefOf / Acquire / Timer ... in TermArg position. The items
             * an operand owes are pushed in REVERSE, so the first one owed
             * is the first one taken off. */
            if ((size_t)(end - *p) < 2) return AML_SKIP_NO;
            uint8_t sub = (*p)[1];
            *p += 2;
            switch (sub) {
                case ExtCondRefOfOp:           /* SuperName Target */
                    if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
                    if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
                    continue;
                case ExtAcquireOp:             /* SuperName WordData */
                    if (!AmlSkipOwe(&n, AML_SKIP_WORD))  return AML_SKIP_NO;
                    if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
                    continue;
                case ExtWaitOp:                /* SuperName TermArg */
                    if (!AmlSkipOwe(&n, AML_SKIP_TERM))  return AML_SKIP_NO;
                    if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
                    continue;
                case ExtFromBCDOp: case ExtToBCDOp:    /* TermArg Target */
                    if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
                    if (!AmlSkipOwe(&n, AML_SKIP_TERM))  return AML_SKIP_NO;
                    continue;
                case ExtRevisionOp: case ExtDebugOp: case ExtTimerOp:
                    continue;                  /* no operands */
                default:
                    return AML_SKIP_NO;
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
                return AML_SKIP_NO;
        }
        (*p)++;
        for (uint8_t i = 0; i < ntargets; i++)
            if (!AmlSkipOwe(&n, AML_SKIP_SUPER)) return AML_SKIP_NO;
        for (uint8_t i = 0; i < nargs; i++)
            if (!AmlSkipOwe(&n, AML_SKIP_TERM)) return AML_SKIP_NO;
    }
    return guessed ? AML_SKIP_GUESS : AML_SKIP_YES;
}

/* ‼ WHERE A GUESS IS STILL WORTH ACTING ON, AND WHERE IT IS NOT.
 *
 * In an OPERAND — an OperationRegion's offset, a BankValue, the arguments of
 * a CreateField — the guess is that a name takes no arguments, and the bytes
 * it measured are the NameString itself, exactly. If the name turns out to be
 * a method with arguments, what was kept is short; and what is kept is only
 * ever handed to AmlRegionArm, which runs the term in a real frame with the
 * namespace complete and refuses ALOUD when the term ends where an operand
 * was owed. Refusing here instead would throw away the Intel chipset form
 * `OperationRegion (RCRB, SystemMemory, \RCBA, 0x4000)` on any board whose
 * \RCBA is declared in a later table, and resynchronise through the block it
 * stands in — a worse outcome from a more cautious rule.
 *
 * Where a guess must not be acted on is where the END of the term decides
 * where a BODY begins, and both of those callers ask AmlSkipRun directly and
 * take AML_SKIP_GUESS apart from AML_SKIP_YES. See parse_one. */
static bool skip_term_arg(const uint8_t** p, const uint8_t* end,
                           aml_object_t* scope) {
    return AmlSkipRun(p, end, scope, AML_SKIP_TERM) != AML_SKIP_NO;
}

/* Target := SuperName | NullName (§20.2.2). */
static bool skip_super_name(const uint8_t** p, const uint8_t* end,
                             aml_object_t* scope) {
    return AmlSkipRun(p, end, scope, AML_SKIP_SUPER) != AML_SKIP_NO;
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
            return skip_super_name(p, end, scope) &&
                   skip_super_name(p, end, scope);
        case ExtCreateFieldOp:            /* TermArg x3 NameString */
            return skip_term_arg(p, end, scope) &&
                   skip_term_arg(p, end, scope) &&
                   skip_term_arg(p, end, scope) &&
                   skip_name_string(p, end);
        case ExtLoadTableOp:              /* TermArg x6 */
            for (int i = 0; i < 6; i++)
                if (!skip_term_arg(p, end, scope)) return false;
            return true;
        case ExtLoadOp:                   /* NameString SuperName */
            return skip_name_string(p, end) &&
                   skip_super_name(p, end, scope);
        case ExtStallOp: case ExtSleepOp: /* TermArg */
            return skip_term_arg(p, end, scope);
        case ExtAcquireOp:                /* SuperName WordData */
            if (!skip_super_name(p, end, scope)) return false;
            if ((size_t)(end - *p) < 2) return false;
            *p += 2;
            return true;
        case ExtSignalOp: case ExtResetOp:
        case ExtReleaseOp: case ExtUnloadOp:      /* SuperName */
            return skip_super_name(p, end, scope);
        case ExtWaitOp:                   /* SuperName TermArg */
            return skip_super_name(p, end, scope) &&
                   skip_term_arg(p, end, scope);
        case ExtFromBCDOp: case ExtToBCDOp:       /* TermArg Target */
            return skip_term_arg(p, end, scope) &&
                   skip_super_name(p, end, scope);
        case ExtRevisionOp: case ExtDebugOp: case ExtTimerOp:
            return true;                  /* no operands */
        case ExtFatalOp:                  /* ByteData DWordData TermArg */
            if ((size_t)(end - *p) < 5) return false;
            *p += 5;
            return skip_term_arg(p, end, scope);
        case ExtOpRegionOp:               /* NameString RegionSpace TermArg x2 */
            if (!skip_name_string(p, end)) return false;
            if (*p >= end) return false;
            (*p)++;
            return skip_term_arg(p, end, scope) &&
                   skip_term_arg(p, end, scope);
        case ExtFieldOp: case ExtDeviceOp: case ExtProcessorOp:
        case ExtPowerResOp: case ExtThermalZoneOp:
        case ExtIndexFieldOp: case ExtBankFieldOp:
            return skip_pkg_block(p, end);        /* PkgLength-bearing */
        case ExtDataRegionOp:             /* NameString TermArg x3 */
            if (!skip_name_string(p, end)) return false;
            for (int i = 0; i < 3; i++)
                if (!skip_term_arg(p, end, scope)) return false;
            return true;
        default:
            return false;
    }
}

/* ===========================================================
 * Parser — populate namespace from a TermList byte range
 * =========================================================== */

/*
 * ‼ THE NAMESPACE WALK DOES NOT DESCEND THE C STACK, AND THAT IS WHY THERE
 *   IS NO DEPTH LIMIT IN IT ANY MORE.
 *
 * What stood here was `if (depth > AML_MAX_SCOPE_DEPTH) return
 * AML_ERR_DEEP_NEST` — a 32 that no specification names, guarding a C
 * recursion that no specification requires either. A firmware that nested
 * one level deeper lost an ENTIRE SUBTREE of its namespace, silently, and
 * everything the evaluator later failed to find in it was reported as the
 * firmware's silence rather than as this pass's.
 *
 * So the walk carries its own stack instead. A Scope or a Device body is not
 * entered by calling parse_term_list again: parse_one hands the body BACK as
 * an AmlParseInto, and the loop pushes it onto a heap-grown stack that grows
 * with the nesting the firmware wrote. Nothing here decides how deep a board
 * may nest — what ends the growth is kmalloc saying no, and that is said out
 * loud with the body named. The same rule the evaluator lives under, and for
 * the same reason.
 */
typedef struct AmlParseInto {
    const uint8_t* p;            /* NULL when there is nothing to descend */
    const uint8_t* end;
    aml_object_t*  scope;
} AmlParseInto;

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
                                         aml_object_t* scope, bool is_method,
                                         AmlParseInto* into) {
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

    /* ‼ SCOPE OPENS AN OBJECT; IT DOES NOT SAY WHAT THE OBJECT IS.
     *
     * ACPI 6.5 §20.2.5.1 gives
     *     DefScope := ScopeOp PkgLength NameString TermList
     * and there is no type in it — §19.6.120 names the operator "Scope (Open
     * Named Scope)". WHAT a named object is comes from the operator that
     * declared it: DefDevice, DefMethod, DefProcessor and the rest of
     * §20.2.5.2.
     *
     * What stood here stamped the opened object AML_OBJ_SCOPE every time. A
     * board that declares `Device (PCI0)` and later opens `Scope (\\_SB.PCI0)`
     * to hang further objects off it — which the firmware on this desk does
     * twice, and shipping boards do as a matter of course — had its PCI host
     * bridge DEMOTED to a plain scope. AmlRoutingAdopt walks the namespace
     * for Devices, found none, and printed "this firmware's namespace holds
     * no PCI root bridge" about a namespace that held one, with _HID
     * PNP0A03 and a 128-entry _PRT beneath it. Every PCI function on the
     * machine then took its pin from config byte 0x3C.
     *
     * A Method is the other case and keeps its assignment: DefMethod IS a
     * declaration, and a forward reference may have left a placeholder scope
     * under that name first. */
    aml_object_t* target;
    if (!leaf[0]) {
        /* The NameString ended in NullName: it names `parent` itself. There
         * is nothing to declare and the TermList belongs to `parent` — this
         * is `Scope (\\)`, which is how firmware puts objects at the root. A
         * Method cannot be nameless, and that malformation is said aloud. */
        if (is_method) {
            kprintf("[AML] a Method opened under %s declares no name — its "
                    "body is not recorded, and nothing can call it\n",
                    parent->name);
            *p = body_end;
            return AML_REFUSED;
        }
        target = parent;
    } else {
        target = obj_find_child(parent, leaf);
        if (!target) {
            target = obj_create_child(parent, leaf,
                        is_method ? AML_OBJ_METHOD : AML_OBJ_SCOPE);
            if (!target) { *p = body_end; return AML_ERR_NOMEM; }
        } else if (is_method) {
            /* ‼ A SECOND BODY FOR A NAME THAT ALREADY HAS ONE OVERWROTE THE
             * FIRST, WHICH IS WORSE THAN SHADOWING IT. See
             * AmlFirstDeclarationWins: the whole package is already measured,
             * so the refusal steps past it and the block goes on. */
            if (!AmlFirstDeclarationWins(target, "Method")) {
                *p = body_end;
                return AML_REFUSED;
            }
            target->type = AML_OBJ_METHOD;
        }
    }

    if (is_method) {
        if (np >= body_end) { *p = body_end; return AML_OK; }
        target->v.method.flags  = *np++;
        target->v.method.aml    = np;
        target->v.method.aml_len = (uint32_t)(body_end - np);
        target->declared        = true;
    } else if (np < body_end) {
        into->p     = np;
        into->end   = body_end;
        into->scope = target;
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
    /* ‼ THE CURSOR IS WIDER THAN WHAT IT COUNTS, ON PURPOSE. Each element's
     * width is a PkgLength, which §20.2.4 lets reach 0x0FFFFFFF, and a
     * FieldList may hold any number of them. Added up in 32 bits they wrap,
     * and a wrapped offset is a SMALL one — it passes every bound check and
     * points the access at the wrong end of the register. Counted here in
     * 64, and a field whose offset no longer fits the 32 bits this pass
     * records is said aloud instead of recorded wrong. */
    uint64_t bit_cursor = 0;
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

        if (bit_cursor + bits > 0xFFFFFFFFull) {
            kprintf("[AML] FieldList in %s: %c%c%c%c would start at bit %lu, "
                    "past where this pass can record an offset — the rest of "
                    "the block is dropped rather than recorded at a bit "
                    "offset that wrapped\n", scope->name,
                    seg[0], seg[1], seg[2], seg[3],
                    (unsigned long)bit_cursor);
            break;
        }

        aml_object_t* f = obj_create_child(scope, seg, AML_OBJ_FIELD);
        if (!f) break;
        f->v.field.region     = site->region;
        f->v.field.index      = site->index;
        f->v.field.data       = site->data;
        f->v.field.bank       = site->bank;
        f->v.field.bank_value = site->bank_value;
        f->v.field.bit_offset = (uint32_t)bit_cursor;
        f->v.field.bit_width  = bits;
        f->v.field.access     = access;
        bit_cursor += bits;
    }
}

static aml_status_t handle_extended(const uint8_t** p, const uint8_t* end,
                                     aml_object_t* scope,
                                     AmlParseInto* into) {
    (*p) += 2;     /* ExtOp + opcode */
    /* Sub-opcode is at (*p - 1). */
    uint8_t sub = (*p)[-1];

    if (sub == ExtOpRegionOp) {
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent || *p >= end) return AML_ERR_BOUNDS;
        if (!AmlLeafNamed(leaf, "OperationRegion")) return AML_ERR_BAD_OPCODE;
        uint8_t space = **p; (*p)++;
        /* DefOpRegion := OpRegionOp NameString RegionSpace RegionOffset
         * RegionLen, and §20.2.5.2 makes both RegionOffset and RegionLen
         * TermArgs — a NameSeg naming a Name or a Field is as legal there
         * as a literal, and both Intel's and AMD's shipping reference ASL
         * use exactly that. This pass has no frame to evaluate one in, so
         * the non-constant form is stepped over EXACTLY — the measurement
         * is what keeps the rest of the TermList aligned — and THE BYTES
         * ARE KEPT, to be turned into a number by AmlRegionArm the first
         * time anything wants to read through the region. What is NOT done
         * is recording a zero and calling it a base. */
        uint64_t base = 0, len = 0;
        const uint8_t* base_from = *p;
        bool have_base = read_const_integer(p, end, &base);
        bool measured  = have_base || skip_term_arg(p, end, scope);
        const uint8_t* base_to = *p;
        const uint8_t* len_from = *p;
        bool have_len  = measured && read_const_integer(p, end, &len);
        if (measured && !have_len) measured = skip_term_arg(p, end, scope);
        const uint8_t* len_to = *p;

        aml_object_t* o = obj_create_child(parent, leaf, AML_OBJ_REGION);
        if (!o) return AML_ERR_NOMEM;
        o->v.region.space       = space;
        o->v.region.base        = base;
        o->v.region.len         = len;
        o->v.region.base_known  = have_base;
        o->v.region.len_known   = have_len;
        o->v.region.declared_in = scope;
        if (measured && !have_base && base_to > base_from) {
            o->v.region.base_term     = base_from;
            o->v.region.base_term_end = base_to;
        }
        if (measured && !have_len && len_to > len_from) {
            o->v.region.len_term     = len_from;
            o->v.region.len_term_end = len_to;
        }

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
        if (!AmlLeafNamed(leaf, "Device or Processor")) {
            *p = body_end;
            return AML_REFUSED;
        }
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
        if (np < body_end) {
            into->p     = np;
            into->end   = body_end;
            into->scope = sub_obj;
        }
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
        if (!AmlLeafNamed(leaf, sub == ExtMutexOp ? "Mutex" : "Event"))
            return AML_ERR_BAD_OPCODE;
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
                               aml_object_t* scope, AmlParseInto* into) {
    if (*p >= end) return AML_ERR_BOUNDS;
    uint8_t op = **p;

    if (op == NameOp) {
        (*p)++;
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent) return AML_ERR_BOUNDS;
        if (!AmlLeafNamed(leaf, "Name")) return AML_ERR_BAD_OPCODE;
        aml_object_t* o = obj_find_child(parent, leaf);
        if (o && !AmlFirstDeclarationWins(o, "Name")) {
            /* Refused, and still MEASURED: the DataRefObject is read into an
             * object that goes nowhere, so the stream stands at the next
             * TermObj and the rest of the block is not resynchronised
             * through. */
            aml_object_t scratch;
            memset(&scratch, 0, sizeof(scratch));
            aml_status_t s = handle_named_data(p, end, &scratch);
            return (s == AML_OK) ? AML_REFUSED : s;
        }
        /* A name this pass met only as a path before now gets what it IS —
         * the placeholder resolve_name left, or the scope a Device body
         * opened, is filled rather than shadowed by a second object. */
        if (!o) o = obj_create_child(parent, leaf, AML_OBJ_NAME);
        if (!o) return AML_ERR_NOMEM;
        aml_status_t s = handle_named_data(p, end, o);
        if (s == AML_OK) o->declared = true;
        return s;
    }
    if (op == ScopeOp)   return handle_scope_method(p, end, scope, false, into);
    if (op == MethodOp)  return handle_scope_method(p, end, scope, true, into);
    if (op == AliasOp) {
        (*p)++;
        aml_object_t* src = resolve_name(p, end, scope, false, NULL);
        char leaf[4];
        aml_object_t* parent = resolve_name(p, end, scope, true, leaf);
        if (!parent) return AML_ERR_BOUNDS;
        if (!AmlLeafNamed(leaf, "Alias")) return AML_ERR_BAD_OPCODE;
        aml_object_t* a = obj_create_child(parent, leaf, AML_OBJ_ALIAS);
        if (a) a->v.alias.target = src;
        return AML_OK;
    }
    if (op == ExtOpPrefix) return handle_extended(p, end, scope, into);

    /* ‼ §20.2.5.1: TermObj := Object | StatementOpcode | ExpressionOpcode.
     * Everything above declares an Object. The other two kinds are MEASURED
     * here and stepped over exactly, and that is not tidiness.
     *
     * What stood here was a bare AML_ERR_BAD_OPCODE for all of them, which
     * sent parse_term_list into its one-byte resync — through the whole body
     * of the `If (CondRefOf (...))` that shipping SSDTs wrap their
     * declarations in. The resync reads that body's DATA as opcodes: on the
     * ASRock B365M Pro4-F it landed on an 0x08 followed by an 0x00
     * twenty-six times and refused, each time, a `Name` the firmware never
     * wrote. One byte further along, the same landing DECLARES an object into
     * the namespace instead of refusing one.
     *
     * ‼ AND THE BODY OF AN If IS WALKED, BOTH SIDES OF IT. Skipping the
     * package would be exact and would still lose what the firmware declared
     * inside — and shipping SSDTs declare whole Devices there. Which side a
     * board takes is decided by a value this pass has no frame to compute, so
     * BOTH are recorded, which is the same choice the narrow gate makes when
     * it proves a method it has no values for. A name declared on one side
     * and not the other is then in the namespace either way; a name declared
     * on BOTH is the FIRST one — the If side — and the second is refused
     * aloud by AmlFirstDeclarationWins, which is where that choice is argued.
     * Neither outcome can be a byte of data read as an opcode. */
    if (op == IfOp || op == ElseOp || op == WhileOp) {
        /* §20.2.5.3: DefIfElse := IfOp PkgLength Predicate TermList,
         * DefWhile := WhileOp PkgLength Predicate TermList, and
         * DefElse := ElseOp PkgLength TermList — no predicate on the Else. */
        (*p)++;
        uint32_t cbytes;
        uint32_t pklen = decode_pkglen(p, end, &cbytes);
        if (!cbytes || pklen < cbytes) return AML_ERR_BOUNDS;
        const uint8_t* body_end = *p + pklen;
        if (body_end > end) return AML_ERR_BOUNDS;
        const uint8_t* np = *p + cbytes;
        *p = body_end;                   /* the parent resumes past the whole */
        g_parse_stmts++;
        AmlSkipHow how = (op == ElseOp)
                           ? AML_SKIP_YES
                           : AmlSkipRun(&np, body_end, scope, AML_SKIP_TERM);
        if (how != AML_SKIP_YES) {
            /* Where the predicate ENDS is where the TermList begins, and a
             * body entered at the wrong byte is the very thing this branch
             * exists to stop. A length that was guessed is no better than one
             * that could not be taken at all: AmlSkipRun assumes a name it
             * has not seen declared takes no arguments, so the cursor it left
             * can stand inside the predicate. Both step over the whole
             * package and record nothing inside it. */
            g_parse_unwalked++;
            if (how == AML_SKIP_NO)
                kprintf("[AML] a %s at TermList level under %s has a "
                        "predicate this pass cannot measure, so where its "
                        "body begins is not known and nothing inside it is "
                        "recorded\n",
                        op == IfOp ? "If" : "While", scope->name);
            else if (g_parse_guessed++ == 0)
                kprintf("[AML] a %s at TermList level under %s has a "
                        "predicate naming something this pass has not seen "
                        "declared, so how many arguments follow it — and "
                        "therefore where the predicate ENDS — was assumed "
                        "rather than measured. Nothing inside the body is "
                        "recorded: a body walked from a byte in the middle "
                        "of a predicate declares objects out of operand "
                        "bytes. Said once; the count is in the summary at "
                        "the end of the load\n",
                        op == IfOp ? "If" : "While", scope->name);
            return AML_REFUSED;
        }
        if (np < body_end) {
            into->p     = np;
            into->end   = body_end;
            into->scope = scope;         /* §20.2.5.3 opens no scope */
        }
        return AML_OK;
    }
    if (op == ReturnOp) {
        (*p)++;
        if (!skip_term_arg(p, end, scope)) return AML_ERR_BAD_OPCODE;
        g_parse_stmts++;
        return AML_REFUSED;
    }
    if (op == NoopOp || op == BreakOp || op == ContinueOp ||
        op == BreakPointOp) {
        (*p)++;
        g_parse_stmts++;
        return AML_REFUSED;
    }
    /* ‼ A BARE NAME AT TERMOBJ LEVEL IS A METHOD INVOCATION OR IT IS NOT A
     * STATEMENT AT ALL. §20.2.5.1 admits a NameString here only as
     * MethodInvocation := NameString TermArgList, and how many TermArgs
     * follow is the CALLEE's argument count, which only the namespace holds.
     *
     * So a name that resolves to no Method is not measured, and that is what
     * bounds the resync. The resync steps one byte at a time and every byte
     * from 'A' to 'Z', plus '_', '\', '^', 0x2E and 0x2F, leads a NameString
     * — and §20.2.2 lets a MultiNamePath carry a SegCount of up to 255
     * four-character segments. Measuring a stray one of those swallowed up to
     * 1020 bytes of the block being resynchronised, taking every declaration
     * inside that span with it, where a byte that leads nothing advances by
     * one. A forward-referenced call is stepped over by one byte for the same
     * reason an unmeasurable predicate is: a length nobody could take is not
     * a length. */
    if (aml_is_expression_op(op) ||
        (aml_is_name_string_lead(op) && op != 0x00)) {
        bool callable = aml_is_expression_op(op);
        if (!callable) {
            const uint8_t* probe  = *p;
            aml_object_t*  callee = resolve_name(&probe, end, scope, false,
                                                 NULL);
            callable = (callee && callee->type == AML_OBJ_METHOD);
        }
        if (callable) {
            const uint8_t* before = *p;
            AmlSkipHow     how    = AmlSkipRun(p, end, scope, AML_SKIP_TERM);
            if (how == AML_SKIP_YES && *p > before) {
                g_parse_stmts++;
                return AML_REFUSED;
            }
            if (how == AML_SKIP_GUESS && g_parse_guessed++ == 0)
                kprintf("[AML] a term standing alone under %s names something "
                        "this pass has not seen declared, so where it ends "
                        "was assumed rather than measured — the walk goes on "
                        "from the next byte rather than from a byte that may "
                        "stand in the middle of it. Said once; the count is "
                        "in the summary at the end of the load\n",
                        scope->name);
            *p = before;
        }
    }

    return AML_ERR_BAD_OPCODE;
}

static aml_status_t parse_term_list(const uint8_t* aml, uint32_t len,
                                     aml_object_t* scope) {
    uint32_t       room = 0, n = 0;
    AmlParseInto*  stk  =
        (AmlParseInto*)AmlStackRoom(NULL, &room, 1, sizeof(AmlParseInto));
    if (!stk) {
        kprintf("[AML] the heap has no room to begin walking this table's "
                "namespace — nothing of it is recorded\n");
        return AML_ERR_NOMEM;
    }
    stk[0].p     = aml;
    stk[0].end   = aml + len;
    stk[0].scope = scope;
    n = 1;

    aml_status_t worst = AML_OK;
    while (n) {
        if (stk[n - 1].p >= stk[n - 1].end) { n--; continue; }

        const uint8_t* before = stk[n - 1].p;
        AmlParseInto   into   = { NULL, NULL, NULL };
        aml_status_t   s      = parse_one(&stk[n - 1].p, stk[n - 1].end,
                                          stk[n - 1].scope, &into);
        /* The pool is gone: every later declaration would be lost anyway,
         * and obj_alloc has already said so. */
        if (s == AML_ERR_NOMEM) { worst = s; break; }

        if (into.p) {
            /* A body to walk, with the parent already standing past it —
             * handle_scope_method and handle_extended move it to body_end
             * before they hand the body over, so the parent resumes at the
             * next TermObj whatever happens inside. */
            AmlParseInto* grown =
                (AmlParseInto*)AmlStackRoom(stk, &room, n + 1,
                                            sizeof(AmlParseInto));
            if (!grown) {
                kprintf("[AML] %[E]the heap has no room to walk into %s — "
                        "this firmware nests its namespace deeper than the "
                        "heap can follow, and everything declared inside "
                        "that body is missing rather than silently "
                        "renamed%[D]\n", into.scope->name);
                worst = AML_ERR_NOMEM;
                break;
            }
            stk = grown;
            stk[n++] = into;
            continue;
        }

        /* AML_REFUSED: the term was measured and rejected, and `p` already
         * stands at the next TermObj. Resyncing over it would walk the block
         * this pass just declined, one byte at a time, declaring objects out
         * of whatever operand bytes happen to look like an opcode. */
        if (s == AML_REFUSED && stk[n - 1].p > before) continue;
        if (stk[n - 1].p == before || s == AML_ERR_BAD_OPCODE)
            stk[n - 1].p = before + 1;
    }

    kfree(stk);
    return worst;
}

/* =====================================================================
 * THE NARROW EVALUATOR — enough AML to ask the firmware WHERE A PCI
 * INTERRUPT GOES, and not one operator beyond what that question needs.
 *
 * ‼ WHAT THIS IS ALLOWED TO DO, AND WHY THE BOUNDARY IS WHERE IT IS.
 *
 * An executor lived in this file once and was removed with the ACPI S1-S4
 * sleep path it served. That removal stands: there is no sleep path here, no
 * Notify, and no mutex taken as a means of synchronising with the firmware.
 *
 * ‼ AND IT WRITES OUTSIDE THIS KERNEL'S MEMORY IN EXACTLY ONE CASE, WHICH IS
 * NAMED HERE BECAUSE IT USED NOT TO. There was once no function in this file
 * that wrote an address space at all, so "reads only" needed no guard. There
 * is one now — AmlRegionWrite — and what replaces the old guarantee is not a
 * weaker one but a narrower one, spelled out above that function: a store
 * happens only inside a \_PIC that a dry walk proved can be run from
 * beginning to end, only into a field of a SystemMemory region, and only at
 * an address this board's own memory map declares ACPI NVS. Every other
 * store into every other address space is refused BY NAME, with the reason
 * printed — see AmlSpaceWriteReason, which takes the ten refused spaces one
 * at a time. \_PIC earned that and nothing else did: ACPI 6.5 §5.8.1 makes
 * an untold firmware go on describing the machine to a kernel that does not
 * exist, and there is no way to tell it without a store.
 *
 * What is here is a READER. It exists because the answer to "where does this
 * interrupt go" is not data on most boards; it is four to twenty lines of
 * AML. The shape is the same everywhere, and every part of it is needed:
 *
 *     Method (_PRT, 0) { If (PICM) { Return (AR00) } Return (PR00) }
 *
 *     Device (LNKA) {
 *       Name (_HID, EISAID ("PNP0C0F"))
 *       Method (_STA, 0) { Return (IQST (PRQ0)) }      <- calls a method
 *       Method (_CRS, 0) { Return (IQCR (PRQ0)) }      <- calls a method
 *     }
 *     Method (IQCR, 1, Serialized) {
 *       Name (PRR0, ResourceTemplate () { Interrupt (...) {0} })  <- local object
 *       CreateDWordField (PRR0, 0x05, PRRI)                       <- buffer field
 *       Store (Arg0, PRRI)                                        <- writes MEMORY
 *       Return (PRR0)
 *     }
 *
 * and PRQ0 is a Field over `OperationRegion (P40C, PCI_Config, 0x60, 4)` —
 * the chipset's PIRQ routing register. That last step is the whole point: the
 * number this kernel needs is IN A REGISTER, and the firmware's own ASL is
 * the only description of where that register is.
 *
 * ‼ SO THE EVALUATOR READS ADDRESS SPACES, AND IT DOES IT BY ASKING THE
 *   SUBSYSTEM THAT OWNS THEM. It owns no door of its own:
 *     PCI_Config   -> pci.c's configuration accessors, the only holder of
 *                     0xCF8/0xCFC and of the ECAM window in this kernel
 *     SystemMemory -> vmm_map_mmio / vmm_unmap_mmio, mapped READ-ONLY, for
 *                     the length of one field read and not one instruction
 *                     longer
 *     SystemIO     -> io.h
 *   Three of the spaces ACPI 6.5 Table 5.182 defines. Every other space in
 *   that table is refused BY NAME: each needs a transport protocol of its
 *   own (EmbeddedControl, SMBus, IPMI, GenericSerialBus, PCC,
 *   PlatformRtMechanism), or an instruction sequence the processor
 *   architecture rather than ACPI defines (FFixedHW), or a target this
 *   kernel does not resolve (PciBarTarget, GeneralPurposeIO), or belongs to
 *   a driver that already owns it (SystemCMOS) — and none of them appears on
 *   the interrupt-routing path on any board.
 *
 * ‼ AND IT WRITES IN ONE OF THEM, UNDER THREE CONDITIONS THAT MUST ALL HOLD.
 *   AmlRegionWrite is the one function in this file that writes an address
 *   space, and the long note above it sets out what has to be true at once:
 *   the narrow gate proved the whole method first, the run is the real run
 *   of \_PIC, and the target is a whole-access-unit field of a SystemMemory
 *   region at an address the board's own memory map declares ACPI NVS. A
 *   Store into any other field, in any other space, on any other run, is
 *   refused ALOUD and the method is abandoned rather than half-run — see
 *   AmlStoreInto and AmlSpaceWriteReason. An IndexField and a BankField are
 *   refused a step earlier in both directions: reaching either one means
 *   writing its index or bank selector FIRST, and two writes in a fixed
 *   order is a protocol, not a store.
 *
 * ‼ THE WINDOW, AND WHY IT CANNOT BE LEFT OPEN.
 *   An address space is reachable only while an object that DESCRIBES the
 *   machine is being evaluated — _PRT (§6.2.13), _CRS (§6.2.2), _PRS
 *   (§6.2.12), _STA (§6.3.7), _PIC (§5.8.1), _HID (§6.1.5) and _CID
 *   (§6.1.2). Outside that, every access is refused exactly as it
 *   was before any of this existed.
 *
 *   It is not a flag. `window` is a field of AmlRun, AmlRun is a LOCAL
 *   VARIABLE of the function that wanted the routing object, and it is
 *   assigned in exactly ONE place in this file — the first statement of
 *   AmlEvalIn, from the NAME OF THE OBJECT BEING EVALUATED. So:
 *     * it cannot be opened for something that is not a routing object,
 *       because the caller does not get to say;
 *     * it cannot be left open, because closing it is the C function
 *       returning, and that happens on every path including every refusal
 *       and every abandoned method;
 *     * it cannot leak into a later evaluation, because the next AmlEvalIn
 *       on the same run assigns it again from that object's own name.
 *   There is one assignment to it in this file and it is inside AmlEvalIn,
 *   which is the mechanism — not a convention somebody has to keep.
 *
 * ‼ AND IT NEVER ANSWERS ZERO INSTEAD OF REFUSING. Every function here that
 * cannot produce a value returns false with a sentence on the console naming
 * what it could not do. A fabricated 0 in this path is a GSI, and a
 * fabricated GSI is a device wired to an interrupt nobody delivers.
 *
 * ‼ HOW DEEP AND HOW LONG IT CAN RUN — by construction, not by a counter.
 *   * NO C RECURSION. Statements, expressions and METHOD CALLS are all
 *     driven by one loop (AmlRunTo) over one explicit stack of activations
 *     that grows on the heap with the nesting the firmware actually wrote.
 *     What ends the growth is kmalloc saying no, and that is said out loud.
 *     The C stack depth of this whole subsystem is constant.
 *   * IT PROVABLY TERMINATES, AND THERE IS NOW A LOOP OPCODE. DefWhile
 *     (§20.2.5.3) is run. What used to carry the guarantee was that no block
 *     could be re-entered — every one of them a finite byte range walked
 *     forward once — and a loop deletes that. What carries it instead is not
 *     a turn count and not a timeout; a wait that ends on a number somebody
 *     guessed is a wait that ends at the wrong moment on somebody's board.
 *     It is this:
 *
 *       A TURN OF A LOOP IS A FUNCTION OF THE STATE IT STARTS FROM. That
 *       state is a finite, enumerable thing and every part of it is in this
 *       file: the eight Locals and seven Args of §20.2.6, the objects the
 *       method declared for itself and the bytes of their buffers, and what
 *       the turn has to do with anything OUTSIDE the activation — what a
 *       field read answered, what a store placed in a namespace object or in
 *       an address space. Nothing else can reach a running method: the
 *       namespace is otherwise read-only here, a called method's own
 *       activation dies with the call, and there is no interrupt, no thread
 *       and no clock in this evaluator's reach.
 *
 *       So the whole of it is reduced to one number at the end of every turn
 *       (AmlTurnMark), and the numbers a loop has already produced are kept
 *       (AmlSeen). A turn that produces a number this loop produced before
 *       has put the machine back in a state it has already been in, with
 *       nothing outside it having changed that it did not itself observe —
 *       so every turn from there is a turn that already ran, and the loop
 *       has no end. It is refused at that turn, ALOUD, and the method is
 *       abandoned rather than left half-run, exactly as every other refusal
 *       in this file abandons it.
 *
 *       Termination follows by construction and not by measurement: a loop
 *       can produce a number it has never produced before only finitely many
 *       times. Every turn either leaves the loop, or shows a state never seen
 *       before — and the account of those states grows on the heap, so a
 *       board that manages to keep showing new ones stops at kmalloc saying
 *       no, loudly, exactly as an endlessly recursing firmware already did.
 *       There is no third outcome. It is the same shape as the bound on
 *       halt_drain_async_writes, which ends because a non-negative integer
 *       can set a new low only finitely many times.
 *
 *       ‼ AND HERE IS WHAT IT DOES NOT DO: IT DOES NOT WAIT ON HARDWARE.
 *       `While (LEqual (STAT, One))` is a firmware wait, and what the machine
 *       answered is IN the number — so a register that answers something new
 *       leaves the loop turning for as long as it keeps doing so, and a
 *       register that answers the same bits twice with nothing else in the
 *       activation changed is REFUSED, loudly, with the method abandoned. To
 *       a static evaluator those two turns are the same turn and there is no
 *       honest third answer; but a real device can and does answer the same
 *       bits for a while before it changes, so the refusal can land on a wait
 *       that would have finished. That is the bargain: this evaluator reads
 *       the machine and never waits for it, and a firmware method that waits
 *       is a method it does not carry out. No duration is involved in either
 *       direction, and none is claimed.
 *
 *     A method that calls another method still costs an activation the heap
 *     has to supply, and a firmware that recurses without end still stops at
 *     the heap, loudly, instead of spinning a core.
 *   * ONE FRAME AT A TIME IS GONE, AND ON PURPOSE. It used to hold because
 *     a NameString resolving to a Method was refused in expression position.
 *     That refusal made the evaluator unable to read a PCI interrupt link on
 *     ANY shipping board — every one of them writes `Return (IQCR (PRQ0))`
 *     or its equivalent — so it was the wrong invariant. What it was
 *     protecting (no C recursion, no unbounded descent) is protected above,
 *     by the explicit stack, which is stronger.
 *
 * Single-threaded by the only thing that calls it: the routing table is read
 * on the BSP during device bring-up, and every entry point below takes its
 * own AmlRun, so there is no state between them to share.
 * ===================================================================== */

/* Buffer-field makers and the two bit scanners — §20.2.5.2, §20.2.5.4. */
#define FindSetLeftBitOp   0x81
#define FindSetRightBitOp  0x82
#define CreateDWordFieldOp 0x8A
#define CreateWordFieldOp  0x8B
#define CreateByteFieldOp  0x8C
#define CreateBitFieldOp   0x8D
#define ObjectTypeOp       0x8E
#define CreateQWordFieldOp 0x8F

/* ACPI 6.5 §20.2.6.2 gives the locals as Local0..Local7 and §20.2.6.1 the
 * arguments as Arg0..Arg6. Both counts are the specification's; the opcode
 * ranges 0x60..0x67 and 0x68..0x6E are Table 20.2's. */
#define Local0Op         0x60
#define Local7Op         0x67
#define Arg0Op           0x68
#define Arg6Op           0x6E
#define AML_LOCAL_COUNT  8
#define AML_ARG_COUNT    7

/*
 * How wide an Integer is. ACPI 6.5 §19.6.29, DefinitionBlock: "the bit width
 * of Integer objects is dependent on the ComplianceRevision of the DSDT. If
 * the ComplianceRevision is less than 2, all integers are restricted to 32
 * bits. Otherwise, full 64-bit integers are used" — the revision being the
 * DSDT header's, which aml_init reads. (The sentence quoted here used to be
 * a paraphrase in quotation marks attributed to no subsection at all; this
 * is the specification's own wording, and its own section.) Not a constant
 * chosen here: it is a property of the table the board shipped, and getting
 * it wrong turns Ones into the wrong number and every Not and ShiftLeft with
 * it.
 *
 * Zero until that table has been read, and the mask below answers 64 bits
 * ONLY for a board that said 64. Nothing can be evaluated before the DSDT is
 * parsed — there is no namespace to evaluate — so the zero state is
 * unreachable; it is written this way so that the wide answer has a board
 * behind it rather than a default, which is what a 64 standing here as an
 * initialiser was.
 */
static uint32_t g_int_bits = 0;

static uint64_t AmlIntMask(void) {
    return (g_int_bits == 64) ? ~0ULL : 0xFFFFFFFFULL;
}

typedef struct AmlValue {
    aml_obj_type_t type;
    uint64_t       integer;
    const uint8_t *bytes;        /* STRING/BUFFER contents; PACKAGE elements */
    const uint8_t *bytes_end;
    uint32_t       count;        /* PACKAGE: the element count it declares */
    aml_object_t  *scope;        /* where a NameString inside it resolves */
    aml_object_t  *node;         /* set when the operand WAS a name */
} AmlValue;

/*
 * ‼ THE STATES ONE While LOOP HAS ALREADY BEEN IN.
 *
 * Open addressing on a power-of-two table, so asking "has this turn happened
 * before" costs the same on the thousandth turn as on the second. It grows on
 * the heap with the number of DISTINCT states the loop reaches — never with
 * the number of turns, because a turn that is not distinct is the turn the
 * loop is refused on. Nothing here decides how many states a board may have;
 * what ends the growth is kmalloc saying no, and that is said out loud.
 *
 * Zero is the empty slot, so AmlTurnMark never answers zero.
 */
typedef struct AmlSeen {
    uint64_t *slot;
    uint32_t  room;              /* a power of two, or zero before the first */
    uint32_t  used;
} AmlSeen;

/* One TermList still being walked: a method body, the body of an If or an
 * Else that was entered, or the body of a While that is turning. */
typedef struct AmlBlock {
    const uint8_t *p;
    const uint8_t *end;

    /* ‼ SET ONLY ON THE BODY OF A While, AND IT IS WHAT MAKES THE BLOCK A
     * LOOP. `pred` is where the loop's predicate begins — inside the same
     * package, ahead of the body — and `end` is the end of that package. A
     * block whose `pred` is NULL is walked forward once and popped, exactly
     * as every block in this file used to be. */
    const uint8_t *pred;
    AmlSeen        seen;

    /* What this TURN has had to do with anything outside the activation: what
     * the machine answered a read, and what the method placed into a
     * namespace object or an address space. Folded in by AmlRunHeard, read
     * and cleared by AmlLoopTurn at the end of every turn. Two turns whose
     * dealings differ are two different turns, however alike the Locals
     * look. */
    uint64_t       world;
    uint32_t       worldn;
} AmlBlock;

/*
 * An object a method body declared for itself — `Name (PRR0, ...)` or one of
 * the CreateXField operators. ACPI 6.5 §20.2.5.2 puts these in the namespace
 * under the running method and takes them away again when it returns, and
 * that is exactly their lifetime here: they live in the activation, and the
 * activation dies with the call. Nothing a method declares can outlive it
 * into the namespace the next boot stage reads.
 */
typedef struct AmlLocalObj {
    char      name[4];
    bool      is_field;          /* a CreateXField view over another local */
    uint32_t  over;              /* index of the object it cuts into */
    uint32_t  bit_offset;
    uint32_t  bit_width;
    AmlValue  value;             /* for a plain Name */
    uint8_t  *buf;               /* writable copy, when the value is a Buffer */
    uint32_t  buf_len;
} AmlLocalObj;

/* One pending prefix operator — or one pending method call — and the
 * operands collected for it so far. */
typedef struct AmlPending {
    uint8_t       op;
    bool          call;
    aml_object_t *callee;
    uint8_t       need;
    uint8_t       have;
    AmlValue      got[AML_ARG_COUNT];
} AmlPending;

/* What the expression now being folded is FOR. */
typedef enum {
    AML_AFTER_RETURN = 0,
    AML_AFTER_IF,
    AML_AFTER_WHILE,
    AML_AFTER_STORE,
    AML_AFTER_DISCARD,
} AmlAfter;

/* One method body being walked. */
typedef struct AmlAct {
    aml_object_t *method;
    const char   *what;          /* its name, printed in every refusal */
    AmlValue      local[AML_LOCAL_COUNT];
    AmlValue      arg[AML_ARG_COUNT];

    AmlBlock     *blk;    uint32_t blkn,  blk_room;
    AmlLocalObj  *obj;    uint32_t objn,  obj_room;
    AmlPending   *pend;   uint32_t pendn, pend_room;

    bool            in_expr;
    const uint8_t  *cur;
    const uint8_t  *cur_end;
    AmlValue        val;
    bool            have_val;
    AmlAfter        after;
    const uint8_t  *if_end;      /* AML_AFTER_IF: end of the If package */

    AmlValue      ret;
    bool          returned;
} AmlAct;

/*
 * One evaluation. A LOCAL VARIABLE of whoever asked a question, never a
 * global — which is what makes the window below impossible to leave open and
 * the heap below impossible to leak.
 */
typedef struct AmlRun {
    AmlAct   *act;  uint32_t actn,  act_room;
    void    **own;  uint32_t ownn,  own_room;   /* heap this run handed out */

    /* ‼ THE ROUTING WINDOW. Assigned in exactly one place: the first
     * statement of AmlEvalIn, from the name of the object being evaluated. */
    bool      window;
    const char *window_for;

    /* ‼ THE PERMISSION TO WRITE, WHICH IS TWO FACTS AND NOT ONE.
     *
     * `proven` is set by AmlRoutingAdopt and nowhere else, only after the
     * narrow gate has walked the whole of \_PIC and of every method it calls
     * and found nothing it cannot perform. `may_write` is then assigned in
     * exactly one place — beside the window, in AmlEvalIn — as `proven` AND
     * "the object being evaluated is \_PIC". A run that AmlRunInit made and
     * nobody proved has `proven` false, so a nested evaluation (a region's
     * base term, a link's _CRS) cannot inherit the permission; and since
     * AmlRun is a local variable of whoever asked, the permission ends when
     * that C function returns, on every path including every refusal.
     *
     * `writes` counts what actually landed. It is not bookkeeping: if a
     * method that was proven runnable then fails anyway — a register nobody
     * answers at, a division by zero the gate cannot see — this is what lets
     * the machine say out loud that the firmware has been told half of
     * something, instead of reporting a clean failure. */
    bool      proven;
    bool      may_write;
    uint32_t  writes;

    AmlValue  result;
    bool      returned;
} AmlRun;

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

/* ===========================================================
 * The run's own memory
 * =========================================================== */

static void AmlRunInit(AmlRun *run) {
    memset(run, 0, sizeof(*run));
}

/* Memory the run hands out lives exactly as long as the run. A buffer a
 * method BUILT and returned is read by the caller after the machine has
 * stopped — AmlLinkRoute reads the resource template its _CRS constructed —
 * so it cannot be freed with the activation that made it, and it must not
 * outlive the question either. */
static void *AmlRunAlloc(AmlRun *run, uint32_t bytes) {
    void **grown = (void **)AmlStackRoom(run->own, &run->own_room,
                                         run->ownn + 1, sizeof(void *));
    if (!grown) return NULL;
    run->own = grown;
    void *p = kmalloc(bytes);
    if (!p) return NULL;
    run->own[run->ownn++] = p;
    return p;
}

static void AmlSeenForget(AmlSeen *s) {
    if (s->slot) kfree(s->slot);
    s->slot = NULL;
    s->room = 0;
    s->used = 0;
}

/*
 * Leave every block above `to`. The ONLY way a block is left in this file, so
 * that a loop's account of the states it has been in is given back on every
 * path there is — the loop ending, a Break, a Return out of the middle of it,
 * and the activation being torn down.
 */
static void AmlBlocksDrop(AmlAct *a, uint32_t to) {
    while (a->blkn > to) {
        a->blkn--;
        AmlSeenForget(&a->blk[a->blkn].seen);
    }
}

static void AmlActRelease(AmlAct *a) {
    if (a->blk) {
        AmlBlocksDrop(a, 0);
        kfree(a->blk);
    }
    if (a->obj)  kfree(a->obj);
    if (a->pend) kfree(a->pend);
    a->blk = NULL; a->obj = NULL; a->pend = NULL;
}

static void AmlRunDone(AmlRun *run) {
    for (uint32_t i = 0; i < run->actn; i++) AmlActRelease(&run->act[i]);
    if (run->act) kfree(run->act);
    for (uint32_t i = 0; i < run->ownn; i++) kfree(run->own[i]);
    if (run->own) kfree(run->own);
    memset(run, 0, sizeof(*run));
}

/* =====================================================================
 * WHAT A TURN OF A LOOP WAS — see THE NARROW EVALUATOR's doctrine below,
 * under HOW LONG IT CAN RUN.
 * ===================================================================== */

/* FNV-1a's 64-bit prime, with a final xor-shift so a change in one low bit
 * reaches the high ones. Nothing here depends on the constant beyond its
 * spreading bits; what the mixing is FOR is the argument in the doctrine. */
static uint64_t AmlMix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ULL;
    return h ^ (h >> 29);
}

/*
 * ‼ EVERY LOOP NOW TURNING ON THIS RUN HEARS THIS.
 *
 * Called with what the machine answered a read, and with what a store placed
 * into a namespace object or an address space — the only two ways a method
 * body can differ from one turn to the next without the difference being
 * visible in the activation itself. It reaches every loop frame of every
 * activation, not just the innermost, because from an outer loop's point of
 * view the whole of an inner loop, and the whole of a method it called, is
 * part of ONE of its turns.
 */
static void AmlRunHeard(AmlRun *run, uint64_t what) {
    for (uint32_t i = 0; i < run->actn; i++) {
        AmlAct *a = &run->act[i];
        for (uint32_t b = 0; b < a->blkn; b++) {
            if (!a->blk[b].pred) continue;
            a->blk[b].world = AmlMix(a->blk[b].world, what);
            a->blk[b].worldn++;
        }
    }
}

/* One AmlValue, whole. Its pointers are identities — the bytes a String or a
 * Buffer names are inside the table this kernel mapped and do not move — so
 * the pointer is the value. The bytes that DO change under a pointer are a
 * method-local buffer's, and AmlTurnMark reads those one by one. */
static uint64_t AmlValueMark(uint64_t h, const AmlValue *v) {
    h = AmlMix(h, (uint64_t)v->type);
    h = AmlMix(h, v->integer);
    h = AmlMix(h, (uint64_t)(uintptr_t)v->bytes);
    h = AmlMix(h, (uint64_t)(uintptr_t)v->bytes_end);
    h = AmlMix(h, (uint64_t)v->count);
    h = AmlMix(h, (uint64_t)(uintptr_t)v->scope);
    h = AmlMix(h, (uint64_t)(uintptr_t)v->node);
    return h;
}

/*
 * ‼ EVERYTHING A TURN OF A While CAN HAVE CHANGED, IN ONE NUMBER.
 *
 * The eight Locals and seven Args of §20.2.6, every object the method
 * declared for itself and every byte of every buffer among them, and the fold
 * of this turn's dealings with everything outside the activation. That list
 * is not a selection: it is the whole of the state this evaluator's next step
 * reads. The block cursor is not in it because the turn always begins at the
 * same byte — that is what makes it a turn.
 *
 * Never zero, because zero is the empty slot of the table this goes into.
 */
static uint64_t AmlTurnMark(const AmlAct *a, const AmlBlock *loop) {
    uint64_t h = 0xCBF29CE484222325ULL;          /* FNV-1a's offset basis */

    for (uint32_t i = 0; i < AML_LOCAL_COUNT; i++)
        h = AmlValueMark(h, &a->local[i]);
    for (uint32_t i = 0; i < AML_ARG_COUNT; i++)
        h = AmlValueMark(h, &a->arg[i]);

    h = AmlMix(h, a->objn);
    for (uint32_t i = 0; i < a->objn; i++) {
        const AmlLocalObj *o = &a->obj[i];
        h = AmlMix(h, (uint64_t)(uint8_t)o->name[0]        |
                      ((uint64_t)(uint8_t)o->name[1] << 8)  |
                      ((uint64_t)(uint8_t)o->name[2] << 16) |
                      ((uint64_t)(uint8_t)o->name[3] << 24));
        h = AmlMix(h, (uint64_t)(o->is_field ? 1u : 0u) |
                      ((uint64_t)o->over << 1));
        h = AmlMix(h, ((uint64_t)o->bit_offset << 32) | o->bit_width);
        h = AmlValueMark(h, &o->value);
        h = AmlMix(h, o->buf_len);
        for (uint32_t b = 0; b < o->buf_len; b++) h = AmlMix(h, o->buf[b]);
    }

    h = AmlMix(h, loop->world);
    h = AmlMix(h, loop->worldn);
    return h ? h : 1ULL;
}

/* Remember one turn. `*again` comes back true when this loop has stood in
 * exactly this state before; false means the state is new and is now kept.
 * False FROM THE FUNCTION means the heap would not hold it. */
static bool AmlSeenAdd(AmlSeen *s, uint64_t fp, bool *again) {
    *again = false;

    /* Kept at half full: a table that fills cannot be probed. */
    if ((s->used + 1u) * 2u > s->room) {
        uint32_t  bigger = s->room ? s->room * 2u : 16u;
        uint64_t *fresh  = (uint64_t *)kmalloc((size_t)bigger * sizeof(uint64_t));
        if (!fresh) return false;
        memset(fresh, 0, (size_t)bigger * sizeof(uint64_t));
        for (uint32_t i = 0; i < s->room; i++) {
            uint64_t v = s->slot[i];
            if (!v) continue;
            uint32_t j = (uint32_t)(v & (uint64_t)(bigger - 1u));
            while (fresh[j]) j = (j + 1u) & (bigger - 1u);
            fresh[j] = v;
        }
        if (s->slot) kfree(s->slot);
        s->slot = fresh;
        s->room = bigger;
    }

    uint32_t i = (uint32_t)(fp & (uint64_t)(s->room - 1u));
    while (s->slot[i]) {
        if (s->slot[i] == fp) { *again = true; return true; }
        i = (i + 1u) & (s->room - 1u);
    }
    s->slot[i] = fp;
    s->used++;
    return true;
}

/* ===========================================================
 * Values held by namespace objects
 * =========================================================== */

/* The value a namespace object HOLDS. A Method is not one of them: reading a
 * method means running it, and that is the machine's business, not a
 * dereference buried inside a lookup. A Field is not one either — reading it
 * is an address-space access, which goes through AmlRegionRead and its
 * window. */
static bool AmlValueOfNode(aml_object_t *node, const char *what,
                            AmlValue *out) {
    memset(out, 0, sizeof(*out));
    out->node = node;
    switch (node->type) {
    case AML_OBJ_INTEGER:
        out->type    = AML_OBJ_INTEGER;
        out->integer = node->v.integer & AmlIntMask();
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
 * One element of a Package, or one literal, read with no method frame around
 * it and no dereferencing: a NameString here yields the OBJECT, because that
 * is what a _PRT entry's Source field means and dereferencing it would throw
 * away the very thing being asked for. Nothing in here can reach hardware.
 */
static bool AmlReadOperand(const uint8_t **p, const uint8_t *end,
                            aml_object_t *scope, const char *what,
                            AmlValue *out) {
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
        out->integer = v & AmlIntMask();
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
         * is there, so the end is the smaller of the two. */
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
        out->node = o;
        return true;
    }

    kprintf("[AML] %s: opcode 0x%02x in operand position is not one this "
            "evaluator reads\n", what, op);
    return false;
}

/* =====================================================================
 * ADDRESS SPACES — the three this kernel can reach, and the rest it
 * refuses by name.
 * ===================================================================== */

/* The names are ACPI 6.5 Table 5.182, "Operation Region Address Space
 * Identifiers" (§5.5.2.4.1; §19.6.99 OperationRegion sends the reader to it:
 * "The predefined Operation Region types specified in ACPI are shown in
 * Table 5.182").
 *
 * ‼ THE RANGE ABOVE THE NAMED ONES IS NOT ALL THE VENDOR'S, AND THE TABLE IS
 * WHERE THAT IS WRITTEN DOWN. This comment used to cite §20.2.5.2 for a
 * sentence reserving 0x0B..0x7F to the specification. That sentence is not
 * there: §20.2.5.2 is AML grammar, and its RegionSpace production lists only
 * 0x00 SystemMemory .. 0x0A PCC and "0x80-0xFF: OEM Defined" — no reserved
 * range, no 0x0B, no 0x7F. Table 5.182 is the list that has them, and it
 * divides the space differently from what stood here: 0x0B is
 * PlatformRtMechanism, 0x0C-0x7E is Reserved, 0x7F is FFixedHW — functional
 * fixed hardware, an ACPI-defined space with a section of its own
 * (§5.5.2.4.2) — and only 0x80 to 0xFF is OEM defined.
 *
 * The difference is not academic: calling 0x7F "the board vendor's" sends an
 * operator reading a refusal on a live board after a vendor document that
 * does not exist, when the specification names that space itself. Firmware
 * that publishes _CPC or another CPPC interface declares FFixedHW regions,
 * so this is a code a real board hands us. */
static const char *AmlSpaceName(uint8_t space) {
    switch (space) {
    case 0x00: return "SystemMemory";
    case 0x01: return "SystemIO";
    case 0x02: return "PCI_Config";
    case 0x03: return "EmbeddedControl";
    case 0x04: return "SMBus";
    case 0x05: return "SystemCMOS";
    case 0x06: return "PciBarTarget";
    case 0x07: return "IPMI";
    case 0x08: return "GeneralPurposeIO";
    case 0x09: return "GenericSerialBus";
    case 0x0A: return "PCC";
    case 0x0B: return "PlatformRtMechanism";
    case 0x7F: return "FFixedHW";
    default:   return NULL;
    }
}

/*
 * ‼ WHY EXACTLY ONE ADDRESS SPACE MAY BE WRITTEN, AND WHAT A WRITE COSTS IN
 *   EACH OF THE OTHERS.
 *
 * The narrow gate below lets \_PIC store, and the question it has to answer
 * first is not "is this space convenient" but "does a store here ACT on
 * something". Reading is allowed in three spaces because reading a register
 * is, on the routing path, an observation. Writing is not symmetrical: an
 * address space is a bus, and in every space below except SystemMemory a
 * write is a command that has already been obeyed by the time the
 * instruction retires.
 *
 * SystemMemory is the one exception, and not because it is "memory" — a
 * SystemMemory region is just as often an MMIO window (`OperationRegion
 * (RCRB, SystemMemory, \RCBA, 0x4000)` is the Intel chipset form, and a PCH's
 * power-management and GPIO registers live behind exactly that). What makes
 * the difference is not the space, it is the ADDRESS, and the only thing on
 * this machine that knows a device window from firmware scratch RAM is the
 * firmware's own memory map. AmlNvsHolds asks it. So the rule this file
 * enforces is narrower than "SystemMemory": a store lands only where the
 * board itself declared AddressRangeNVS.
 *
 * Each sentence below is printed in place of the write, so that a board this
 * kernel refuses says WHY on its own console rather than going quiet.
 */
static const char *AmlSpaceWriteReason(uint8_t space) {
    switch (space) {
    case 0x01: return
        "an OUT instruction is an action on a device the moment it retires. "
        "Port 0xB2 raises a System Management Interrupt and runs firmware "
        "this kernel cannot see or interrupt; the 8237's byte-pointer "
        "flip-flop moves; an index port moves the window that the next read "
        "answers from. Nothing in a region's offset says which of those a "
        "port is";
    case 0x02: return
        "a configuration write MOVES a device. A Base Address Register "
        "relocates its window, the Command register turns its decode on or "
        "off, a bridge's Secondary Bus number re-parents every function "
        "below it — and a region offset is a byte number, which says nothing "
        "about which of those it lands on";
    case 0x03: return
        "a write is a command handed to the embedded controller's own "
        "processor over a handshake protocol (ACPI 6.5 §12) that this kernel "
        "does not speak in either direction";
    case 0x04: return
        "a write is a bus transaction addressed at another device entirely — "
        "the memory modules' SPD, the battery, the clock generator";
    case 0x05: return
        "the CMOS index port carries the NMI mask in its top bit, so writing "
        "CMOS at all decides whether this machine can still take a "
        "non-maskable interrupt; and the bytes behind it are the RTC "
        "driver's, which owns that pair of ports";
    case 0x06: return
        "an offset inside a Base Address Register's window is a device "
        "register by definition — the space exists to name one";
    case 0x07: return
        "a write is a request message to the baseboard management "
        "controller, which acts on it independently of this processor";
    case 0x08: return
        "a write drives a physical pin, and nothing in the namespace says "
        "whether that pin is a status LED or a device's reset line";
    case 0x09: return
        "a write is a transaction on a serial bus, addressed at a device "
        "this kernel has not enumerated and cannot name";
    case 0x0A: return
        "a write rings a doorbell that sets firmware running on another "
        "processor, and the reply comes back when that firmware is finished";
    case 0x0B: return
        "a write invokes a platform runtime service, which is firmware "
        "execution under another name";
    case 0x7F: return
        "functional fixed hardware is reached by an instruction sequence the "
        "processor architecture defines, not by an address — ACPI 6.5 "
        "§5.5.2.4.2 leaves the syntax of an FFixedHW region architecture "
        "specific and refers the reader out of the specification for it. A "
        "region offset here names nothing this evaluator can carry out";
    default:
        if (space >= 0x0C && space <= 0x7E) return
            "ACPI 6.5 Table 5.182 marks this code Reserved and has not yet "
            "said what it means, so what a write would do is not written "
            "down anywhere";
        return
            "this code belongs to the board vendor, and what a write would do "
            "is written down nowhere this kernel can read";
    }
}

/* Where in PCI configuration space a PCI_Config region actually sits.
 *
 * A PCI_Config OperationRegion is addressed relative to the PCI device it is
 * DECLARED UNDER — the nearest enclosing Device with an _ADR — on the bus and
 * segment of the PCI root bridge above it; the region's own base is then a
 * byte offset within that function's configuration space. That rule is
 * assembled out of two clauses rather than quoted from one. ACPI 6.5 §6.1.1:
 * "OSPM infers the parent bus and segment from the location of the _ADR
 * object's device package in the ACPI namespace", with Table 6.2 giving the
 * PCI encoding as "High word-Device #, Low word-Function #"; and §19.6.99
 * makes the OperationRegion's Offset "the offset within the selected
 * RegionSpace at which the region starts (byte-granular)". (§5.5.2.4.2 was
 * cited here for the whole rule and says none of it — it is "Declaring
 * Functional Fixed Hardware (FFH) Operation Regions", about the 0x7F space.)
 *
 * ‼ _ADR IS READ AS A STATIC OBJECT HERE AND ONLY AS A STATIC OBJECT, and
 * that is deliberate rather than lazy. Running a method to find out where to
 * perform an address-space access could need another address-space access to
 * find out where to run it, and there is no bottom to that. §6.1.1 is on the
 * same side: "_ADR object information must be static". Every PCI device on
 * every board writes the object, and a board that writes a method instead is
 * told so by name rather than followed into a circle.
 */
static bool AmlPciConfigSite(aml_object_t *region, const char *who,
                              uint16_t *seg, uint8_t *bus,
                              uint8_t *dev, uint8_t *fn);

/*
 * WHERE A REGION STARTS AND HOW LONG IT IS, when the firmware did not write
 * either as a number. Declared here, defined below the machine it runs the
 * firmware's own term on.
 *
 * ‼ AND IT RUNS THAT TERM WITH NO WINDOW, WHICH IS WHAT KEEPS THIS ONE
 *   LEVEL DEEP INSTEAD OF UNBOUNDED. Resolving an address must not itself
 *   become an address-space access: a base computed by reading another
 *   region would need that region's base, and there is no bottom to that.
 *   The evaluation gets its OWN AmlRun, whose window is false because
 *   AmlRunInit zeroes it and only AmlEvalIn ever assigns it — so a Field
 *   standing in a base term is refused by AmlRegionRead's first line,
 *   BEFORE it can ask for another base. One nested evaluation, never two,
 *   and the bound is the window rather than a depth counter.
 */
static bool AmlRegionArm(aml_object_t *rg, const char *who);

/* One access of `width` bytes at `offset` inside a PCI function's
 * configuration space. pci.c owns both doors — the 0xCF8/0xCFC pair and the
 * ECAM window — and this asks it for them rather than opening either. */
static bool AmlPciUnitRead(uint16_t seg, uint8_t bus, uint8_t dev, uint8_t fn,
                            uint32_t offset, uint32_t width, const char *who,
                            uint64_t *out) {
    /* PCI Express Base Specification: a function's configuration space is
     * 4096 bytes, of which PCI 3.0 defines the first 256. */
    if (offset + width > 4096u) {
        kprintf("[AML] %s: an operation region would read configuration byte "
                "%lu of %02x:%02x.%u, and a PCI function's configuration "
                "space ends at 4096\n",
                who, (unsigned long)offset, bus, dev, fn);
        return false;
    }
    /* ‼ WHICH DOOR, AND WHY THE SEGMENT DECIDES IT AS MUCH AS THE OFFSET.
     * The 0xCF8/0xCFC pair addresses bus, device, function and eight bits of
     * offset — and NOTHING ELSE. It has no segment field, so every access
     * through it lands in segment 0 whatever segment was asked for. A board
     * with more than one PCI segment group (the MCFG lists them; multi-root
     * server and workstation boards ship them) declares its root bridge with
     * _SEG != 0, and taking the legacy door there reads another segment's
     * device and stamps the answer "the ACPI routing table". So the segment
     * sends the access through ECAM exactly as an offset past 256 does, and
     * a machine that published no MCFG cannot perform it at all. */
    if (offset + width > 256u || seg != 0) {
        if (!pci_has_ecam()) {
            if (seg != 0)
                kprintf("[AML] %s: an operation region reads configuration "
                        "byte %lu of %04x:%02x:%02x.%u, and the 0xCF8/0xCFC "
                        "door carries no segment number — this machine "
                        "published no MCFG, so segment %u cannot be reached "
                        "at all\n",
                        who, (unsigned long)offset, seg, bus, dev, fn, seg);
            else
                kprintf("[AML] %s: an operation region reaches configuration "
                        "byte %lu of %02x:%02x.%u, which is past the 256 "
                        "bytes the 0xCF8/0xCFC door can address, and this "
                        "machine published no MCFG for the ECAM window\n",
                        who, (unsigned long)offset, bus, dev, fn);
            return false;
        }
        switch (width) {
        case 1: *out = pci_ecam_read_byte(seg, bus, dev, fn,
                                          (uint16_t)offset); return true;
        case 2: *out = pci_ecam_read_word(seg, bus, dev, fn,
                                          (uint16_t)offset); return true;
        case 4: *out = pci_ecam_read_dword(seg, bus, dev, fn,
                                           (uint16_t)offset); return true;
        case 8: *out = (uint64_t)pci_ecam_read_dword(seg, bus, dev, fn,
                                                     (uint16_t)offset) |
                       ((uint64_t)pci_ecam_read_dword(seg, bus, dev, fn,
                                          (uint16_t)(offset + 4)) << 32);
                return true;
        default: break;
        }
    } else {
        switch (width) {
        case 1: *out = pci_config_read_byte(bus, dev, fn,
                                            (uint8_t)offset); return true;
        case 2: *out = pci_config_read_word(bus, dev, fn,
                                            (uint8_t)offset); return true;
        case 4: *out = pci_config_read_dword(bus, dev, fn,
                                             (uint8_t)offset); return true;
        case 8: *out = (uint64_t)pci_config_read_dword(bus, dev, fn,
                                             (uint8_t)offset) |
                       ((uint64_t)pci_config_read_dword(bus, dev, fn,
                                             (uint8_t)(offset + 4)) << 32);
                return true;
        default: break;
        }
    }
    kprintf("[AML] %s: a field asks for a %lu-byte configuration access, and "
            "PCI defines byte, word and dword\n", who, (unsigned long)width);
    return false;
}

/* One access of `width` bytes at `port`. */
static bool AmlIoUnitRead(uint64_t port, uint32_t width, const char *who,
                           uint64_t *out) {
    if (port + width > 0x10000u) {
        kprintf("[AML] %s: an operation region would read I/O port 0x%lx, and "
                "the x86 I/O space ends at 0xFFFF\n",
                who, (unsigned long)port);
        return false;
    }
    switch (width) {
    case 1: *out = inb((uint16_t)port); return true;
    case 2: *out = inw((uint16_t)port); return true;
    case 4: *out = inl((uint16_t)port); return true;
    default: break;
    }
    /* Intel SDM Vol 1 §18.2: IN and OUT move a byte, a word or a doubleword.
     * There is no eight-byte port access to perform. */
    kprintf("[AML] %s: a field declares a %lu-byte I/O access, and the x86 "
            "I/O instructions move one, two or four bytes\n",
            who, (unsigned long)width);
    return false;
}

/*
 * HOW FAR INTO ITS REGION ONE FIELD REACHES, and at what granularity the
 * hardware has to be touched to reach it. The same geometry answers a read
 * and a write, and it is computed in one place so that the bound between
 * this evaluator and the bus is one piece of arithmetic rather than two that
 * have to be kept equal by hand.
 *
 * ACPI 6.5 §20.2.5.2 makes a Field a bit offset and a bit width inside the
 * region, with an access width the FieldFlags byte names, and §19.6.47
 * (Field) makes that width the granularity the hardware is touched at:
 * a DWordAcc field is read with 32-bit accesses even when it is one bit wide,
 * because the register behind it answers no other way.
 */
typedef struct AmlFieldReach {
    uint32_t access;        /* bytes per access unit */
    uint32_t bits;          /* how many bits the field is */
    uint32_t lo;            /* where it starts inside the first access unit */
    uint64_t first;         /* first byte of the region it touches */
    uint64_t last;          /* one past the last byte it touches */
} AmlFieldReach;

static bool AmlFieldReachOf(aml_object_t *field, aml_object_t *rg,
                             const char *who, AmlFieldReach *out) {
    /* §20.2.5.2, FieldFlags bits[3:0] — AccessType. AnyAcc and BufferAcc name
     * no width of their own; ACPICA resolves both to byte access and so does
     * this, because a byte access is the only one that is legal at every
     * offset in all three of these spaces. */
    uint32_t aw;
    switch (field->v.field.access & 0x0Fu) {
    case 0x00: aw = 1; break;            /* AnyAcc    */
    case 0x01: aw = 1; break;            /* ByteAcc   */
    case 0x02: aw = 2; break;            /* WordAcc   */
    case 0x03: aw = 4; break;            /* DWordAcc  */
    case 0x04: aw = 8; break;            /* QWordAcc  */
    case 0x05: aw = 1; break;            /* BufferAcc */
    default:
        kprintf("[AML] %s: field %s declares access type %u, which "
                "§20.2.5.2 does not define\n",
                who, field->name, field->v.field.access & 0x0Fu);
        return false;
    }

    uint32_t bits = field->v.field.bit_width;
    if (bits == 0 || bits > 64) {
        /* §19.6.47: "If the FieldUnit is larger than the size of an Integer,
         * it will be treated as a Buffer" — and nothing on the routing path
         * asks for one. Said rather than truncated. */
        kprintf("[AML] %s: field %s is %lu bits wide; this evaluator carries "
                "a field as an integer, and an integer on this machine is "
                "%lu bits\n", who, field->name,
                (unsigned long)bits, (unsigned long)g_int_bits);
        return false;
    }

    /* ‼ IN 64 BITS, BECAUSE BOTH OPERANDS ARE THE FIRMWARE'S. A field's bit
     * offset is accumulated from PkgLengths the table wrote, each of which
     * §20.2.4 lets reach 0x0FFFFFFF; a sum of them that wraps a 32-bit
     * register would come out SMALL, pass the length check below, and send
     * the access somewhere else entirely. `last` is the single bound between
     * this evaluator and the bus, so it is computed where it cannot wrap. */
    uint64_t awb = (uint64_t)aw * 8u;
    out->access = aw;
    out->bits   = bits;
    out->lo     = (uint32_t)((uint64_t)field->v.field.bit_offset % awb);
    out->first  = ((uint64_t)field->v.field.bit_offset / awb) * aw;
    out->last   = (((uint64_t)field->v.field.bit_offset + bits + awb - 1u)
                   / awb) * aw;

    /* The region's own length is the bound, and it is the FIRMWARE's number,
     * not one written here. A region whose length this pass could not read
     * bounds nothing, and a field read against no bound is a read at an
     * address nobody stated. */
    if (rg->v.region.len == 0) {
        kprintf("[AML] %s: operation region %s was declared with a length of "
                "zero, so nothing bounds an access inside it — refused\n",
                who, rg->name);
        return false;
    }
    if (out->last > rg->v.region.len) {
        kprintf("[AML] %s: field %s ends at byte %lu of operation region %s, "
                "which the firmware declared %lu byte(s) long\n",
                who, field->name, (unsigned long)out->last, rg->name,
                (unsigned long)rg->v.region.len);
        return false;
    }
    return true;
}

/*
 * The heart of it: the value of one Field, read out of the address space its
 * OperationRegion names.
 */
static bool AmlRegionRead(AmlRun *run, aml_object_t *field, const char *who,
                           uint64_t *out) {
    /* ‼ THE WINDOW. Nothing below this line runs outside one. */
    if (!run->window) {
        kprintf("[AML] %s: reading %s means touching the address space its "
                "operation region names, and this kernel opens one only while "
                "it is evaluating an object that describes the machine "
                "(_PRT, _CRS, _PRS, _STA, _PIC, _HID, _CID) — refused\n",
                who, field->name);
        return false;
    }

    /* ‼ AN INDEXED OR BANKED FIELD IS A WRITE BEFORE IT IS A READ, and the
     * one write this kernel performs is a single store into firmware scratch
     * RAM, not a protocol. §20.2.5.2: an IndexField selects what the Data
     * register answers by STORING into the Index register first, and a
     * BankField by storing its BankValue into the bank selector — two
     * accesses in a fixed order, into a register chosen by the firmware and
     * not by this kernel. Both are refused here rather than half-attempted. */
    if (field->v.field.index || field->v.field.data) {
        kprintf("[AML] %s: %s is an IndexField — choosing what its data "
                "register answers means WRITING its index register first, "
                "and two accesses in a fixed order is a protocol this kernel "
                "does not run\n",
                who, field->name);
        return false;
    }
    if (field->v.field.bank) {
        kprintf("[AML] %s: %s is a BankField — reading it means WRITING %lu "
                "into its bank selector %s first, and two accesses in a "
                "fixed order is a protocol this kernel does not run\n",
                who, field->name, (unsigned long)field->v.field.bank_value,
                field->v.field.bank->name);
        return false;
    }

    aml_object_t *rg = field->v.field.region;
    if (!rg || rg->type != AML_OBJ_REGION) {
        kprintf("[AML] %s: %s is a field of something this pass never recorded "
                "as an operation region\n", who, field->name);
        return false;
    }

    const char *space = AmlSpaceName(rg->v.region.space);
    if (!space) {
        /* Two different unknowns, and the operator is told which one he has:
         * ACPI 6.5 Table 5.182 marks 0x0C-0x7E Reserved — nobody has said
         * what it means yet — while 0x80 to 0xFF is the board vendor's, and
         * only in that second case is there a vendor document to go and look
         * for. */
        kprintf("[AML] %s: operation region %s lives in address space 0x%02x, "
                "which %s\n",
                who, rg->name, rg->v.region.space,
                rg->v.region.space >= 0x80
                    ? "belongs to the board vendor and is written down "
                      "nowhere this kernel can read"
                    : "ACPI 6.5 Table 5.182 marks Reserved — the "
                      "specification has not yet said what lives there");
        return false;
    }
    if (rg->v.region.space > 0x02) {
        kprintf("[AML] %s: operation region %s lives in %s, which is reached "
                "over a transport protocol of its own. This kernel reaches "
                "SystemMemory, SystemIO and PCI_Config, which is where every "
                "interrupt-routing register on every board so far has been — "
                "refused by name rather than guessed at\n",
                who, rg->name, space);
        return false;
    }

    /* ‼ THE FIRMWARE'S OWN NUMBERS FIRST, and never a substitute for one.
     * A region whose base or length is a term rather than a literal has it
     * evaluated here, once, before anything is touched — and a region that
     * cannot produce both is refused aloud below without a single access. */
    if (!AmlRegionArm(rg, who)) return false;

    /* ‼ THE REGION SAYS WHAT IT IS, ONCE. On a board where something does not
     * add up, this is the only line that says where the firmware reached, and
     * it is kprintf for that reason. */
    if (!rg->v.region.said) {
        rg->v.region.said = true;
        kprintf("[AML] operation region %s, declared under %s: %s, base "
                "0x%lx, length %lu byte(s) — read from for the first time now, "
                "for %s\n",
                rg->name, rg->parent ? rg->parent->name : "the root", space,
                (unsigned long)rg->v.region.base,
                (unsigned long)rg->v.region.len,
                run->window_for ? run->window_for : who);
    }

    AmlFieldReach at;
    if (!AmlFieldReachOf(field, rg, who, &at)) return false;
    uint32_t aw    = at.access;
    uint32_t bits  = at.bits;
    uint64_t first = at.first;
    uint64_t last  = at.last;
    uint64_t awb   = (uint64_t)aw * 8u;

    /* PCI_Config resolves to a function once, before any access. Its offsets
     * are checked here as 64-bit quantities, because the per-access check
     * below sees them already narrowed and a base past four kilobytes would
     * wrap into a legal-looking one. */
    uint16_t seg = 0; uint8_t bus = 0, dev = 0, fn = 0;
    if (rg->v.region.space == 0x02) {
        if (rg->v.region.base + last > 4096u) {
            kprintf("[AML] %s: operation region %s puts configuration byte "
                    "%lu inside a PCI function, whose configuration space "
                    "PCI Express makes 4096 bytes long\n",
                    who, rg->name,
                    (unsigned long)(rg->v.region.base + last));
            return false;
        }
        if (!AmlPciConfigSite(rg, who, &seg, &bus, &dev, &fn)) return false;
    }

    /* SystemMemory is MAPPED FOR THE LENGTH OF THIS READ AND UNMAPPED BEFORE
     * THIS FUNCTION RETURNS — on every path, including every refusal below,
     * because there is exactly one return point past this line. Read-only: a
     * page this kernel cannot write is a page no later defect can write
     * either, and the mapping is uncacheable because vmm_map_mmio makes every
     * mapping it hands out uncacheable. */
    volatile uint8_t *va = NULL;
    size_t            va_len = 0;
    if (rg->v.region.space == 0x00) {
        va_len = (size_t)(last - first);
        va = (volatile uint8_t *)vmm_map_mmio(
                 (uintptr_t)(rg->v.region.base + first), va_len,
                 VMM_FLAGS_KERNEL_RO);
        if (!va) {
            kprintf("[AML] %s: operation region %s asks for system memory at "
                    "0x%lx, and the mapper would not hand out a read-only "
                    "window onto it\n",
                    who, rg->name,
                    (unsigned long)(rg->v.region.base + first));
            return false;
        }
    }

    bool     ok    = true;
    uint64_t value = 0;
    uint32_t done  = 0;
    uint32_t lo    = at.lo;

    for (uint64_t off = first; done < bits && ok; off += aw) {
        uint64_t unit = 0;
        switch (rg->v.region.space) {
        case 0x00:
            switch (aw) {
            case 1: unit = va[(size_t)(off - first)]; break;
            case 2: unit = *(volatile uint16_t *)(va + (size_t)(off - first));
                    break;
            case 4: unit = *(volatile uint32_t *)(va + (size_t)(off - first));
                    break;
            default: unit = *(volatile uint64_t *)(va + (size_t)(off - first));
                    break;
            }
            break;
        case 0x01:
            ok = AmlIoUnitRead(rg->v.region.base + off, aw, who, &unit);
            break;
        default:
            ok = AmlPciUnitRead(seg, bus, dev, fn,
                                (uint32_t)(rg->v.region.base + off), aw,
                                who, &unit);
            break;
        }
        if (!ok) break;

        uint32_t take = (uint32_t)awb - lo;
        if (take > bits - done) take = bits - done;
        uint64_t mask = (take == 64) ? ~0ULL : ((1ULL << take) - 1ULL);
        value |= ((unit >> lo) & mask) << done;
        done  += take;
        lo     = 0;
    }

    if (va) vmm_unmap_mmio((volatile void *)va, va_len);
    if (!ok) return false;
    *out = value & AmlIntMask();
    /* ‼ A LOOP WAITING ON THIS REGISTER IS TOLD WHAT IT ANSWERED. A While
     * that spins on a field is only getting somewhere while the field's
     * answer changes, and this is the only place in the file that knows what
     * it answered. */
    AmlRunHeard(run, AmlMix((uint64_t)(uintptr_t)field, *out));
    return true;
}

/* =====================================================================
 * THE ONE PLACE THIS KERNEL WRITES AN ADDRESS SPACE.
 *
 * ‼ AND IT IS ONE PLACE ON PURPOSE. What used to be true of this file is
 * that there was NO function that wrote an address space, so "reads only"
 * needed no guard: there was nothing to call. That is no longer true, and
 * what replaces it is not a flag somebody remembers to check but three
 * conditions that have to hold at once, each enforced somewhere it cannot be
 * skipped:
 *
 *   1. THE METHOD WAS PROVEN WHOLE FIRST. The narrow gate (AmlGateProve,
 *      below) walks every byte of \_PIC and of every method it calls, takes
 *      BOTH sides of every If, and refuses on anything this evaluator cannot
 *      perform — including every store whose target fails condition 3. Only
 *      a method that came out the far end is run at all. What that excludes
 *      before the first instruction, rather than discovering half-way, is a
 *      method whose TEXT this evaluator cannot carry. What it cannot exclude
 *      is one that runs onto a value of the wrong type, because the walk has
 *      no values — the gate says so itself, at length, and the count of
 *      stores already landed is what reports that residue out loud.
 *   2. IT IS \_PIC, AND THE RUN IS THE ONE THE GATE OPENED FOR. `may_write`
 *      is a field of AmlRun, assigned in exactly one place — AmlEvalIn — from
 *      the name of the object being evaluated AND from a `proven` flag that
 *      only AmlRoutingAdopt sets, after the gate returned. AmlRun is a local
 *      variable of whoever asked the question, so the permission dies when
 *      that C function returns, on every path including every refusal.
 *   3. THE ADDRESS IS FIRMWARE SCRATCH RAM AND NOT A DEVICE. See
 *      AmlSpaceWriteReason for the ten spaces refused by name, and
 *      AmlNvsHolds for the eleventh, where the firmware's own memory map is
 *      what decides.
 * ===================================================================== */

/*
 * Does the board's own memory map call this whole range ACPI NVS?
 *
 * ‼ THIS IS THE WHOLE OF THE WRITE PERMISSION, AND IT IS THE MACHINE'S
 *   STATEMENT, NOT A RANGE WRITTEN HERE.
 *
 * "SystemMemory" is not enough and never was: half the SystemMemory regions
 * on a shipping board are MMIO windows — the PCH's root complex register
 * block, its power-management registers, its GPIO community — and a store
 * into one of those is a device action wearing memory's clothes. The one
 * thing on this machine that can tell them apart is the map the firmware
 * itself published, and it publishes exactly the distinction that is needed.
 *
 * ACPI 6.5 Table 15.1 "Address Range Types" — which stands in the opening of
 * chapter 15, above §15.1, not inside it — value 4: "AddressRangeNVS ... ACPI
 * NVS Memory. This range of addresses is in use or reserved by the system and
 * must not be used by the operating system. This range is required to be
 * saved and restored across an NVS sleep." §15.3, Table 15.6 maps
 * UEFI's EfiACPIMemoryNVS onto that same type, and both of this kernel's
 * loaders write it through as E820_ACPI_NVS.
 *
 * Read the second sentence: a range whose CONTENTS have to be saved and put
 * back is a range whose contents are the point — it holds state, which is
 * what memory is and what a device window is not. A device's registers are
 * not saved and restored; they are re-programmed. So the firmware, by
 * marking a range NVS, has said "this is RAM I set aside", and GNVS — the
 * block every Intel reference ASL passes values through, and the block
 * \_PIC's GPIC lives in — is precisely that block.
 *
 * A range the map does not call NVS is refused however plausible it looks.
 * That includes AddressRangeMemory, which is the OS's own heap, and
 * AddressRangeACPI, which is where the DSDT being interpreted right now is
 * sitting.
 *
 * The whole range must lie inside ONE entry. A range that straddles two
 * entries is one whose far end the map describes differently, and half a
 * store is worse than none.
 */
static bool AmlNvsHolds(uint64_t base, uint64_t len) {
    if (len == 0) return false;
    uint64_t end = base + len;
    if (end < base) return false;                 /* wrapped */

    const e820_entry_t *m = memory_map_get_entries();
    size_t n = memory_map_get_entry_count();
    if (!m || n == 0) return false;

    for (size_t i = 0; i < n; i++) {
        if (m[i].type != E820_ACPI_NVS || m[i].length == 0) continue;
        uint64_t stop = m[i].base + m[i].length;
        if (stop <= m[i].base) continue;          /* wrapped entry */
        if (base >= m[i].base && end <= stop) return true;
    }
    return false;
}

/*
 * May a store into this Field be carried out at all — asked with no side
 * effect, so that the gate can ask it about every store in a method BEFORE
 * any of them happens, and the writer can ask it again about the store it is
 * about to make.
 *
 * On success `*at` and `*phys` say exactly which bytes would move, which is
 * what the writer needs and what the log line prints.
 *
 * ‼ AND IT REFUSES A FIELD THAT DOES NOT FILL WHOLE ACCESS UNITS, which is
 * the one restriction here that is about this kernel rather than about the
 * platform. §20.2.5.2's FieldFlags carry an UpdateRule in bits[6:5] —
 * Preserve, WriteAsOnes, WriteAsZeros — that says what happens to the bits
 * AROUND a partial field, and an AccessField element inside a FieldList
 * replaces the byte this pass records, taking those two bits with it. So
 * this kernel does not know the update rule for every field, and rather than
 * pick one it writes only fields that have no surrounding bits to rule on.
 * The gain is not only correctness: a whole-unit store is a pure store, so
 * placing a value never has to READ the register first, and a register whose
 * read has a side effect is never read on the way to writing it.
 */
static bool AmlFieldWritable(aml_object_t *field, const char *who,
                              AmlFieldReach *at, uint64_t *phys) {
    if (field->v.field.index || field->v.field.data) {
        kprintf("[AML] %s: %s is an IndexField, so placing a value in it "
                "means two writes in a fixed order — its index register "
                "first, its data register after. That is a protocol, not a "
                "store, and this kernel does not run it\n",
                who, field->name);
        return false;
    }
    if (field->v.field.bank) {
        kprintf("[AML] %s: %s is a BankField, so placing a value in it means "
                "writing %lu into its bank selector %s first — two writes in "
                "a fixed order, which is a protocol this kernel does not "
                "run\n", who, field->name,
                (unsigned long)field->v.field.bank_value,
                field->v.field.bank->name);
        return false;
    }

    aml_object_t *rg = field->v.field.region;
    if (!rg || rg->type != AML_OBJ_REGION) {
        kprintf("[AML] %s: %s is a field of something this pass never "
                "recorded as an operation region\n", who, field->name);
        return false;
    }

    const char *space = AmlSpaceName(rg->v.region.space);
    if (rg->v.region.space != 0x00) {
        kprintf("[AML] %s: %s is a field of operation region %s, which lives "
                "in %s, and %s — so no value is placed in it\n",
                who, field->name, rg->name,
                space ? space : "an address space this kernel has no name for",
                AmlSpaceWriteReason(rg->v.region.space));
        return false;
    }

    /* Where the region starts has to be known before anything can be said
     * about what is at that address, and working it out reads no address
     * space — AmlRegionArm runs the firmware's term on a run of its own,
     * whose window is shut. */
    if (!AmlRegionArm(rg, who)) return false;
    if (!AmlFieldReachOf(field, rg, who, at)) return false;

    uint64_t awb = (uint64_t)at->access * 8u;
    if (at->lo != 0 || (uint64_t)at->bits % awb != 0u) {
        kprintf("[AML] %s: field %s occupies bits %lu to %lu of a %lu-bit "
                "access unit rather than whole units, and what happens to the "
                "bits beside it is the UpdateRule of §20.2.5.2, which this "
                "pass does not record once a FieldList has changed its access "
                "rules. No value is placed in a field whose neighbours this "
                "kernel would have to guess at\n",
                who, field->name, (unsigned long)at->lo,
                (unsigned long)(at->lo + at->bits - 1u), (unsigned long)awb);
        return false;
    }

    *phys = rg->v.region.base + at->first;
    if (!AmlNvsHolds(*phys, at->last - at->first)) {
        kprintf("[AML] %[E]%s: field %s would place a value at physical "
                "0x%lx, and this machine's own memory map does not call those "
                "%lu byte(s) ACPI NVS. A SystemMemory region is as often a "
                "device's register window as it is firmware scratch RAM — the "
                "map is the only thing that tells them apart, and it has not "
                "said memory here. Nothing is written%[D]\n",
                who, field->name, (unsigned long)*phys,
                (unsigned long)(at->last - at->first));
        return false;
    }
    return true;
}

/*
 * Place one integer in one Field. The only function in this file that writes
 * anything outside this kernel's own memory.
 *
 * The mapping is uncacheable, which is not a choice made here: vmm_map_mmio
 * hands out nothing else, and it happens to be the right thing — the Pull Map
 * gives a write-back leaf only to a span that is E820 USABLE end to end, so
 * an ACPI NVS page's standing alias is uncacheable too, and the two agree.
 * Intel SDM Vol 3A §12.12.4, "Programming the PAT", names two aliases of
 * different memory type onto one physical page as the thing a program must
 * not do — "Intel does not support this practice because it may lead to
 * undefined operations that can result in a system failure"; here there are
 * not two types. (This said §11.12.4, which was the number before chapter 11
 * became the APIC and memory cache control moved to chapter 12.)
 */
static bool AmlRegionWrite(AmlRun *run, aml_object_t *field, const char *who,
                            uint64_t value) {
    if (!run->may_write) {
        kprintf("[AML] %s: stores into %s, which is a field of an operation "
                "region — this kernel writes an address space only while it "
                "is running a \\_PIC that a dry pass proved it can run whole, "
                "so the method is abandoned here rather than half-run\n",
                who, field->name);
        return false;
    }

    AmlFieldReach at;
    uint64_t      phys = 0;
    if (!AmlFieldWritable(field, who, &at, &phys)) return false;

    size_t len = (size_t)(at.last - at.first);
    volatile uint8_t *va =
        (volatile uint8_t *)vmm_map_mmio((uintptr_t)phys, len,
                                         VMM_FLAGS_KERNEL_RW);
    if (!va) {
        kprintf("[AML] %s: field %s sits at physical 0x%lx and the mapper "
                "would not hand out a writable window onto it — nothing is "
                "written\n", who, field->name, (unsigned long)phys);
        return false;
    }

    uint32_t done = 0;
    for (size_t off = 0; done < at.bits; off += at.access) {
        uint64_t unit = (value >> done);
        switch (at.access) {
        case 1: va[off] = (uint8_t)unit; break;
        case 2: *(volatile uint16_t *)(va + off) = (uint16_t)unit; break;
        case 4: *(volatile uint32_t *)(va + off) = (uint32_t)unit; break;
        default: *(volatile uint64_t *)(va + off) = unit; break;
        }
        done += (uint32_t)(at.access * 8u);
    }
    vmm_unmap_mmio((volatile void *)va, len);

    run->writes++;
    AmlRunHeard(run, AmlMix((uint64_t)(uintptr_t)field, value));
    kprintf("[AML] %s: 0x%lx placed in %s, a field of operation region %s — "
            "%lu byte(s) at physical 0x%lx, inside a range this board's own "
            "memory map declares ACPI NVS, which ACPI 6.5 Table 15.1 "
            "makes memory the firmware set aside and asks to have saved "
            "across sleep\n",
            who, (unsigned long)value, field->name,
            field->v.field.region->name,
            (unsigned long)len, (unsigned long)phys);
    return true;
}

/* =====================================================================
 * The machine
 * ===================================================================== */

static AmlLocalObj *AmlLocalFind(AmlAct *a, const char seg[4]) {
    for (uint32_t i = 0; i < a->objn; i++) {
        AmlLocalObj *o = &a->obj[i];
        if (o->name[0] == seg[0] && o->name[1] == seg[1] &&
            o->name[2] == seg[2] && o->name[3] == seg[3]) return o;
    }
    return NULL;
}

/* A NameString that is one bare NameSeg and nothing else — the only form a
 * method's own object can be named by, since §5.3's search rules are what
 * find it. */
static bool AmlBareSeg(const uint8_t *p, const uint8_t *end, char seg[4]) {
    if ((size_t)(end - p) < 4) return false;
    if (!aml_is_lead_name_char(p[0])) return false;
    seg[0] = (char)p[0]; seg[1] = (char)p[1];
    seg[2] = (char)p[2]; seg[3] = (char)p[3];
    return true;
}

/* Bits out of, and into, a buffer a method built for itself. Never the AML
 * table: the source of a CreateXField must be a method-local object, and a
 * method-local buffer is this run's own copy. */
static bool AmlBufFieldRead(AmlAct *a, AmlLocalObj *f, uint64_t *out) {
    AmlLocalObj *b = &a->obj[f->over];
    if (!b->buf || f->bit_width == 0 || f->bit_width > 64) return false;
    if ((uint64_t)f->bit_offset + f->bit_width > (uint64_t)b->buf_len * 8u)
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < f->bit_width; i++) {
        uint32_t bit = f->bit_offset + i;
        if (b->buf[bit >> 3] & (uint8_t)(1u << (bit & 7u)))
            v |= (1ULL << i);
    }
    *out = v;
    return true;
}

static bool AmlBufFieldWrite(AmlAct *a, AmlLocalObj *f, uint64_t v) {
    AmlLocalObj *b = &a->obj[f->over];
    if (!b->buf || f->bit_width == 0 || f->bit_width > 64) return false;
    if ((uint64_t)f->bit_offset + f->bit_width > (uint64_t)b->buf_len * 8u)
        return false;
    for (uint32_t i = 0; i < f->bit_width; i++) {
        uint32_t bit = f->bit_offset + i;
        uint8_t  m   = (uint8_t)(1u << (bit & 7u));
        if (v & (1ULL << i)) b->buf[bit >> 3] |= m;
        else                 b->buf[bit >> 3] = (uint8_t)(b->buf[bit >> 3] & ~m);
    }
    return true;
}

/*
 * How many operands an operator takes, and how many Targets follow them.
 * ACPI 6.5 §20.2.5.4. An opcode that is not here is not evaluated, and the
 * refusal names it — this list IS the grammar, and widening it is a decision
 * somebody makes on purpose.
 *
 * ‼ WIDENED ON PURPOSE, ONCE: Increment (0x75) and Decrement (0x76).
 * §20.2.5.4 gives them DefIncrement := IncrementOp SuperName and
 * DefDecrement := DecrementOp SuperName — add one, or take one away, store
 * the result back into that same SuperName, and yield the new value. They are
 * here because without them the CANONICAL counted loop,
 * `While (LLess (Local0, N)) { ... Increment (Local0) }`, cannot run a single
 * turn: it is refused by name on the statement that ends the turn, which
 * makes the DefWhile above it worth nothing. Nothing else was added with
 * them.
 *
 * Their single operand IS their Target, which no other shape here has, so
 * AmlExprStep carries them out where the stream position is still in hand
 * rather than through the pending-operator path — the counts below are what
 * the dry walk needs to MEASURE one.
 */
static bool AmlOperatorShape(uint8_t op, uint8_t *nargs, uint8_t *ntargets) {
    switch (op) {
    case StoreOp:        *nargs = 1; *ntargets = 1; return true;
    case IncrementOp: case DecrementOp:
                         *nargs = 0; *ntargets = 1; return true;
    case AddOp: case SubtractOp: case MultiplyOp: case ModOp:
    case AndOp: case NandOp: case OrOp: case NorOp: case XorOp:
    case ShiftLeftOp: case ShiftRightOp:
                         *nargs = 2; *ntargets = 1; return true;
    case DivideOp:       *nargs = 2; *ntargets = 2; return true;
    case NotOp: case FindSetLeftBitOp: case FindSetRightBitOp:
                         *nargs = 1; *ntargets = 1; return true;
    case LAndOp: case LOrOp: case LEqualOp:
    case LGreaterOp: case LLessOp:
                         *nargs = 2; *ntargets = 0; return true;
    case LNotOp:         *nargs = 1; *ntargets = 0; return true;
    default:             return false;
    }
}

/* The sub-opcodes under 0x5B this evaluator declines, by the name the ASL
 * writer used. On a live board this is the line an operator photographs, so
 * "0x5B 0x23" is worth a good deal less than "Acquire". */
static const char *AmlExtOpName(uint8_t sub) {
    switch (sub) {
    case ExtMutexOp:       return "Mutex";
    case ExtEventOp:       return "Event";
    case ExtCondRefOfOp:   return "CondRefOf";
    case ExtCreateFieldOp: return "CreateField";
    case ExtLoadTableOp:   return "LoadTable";
    case ExtLoadOp:        return "Load";
    case ExtStallOp:       return "Stall";
    case ExtSleepOp:       return "Sleep";
    case ExtAcquireOp:     return "Acquire";
    case ExtSignalOp:      return "Signal";
    case ExtWaitOp:        return "Wait";
    case ExtResetOp:       return "Reset";
    case ExtReleaseOp:     return "Release";
    case ExtFromBCDOp:     return "FromBCD";
    case ExtToBCDOp:       return "ToBCD";
    case ExtUnloadOp:      return "Unload";
    case ExtRevisionOp:    return "Revision";
    case ExtDebugOp:       return "Debug";
    case ExtFatalOp:       return "Fatal";
    case ExtTimerOp:       return "Timer";
    case ExtOpRegionOp:    return "OperationRegion";
    case ExtFieldOp:       return "Field";
    case ExtIndexFieldOp:  return "IndexField";
    case ExtBankFieldOp:   return "BankField";
    case ExtDeviceOp:      return "Device";
    case ExtDataRegionOp:  return "DataTableRegion";
    default:               return NULL;
    }
}

/* The name an ASL writer would recognise, for an opcode that stands where
 * this evaluator cannot carry it out. Increment and Decrement were here and
 * are gone with the refusal that named them: AmlOperatorShape now admits
 * both, so neither can reach a line that says they were declined. */
static const char *AmlOpcodeName(uint8_t op) {
    switch (op) {
    case WhileOp:        return "a While loop";
    case BreakOp:        return "Break";
    case ContinueOp:     return "Continue";
    case BreakPointOp:   return "BreakPoint";
    case ConcatOp:       return "Concatenate";
    case IndexOp:        return "Index";
    case DerefOfOp:      return "DerefOf";
    case RefOfOp:        return "RefOf";
    case SizeOfOp:       return "SizeOf";
    case ObjectTypeOp:   return "ObjectType";
    /* Single-byte opcodes, not the extended ones of the same value —
     * NotifyOp is 0x86 and 0x5B 0x86 is IndexField, which are two different
     * things and were one bug's worth of confusion apart. */
    case 0x86:           return "Notify";
    case 0x84:           return "ConcatenateResTemplate";
    case 0x89:           return "Match";
    case 0x96:           return "ToBuffer";
    case 0x98:           return "ToHexString";
    case 0x99:           return "ToInteger";
    case 0x9C:           return "ToString";
    case 0x9D:           return "CopyObject";
    case 0x9E:           return "Mid";
    default:             return NULL;
    }
}

/* ===========================================================
 * Operands, inside a running method
 * =========================================================== */

static bool AmlReadOperandIn(AmlRun *run, AmlAct *a, const uint8_t **p,
                              const uint8_t *end, AmlValue *out) {
    if (*p >= end) {
        kprintf("[AML] %s: an operand was expected and the body ended\n",
                a->what);
        return false;
    }
    uint8_t op = **p;

    if (op >= Local0Op && op <= Local7Op) {
        *out = a->local[op - Local0Op];
        (*p)++;
        return true;
    }
    if (op >= Arg0Op && op <= Arg6Op) {
        *out = a->arg[op - Arg0Op];
        (*p)++;
        return true;
    }

    /* A name. The method's own objects first — §20.2.5.2 puts them under the
     * running method, and §5.3's search rules find them before anything an
     * enclosing scope declares. */
    char seg[4];
    if (AmlBareSeg(*p, end, seg)) {
        AmlLocalObj *lo = AmlLocalFind(a, seg);
        if (lo) {
            (*p) += 4;
            if (lo->is_field) {
                uint64_t v = 0;
                if (!AmlBufFieldRead(a, lo, &v)) {
                    kprintf("[AML] %s: buffer field %c%c%c%c does not fit "
                            "inside the buffer it was cut from\n", a->what,
                            seg[0], seg[1], seg[2], seg[3]);
                    return false;
                }
                memset(out, 0, sizeof(*out));
                out->type    = AML_OBJ_INTEGER;
                out->integer = v & AmlIntMask();
                return true;
            }
            *out = lo->value;
            return true;
        }
    }

    if (aml_is_name_string_lead(op) && op != 0x00) {
        aml_object_t *o = resolve_name(p, end, a->method, false, NULL);
        if (!o) {
            kprintf("[AML] %s: a name in this term is not in the namespace — "
                    "no value is invented for it\n", a->what);
            return false;
        }
        if (o->type == AML_OBJ_FIELD) {
            uint64_t v = 0;
            if (!AmlRegionRead(run, o, a->what, &v)) return false;
            memset(out, 0, sizeof(*out));
            out->type    = AML_OBJ_INTEGER;
            out->integer = v;
            out->node    = o;
            return true;
        }
        if (o->type == AML_OBJ_METHOD) {
            /* Reached only when the caller did not take the call path, which
             * it always does; said rather than silently valued. */
            kprintf("[AML] %s: %s is a method and stands where a value was "
                    "expected\n", a->what, o->name);
            return false;
        }
        return AmlValueOfNode(o, a->what, out);
    }

    if (op == ExtOpPrefix) {
        uint8_t     sub = ((size_t)(end - *p) >= 2) ? (*p)[1] : 0;
        const char *nm  = AmlExtOpName(sub);
        kprintf("[AML] %s: %s stands where a value was expected, and this "
                "evaluator does not evaluate it\n",
                a->what, nm ? nm : "an extended opcode under 0x5B");
        return false;
    }

    /* Everything else is a literal, and AmlReadOperand reads literals. */
    return AmlReadOperand(p, end, a->method, a->what, out);
}

/* ===========================================================
 * Store targets
 * =========================================================== */

/*
 * ‼ THE PLACES A Store MAY LAND, AND THE ONE NARROW GATE IT MAY GO THROUGH.
 * A local, an argument, the Debug object (which discards and touches
 * nothing), an object the running method declared for itself, or a namespace
 * Name holding an integer — none of which leaves this kernel's own memory.
 *
 * A Field is where AML reaches hardware, and it now has exactly one way
 * through: AmlRegionWrite, whose first line refuses unless this run is the
 * real run of a \_PIC that the narrow gate proved. An OperationRegion named
 * whole is refused with no gate at all — a store into a region rather than
 * into a field of one names no width and no offset, so there is nothing to
 * write even in principle.
 */
static bool AmlStoreInto(AmlRun *run, AmlAct *a, const uint8_t **p,
                          const uint8_t *end, const AmlValue *v) {
    if (*p >= end) {
        kprintf("[AML] %s: a Store names no target\n", a->what);
        return false;
    }
    uint8_t t = **p;

    if (t == 0x00) { (*p)++; return true; }              /* NullName */
    if (t >= Local0Op && t <= Local7Op) {
        (*p)++;
        a->local[t - Local0Op] = *v;
        return true;
    }
    if (t >= Arg0Op && t <= Arg6Op) {
        (*p)++;
        a->arg[t - Arg0Op] = *v;
        return true;
    }
    if (t == ExtOpPrefix) {
        if ((size_t)(end - *p) >= 2 && (*p)[1] == ExtDebugOp) {
            *p += 2;
            return true;                                 /* Debug: discarded */
        }
        kprintf("[AML] %s: a Store names an extended target this evaluator "
                "does not write\n", a->what);
        return false;
    }

    char seg[4];
    if (AmlBareSeg(*p, end, seg)) {
        AmlLocalObj *lo = AmlLocalFind(a, seg);
        if (lo) {
            (*p) += 4;
            if (lo->is_field) {
                if (v->type != AML_OBJ_INTEGER) {
                    kprintf("[AML] %s: stores %s into buffer field %c%c%c%c, "
                            "and only an integer store is written here\n",
                            a->what, AmlTypeName(v->type),
                            seg[0], seg[1], seg[2], seg[3]);
                    return false;
                }
                if (!AmlBufFieldWrite(a, lo, v->integer)) {
                    kprintf("[AML] %s: buffer field %c%c%c%c does not fit "
                            "inside the buffer it was cut from\n", a->what,
                            seg[0], seg[1], seg[2], seg[3]);
                    return false;
                }
                return true;
            }
            lo->value = *v;
            return true;
        }
    }

    if (!aml_is_name_string_lead(t)) {
        kprintf("[AML] %s: opcode 0x%02x in target position is not a name\n",
                a->what, t);
        return false;
    }

    aml_object_t *o = resolve_name(p, end, a->method, false, NULL);
    if (!o) {
        kprintf("[AML] %s: a Store names something this namespace does not "
                "hold\n", a->what);
        return false;
    }
    if (o->type == AML_OBJ_REGION) {
        kprintf("[AML] %s: stores into %s, which is an operation region "
                "named whole — a region has no width and no offset of its "
                "own, so there is nothing here to place a value in. The "
                "method is abandoned rather than half-run\n",
                a->what, o->name);
        return false;
    }
    if (o->type == AML_OBJ_FIELD) {
        if (v->type != AML_OBJ_INTEGER) {
            kprintf("[AML] %s: stores %s into %s, and the only value this "
                    "kernel places in a field of an operation region is an "
                    "integer\n", a->what, AmlTypeName(v->type), o->name);
            return false;
        }
        return AmlRegionWrite(run, o, a->what, v->integer);
    }
    if (o->type != AML_OBJ_INTEGER && o->type != AML_OBJ_NAME &&
        o->type != AML_OBJ_UNINIT) {
        kprintf("[AML] %s: stores into %s, which is %s — overwriting it is a "
                "conversion this evaluator does not perform\n",
                a->what, o->name, AmlTypeName(o->type));
        return false;
    }
    if (v->type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s: stores %s into %s, and only an integer store is "
                "written here\n", a->what, AmlTypeName(v->type), o->name);
        return false;
    }
    o->type      = AML_OBJ_INTEGER;
    o->v.integer = v->integer;
    /* ‼ THIS LANDS OUTSIDE THE ACTIVATION, so a loop counting in a namespace
     * object rather than in a Local is still visibly getting somewhere. */
    AmlRunHeard(run, AmlMix((uint64_t)(uintptr_t)o, v->integer));
    return true;
}

/* ===========================================================
 * Folding one operator
 * =========================================================== */

static uint64_t AmlShiftLeft(uint64_t a, uint64_t n) {
    if (n >= g_int_bits) return 0;
    return (a << n) & AmlIntMask();
}

static uint64_t AmlShiftRight(uint64_t a, uint64_t n) {
    if (n >= g_int_bits) return 0;
    return (a & AmlIntMask()) >> n;
}

/*
 * `tv` receives the values the operator's Targets take, in the order the
 * Targets appear in the stream — §20.2.5.4 puts Divide's Remainder before its
 * Quotient, which is the one place the order is not "the result".
 */
static bool AmlFold(AmlAct *a, const AmlPending *n, AmlValue *out,
                     AmlValue tv[2], uint8_t *ntv) {
    *ntv = 0;
    memset(out, 0, sizeof(*out));

    if (n->op == StoreOp) {
        *out   = n->got[0];
        tv[0]  = n->got[0];
        *ntv   = 1;
        return true;
    }

    for (uint8_t i = 0; i < n->need; i++) {
        if (n->got[i].type != AML_OBJ_INTEGER) {
            kprintf("[AML] %s: opcode 0x%02x was handed %s, and this evaluator "
                    "computes with integers only\n",
                    a->what, n->op, AmlTypeName(n->got[i].type));
            return false;
        }
    }
    uint64_t x = n->got[0].integer & AmlIntMask();
    uint64_t y = (n->need == 2) ? (n->got[1].integer & AmlIntMask()) : 0;
    uint64_t r;
    bool     logical = false;

    switch (n->op) {
    case AddOp:       r = x + y;                          break;
    case SubtractOp:  r = x - y;                          break;
    case MultiplyOp:  r = x * y;                          break;
    case ShiftLeftOp: r = AmlShiftLeft(x, y);             break;
    case ShiftRightOp:r = AmlShiftRight(x, y);            break;
    case AndOp:       r = x & y;                          break;
    case NandOp:      r = ~(x & y);                       break;
    case OrOp:        r = x | y;                          break;
    case NorOp:       r = ~(x | y);                       break;
    case XorOp:       r = x ^ y;                          break;
    case NotOp:       r = ~x;                             break;
    case ModOp:
    case DivideOp:
        if (y == 0) {
            /* §20.2.5.4 makes a zero divisor a fatal error for the method.
             * Answering with anything at all here would be the fabricated
             * number this file exists not to produce. */
            kprintf("[AML] %s: a division by zero — the method is abandoned "
                    "rather than answered\n", a->what);
            return false;
        }
        r = (n->op == ModOp) ? (x % y) : (x / y);
        break;
    case FindSetLeftBitOp:
        r = 0;
        for (uint32_t i = 0; i < g_int_bits; i++)
            if (x & (1ULL << i)) r = i + 1u;
        break;
    case FindSetRightBitOp:
        r = 0;
        for (uint32_t i = 0; i < g_int_bits; i++)
            if (x & (1ULL << i)) { r = i + 1u; break; }
        break;
    case LNotOp:     r = (x == 0);            logical = true; break;
    case LAndOp:     r = (x != 0 && y != 0);  logical = true; break;
    case LOrOp:      r = (x != 0 || y != 0);  logical = true; break;
    case LEqualOp:   r = (x == y);            logical = true; break;
    case LGreaterOp: r = (x >  y);            logical = true; break;
    case LLessOp:    r = (x <  y);            logical = true; break;
    default:
        kprintf("[AML] %s: opcode 0x%02x reached the folder and has no rule "
                "there\n", a->what, n->op);
        return false;
    }

    out->type = AML_OBJ_INTEGER;
    if (logical) {
        /* §20.2.5.4: a logical operator yields Ones for true, Zero for
         * false — Ones being as wide as an Integer is on this machine. */
        out->integer = r ? AmlIntMask() : 0ULL;
        return true;
    }
    out->integer = r & AmlIntMask();

    if (n->op == DivideOp) {
        memset(&tv[0], 0, sizeof(tv[0]));
        memset(&tv[1], 0, sizeof(tv[1]));
        tv[0].type = AML_OBJ_INTEGER; tv[0].integer = (x % y) & AmlIntMask();
        tv[1] = *out;
        *ntv = 2;
    } else {
        tv[0] = *out;
        *ntv  = 1;
    }
    return true;
}

/* ===========================================================
 * Activations
 * =========================================================== */

static bool AmlActPush(AmlRun *run, aml_object_t *m, const AmlValue *args,
                        uint8_t argc) {
    uint8_t want = (uint8_t)(m->v.method.flags & 0x7u);
    if (want != argc) {
        kprintf("[AML] %s takes %u arguments and was offered %u — not run\n",
                m->name, want, argc);
        return false;
    }
    if (!m->v.method.aml || m->v.method.aml_len == 0) {
        kprintf("[AML] %s has an empty body\n", m->name);
        return false;
    }

    AmlAct *grown = (AmlAct *)AmlStackRoom(run->act, &run->act_room,
                                           run->actn + 1, sizeof(AmlAct));
    if (!grown) {
        kprintf("[AML] the heap has no room to call %s — this firmware nests "
                "its methods deeper than this machine can hold, and the "
                "question is abandoned rather than answered\n", m->name);
        return false;
    }
    run->act = grown;

    AmlAct *a = &run->act[run->actn];
    memset(a, 0, sizeof(*a));
    a->method = m;
    a->what   = m->name;
    for (uint8_t i = 0; i < argc; i++) a->arg[i] = args[i];

    a->blk = (AmlBlock *)AmlStackRoom(NULL, &a->blk_room, 1, sizeof(AmlBlock));
    if (!a->blk) {
        kprintf("[AML] the heap has no room to walk %s's body\n", m->name);
        return false;
    }
    memset(&a->blk[0], 0, sizeof(AmlBlock));
    a->blk[0].p   = m->v.method.aml;
    a->blk[0].end = m->v.method.aml + m->v.method.aml_len;
    a->blkn       = 1;

    run->actn++;
    return true;
}

/* Pop the top activation and hand its result to whoever asked for it. */
static bool AmlActFinish(AmlRun *run) {
    AmlAct  *a        = &run->act[run->actn - 1];
    AmlValue ret      = a->ret;
    bool     returned = a->returned;

    AmlActRelease(a);
    run->actn--;

    if (run->actn == 0) {
        run->result   = ret;
        run->returned = returned;
        return true;
    }

    AmlAct *c = &run->act[run->actn - 1];
    if (c->pendn == 0 || !c->pend[c->pendn - 1].call) {
        kprintf("[AML] %s: a method returned to a term that was not calling "
                "one\n", c->what);
        return false;
    }
    c->pendn--;
    /* A method that returns nothing yields an object of no type; a caller
     * that then tries to compute with it is refused BY THE FOLDER, naming
     * what it was handed. A caller that discards it is right to. */
    c->val      = returned ? ret : (AmlValue){ 0, 0, NULL, NULL, 0, NULL, NULL };
    c->have_val = true;
    return true;
}

/* ===========================================================
 * One step of the machine
 * =========================================================== */

static bool AmlExprStep(AmlRun *run) {
    AmlAct *a = &run->act[run->actn - 1];

    if (!a->have_val) {
        if (a->cur >= a->cur_end) {
            kprintf("[AML] %s: a term ends where an operand was expected\n",
                    a->what);
            return false;
        }
        uint8_t op = *a->cur;
        uint8_t nargs = 0, ntargets = 0;

        if (op == IncrementOp || op == DecrementOp) {
            /* ‼ THE ONE OPERATOR WHOSE OPERAND AND TARGET ARE THE SAME BYTES.
             * §20.2.5.4: DefIncrement := IncrementOp SuperName. The name is
             * READ, one is added or taken away, and the new value is placed
             * back where it came from — so the cursor is kept and handed to
             * the store, instead of the stream being read twice. Everything
             * the read and the store may touch is what they always touch: a
             * Local, an Arg, an object the method declared, a namespace Name,
             * or a field, which the window and the write gate judge as they
             * judge every other access. */
            const uint8_t *at = a->cur + 1;
            const uint8_t *q  = at;
            AmlValue       v;
            if (!AmlReadOperandIn(run, a, &q, a->cur_end, &v)) return false;
            if (v.type != AML_OBJ_INTEGER) {
                kprintf("[AML] %s: %s names %s, and §20.2.5.4 counts an "
                        "integer up or down by one — nothing else\n", a->what,
                        op == IncrementOp ? "Increment" : "Decrement",
                        AmlTypeName(v.type));
                return false;
            }
            AmlValue nv;
            memset(&nv, 0, sizeof(nv));
            nv.type    = AML_OBJ_INTEGER;
            nv.integer = ((op == IncrementOp) ? v.integer + 1u
                                              : v.integer - 1u) & AmlIntMask();
            const uint8_t *t = at;
            if (!AmlStoreInto(run, a, &t, a->cur_end, &nv)) return false;
            a->cur      = q;
            a->val      = nv;
            a->have_val = true;
            return true;
        }

        if (AmlOperatorShape(op, &nargs, &ntargets)) {
            AmlPending *grown =
                (AmlPending *)AmlStackRoom(a->pend, &a->pend_room,
                                           a->pendn + 1, sizeof(AmlPending));
            if (!grown) {
                kprintf("[AML] %s: the heap has no room to hold this "
                        "expression's operators — the term is refused, not "
                        "guessed\n", a->what);
                return false;
            }
            a->pend = grown;
            memset(&a->pend[a->pendn], 0, sizeof(AmlPending));
            a->pend[a->pendn].op   = op;
            a->pend[a->pendn].need = nargs;
            a->pendn++;
            a->cur++;
            return true;
        }

        /* A name that resolves to a Method is a CALL, and the arguments
         * follow it in the stream. This is the shape every board writes for
         * an interrupt link, and refusing it was what kept this evaluator
         * from reading one. */
        char seg[4];
        if (aml_is_name_string_lead(op) && op != 0x00 &&
            !(AmlBareSeg(a->cur, a->cur_end, seg) && AmlLocalFind(a, seg))) {
            const uint8_t *probe = a->cur;
            aml_object_t  *o     = resolve_name(&probe, a->cur_end, a->method,
                                                false, NULL);
            if (o && o->type == AML_OBJ_METHOD) {
                AmlPending *grown =
                    (AmlPending *)AmlStackRoom(a->pend, &a->pend_room,
                                               a->pendn + 1,
                                               sizeof(AmlPending));
                if (!grown) {
                    kprintf("[AML] %s: the heap has no room to hold this "
                            "expression's operators\n", a->what);
                    return false;
                }
                a->pend = grown;
                memset(&a->pend[a->pendn], 0, sizeof(AmlPending));
                a->pend[a->pendn].call   = true;
                a->pend[a->pendn].callee = o;
                a->pend[a->pendn].need   = (uint8_t)(o->v.method.flags & 0x7u);
                a->pendn++;
                a->cur = probe;
                if (a->pend[a->pendn - 1].need == 0)
                    return AmlActPush(run, o, NULL, 0);
                return true;
            }
        }

        if (!AmlReadOperandIn(run, a, &a->cur, a->cur_end, &a->val)) {
            const char *nm = AmlOpcodeName(op);
            if (nm)
                kprintf("[AML] %s: %s stands in this term, and this evaluator "
                        "does not evaluate it — the method is abandoned rather "
                        "than half-run\n", a->what, nm);
            return false;
        }
        a->have_val = true;
    }

    if (a->pendn == 0) {
        /* The expression is finished. What it was FOR decides what happens
         * next, and every one of the four leaves the block cursor where the
         * next statement begins. */
        a->in_expr  = false;
        a->have_val = false;
        AmlValue v  = a->val;

        switch (a->after) {
        case AML_AFTER_RETURN:
            a->ret      = v;
            a->returned = true;
            /* Dropped rather than zeroed: a Return out of the middle of a
             * While has to give back that loop's account of itself. */
            AmlBlocksDrop(a, 0);
            return AmlActFinish(run);

        case AML_AFTER_STORE: {
            AmlBlock *b = &a->blk[a->blkn - 1];
            if (!AmlStoreInto(run, a, &a->cur, a->cur_end, &v)) return false;
            b->p = a->cur;
            return true;
        }

        case AML_AFTER_DISCARD: {
            AmlBlock *b = &a->blk[a->blkn - 1];
            b->p = a->cur;
            return true;
        }

        case AML_AFTER_IF: {
            if (v.type != AML_OBJ_INTEGER) {
                kprintf("[AML] %s: an If was handed %s to decide on, and only "
                        "an integer decides anything here\n",
                        a->what, AmlTypeName(v.type));
                return false;
            }
            bool truth = (v.integer != 0);

            AmlBlock      *b          = &a->blk[a->blkn - 1];
            const uint8_t *if_end     = a->if_end;
            const uint8_t *take_start = truth ? a->cur : NULL;
            const uint8_t *take_end   = truth ? if_end : NULL;
            const uint8_t *after      = if_end;

            /* DefIfElse := IfOp PkgLength Predicate TermList DefElse — the
             * Else, when there is one, follows the package. §20.2.5.3. */
            if (if_end < b->end && *if_end == ElseOp) {
                const uint8_t *e   = if_end + 1;
                uint32_t       ecb = 0;
                uint32_t       epk = decode_pkglen(&e, b->end, &ecb);
                if (!ecb || epk < ecb) {
                    kprintf("[AML] %s: an Else has a malformed package "
                            "length\n", a->what);
                    return false;
                }
                const uint8_t *e_end = e + epk;
                if (e_end > b->end) {
                    kprintf("[AML] %s: an Else runs past the end of its "
                            "block\n", a->what);
                    return false;
                }
                if (!truth) { take_start = e + ecb; take_end = e_end; }
                after = e_end;
            }

            b->p = after;                    /* before the stack may move */
            if (take_start) {
                AmlBlock *grown =
                    (AmlBlock *)AmlStackRoom(a->blk, &a->blk_room,
                                             a->blkn + 1, sizeof(AmlBlock));
                if (!grown) {
                    kprintf("[AML] %s: the heap has no room for this method's "
                            "nesting — the method is abandoned, not "
                            "guessed\n", a->what);
                    return false;
                }
                a->blk = grown;
                memset(&a->blk[a->blkn], 0, sizeof(AmlBlock));
                a->blk[a->blkn].p   = take_start;
                a->blk[a->blkn].end = take_end;
                a->blkn++;
            }
            return true;
        }

        case AML_AFTER_WHILE: {
            /* ACPI 6.5 §20.2.5.3 writes it `Predicate := TermArg => Integer`,
             * the same shape it gives DefIfElse's. */
            if (v.type != AML_OBJ_INTEGER) {
                kprintf("[AML] %s: a While was handed %s to decide on, and "
                        "only an integer decides anything here\n",
                        a->what, AmlTypeName(v.type));
                return false;
            }
            AmlBlock *L = &a->blk[a->blkn - 1];
            if (v.integer == 0) {
                AmlBlocksDrop(a, a->blkn - 1);
                return true;
            }
            /* The body is whatever stands between the predicate this just
             * read and the end of the loop's package. */
            L->p = a->cur;
            return true;
        }
        }
        return false;
    }

    AmlPending *top = &a->pend[a->pendn - 1];
    top->got[top->have++] = a->val;
    a->have_val = false;
    if (top->have != top->need) return true;

    if (top->call) {
        /* Everything the push needs, read BEFORE it: growing the activation
         * array moves `a` and `top` both. */
        aml_object_t *callee = top->callee;
        uint8_t       argc   = top->need;
        AmlValue      args[AML_ARG_COUNT];
        for (uint8_t i = 0; i < argc; i++) args[i] = top->got[i];
        return AmlActPush(run, callee, args, argc);
    }

    AmlValue folded;
    AmlValue tv[2];
    uint8_t  ntv = 0, nargs = 0, ntargets = 0;
    AmlOperatorShape(top->op, &nargs, &ntargets);
    if (!AmlFold(a, top, &folded, tv, &ntv)) return false;
    a->pendn--;
    for (uint8_t i = 0; i < ntargets; i++) {
        const AmlValue *give = (i < ntv) ? &tv[i] : &folded;
        if (!AmlStoreInto(run, a, &a->cur, a->cur_end, give)) return false;
    }
    a->val      = folded;
    a->have_val = true;
    return true;
}

/* Begin an expression at `from`, ending at `to`, for the reason `why`. */
static void AmlExprBegin(AmlAct *a, const uint8_t *from, const uint8_t *to,
                          AmlAfter why) {
    a->in_expr  = true;
    a->have_val = false;
    a->cur      = from;
    a->cur_end  = to;
    a->after    = why;
}

/*
 * A method's own object — `Name (PRR0, ResourceTemplate () {...})`. Its bytes
 * are COPIED: the originals are inside the DSDT this kernel mapped, they are
 * shared by every later reader of that table, and the very next statement of
 * every link's _CRS writes an interrupt number into them.
 */
static bool AmlDeclareLocal(AmlRun *run, AmlAct *a, const char seg[4],
                             const AmlValue *v, AmlLocalObj **out) {
    /* ‼ ONE OBJECT PER NAME, AND THE SECOND DECLARATION IS THE SAME OBJECT.
     *
     * ACPI 6.5 §20.2.5.2 gives a method's namespace one object per name, and
     * a Name or a CreateXField inside a While body is declared again on every
     * turn. What stood here APPENDED every time while AmlLocalFind answers
     * with the first match, so turn two's declaration could never be found,
     * `objn` grew by one per turn, and AmlTurnMark — which mixes objn in —
     * could never produce a number it had produced before. The loop's own
     * refusal was then unreachable: a firmware poll ended when kmalloc
     * refused the growing list, which is termination by heap exhaustion
     * wearing the name of a termination argument. Replaced in place, the
     * fingerprint can repeat and the loop is refused where the doctrine says
     * it is. */
    AmlLocalObj *o = AmlLocalFind(a, seg);
    bool         fresh = (o == NULL);
    if (fresh) {
        AmlLocalObj *grown = (AmlLocalObj *)AmlStackRoom(a->obj, &a->obj_room,
                                                         a->objn + 1,
                                                         sizeof(AmlLocalObj));
        if (!grown) {
            kprintf("[AML] %s: the heap has no room for the objects this "
                    "method declares\n", a->what);
            return false;
        }
        a->obj = grown;
        o = &a->obj[a->objn];
    }
    memset(o, 0, sizeof(*o));
    o->name[0] = seg[0]; o->name[1] = seg[1];
    o->name[2] = seg[2]; o->name[3] = seg[3];
    o->value   = *v;

    if (v->type == AML_OBJ_BUFFER && v->bytes && v->bytes_end > v->bytes) {
        uint32_t len = (uint32_t)(v->bytes_end - v->bytes);
        uint8_t *copy = (uint8_t *)AmlRunAlloc(run, len);
        if (!copy) {
            kprintf("[AML] %s: the heap has no room for the %lu-byte buffer "
                    "this method declares\n", a->what, (unsigned long)len);
            return false;
        }
        memcpy(copy, v->bytes, len);
        o->buf       = copy;
        o->buf_len   = len;
        o->value.bytes     = copy;
        o->value.bytes_end = copy + len;
    }
    if (fresh) a->objn++;
    if (out) *out = o;
    return true;
}

/*
 * ‼ ONE TURN OF A While HAS RUN OUT OF BODY, AND THIS IS WHERE THE LOOP HAS
 *   TO SHOW IT IS GETTING SOMEWHERE.
 *
 * The argument is in the doctrine above (HOW LONG IT CAN RUN). What happens
 * here is the whole of it: the turn is reduced to one number that covers
 * every Local, every Arg, every object the method declared, every byte of
 * every buffer among them, and everything the turn read from or placed
 * outside the activation. If this loop has stood in that exact state before,
 * then every step from here is a step it has already taken and the loop
 * cannot end — so it is refused, aloud, and the method is abandoned rather
 * than left half-run. Otherwise the state is kept and the predicate is asked
 * again.
 */
static bool AmlLoopTurn(AmlAct *a) {
    AmlBlock *L = &a->blk[a->blkn - 1];

    uint64_t fp = AmlTurnMark(a, L);
    L->world  = 0;
    L->worldn = 0;

    bool again = false;
    if (!AmlSeenAdd(&L->seen, fp, &again)) {
        /* The turn being refused is the one whose state would not fit, and
         * the table holds the ones before it — so the count said aloud is
         * one more than the table's, and saying the table's was this file
         * printing a number about itself that was not true. */
        kprintf("[AML] %[E]%s: a While loop has turned %u times without ever "
                "standing where it stood before, and the heap has no room to "
                "go on keeping account of that. A loop this machine cannot "
                "account for is not one it will go on turning — the method is "
                "abandoned rather than half-run%[D]\n",
                a->what, L->seen.used + 1u);
        return false;
    }
    if (again) {
        kprintf("[AML] %[E]%s: a turn of a While loop left every Local, every "
                "Arg, every object this method declared and every byte the "
                "machine answered exactly as an earlier turn left them. The "
                "next turn is a turn that already ran, so this loop has no "
                "end — the method is abandoned rather than half-run%[D]\n",
                a->what);
        return false;
    }

    AmlExprBegin(a, L->pred, L->end, AML_AFTER_WHILE);
    return true;
}

static bool AmlStmtStep(AmlRun *run) {
    AmlAct *a = &run->act[run->actn - 1];

    if (a->blkn == 0) return AmlActFinish(run);
    AmlBlock *b = &a->blk[a->blkn - 1];
    if (b->p >= b->end) {
        if (b->pred) return AmlLoopTurn(a);
        a->blkn--;
        return true;
    }

    uint8_t op = *b->p;

    /* ACPI 6.5 §20.2.5.3 gives DefNoop and DefBreakPoint the same behaviour
     * on a machine with no debugger attached: BreakPoint "is used for
     * debugging purposes ... if a debugger is not present, this operation
     * performs no action". There is no debugger here, so abandoning the
     * method on one was this evaluator inventing a refusal the specification
     * does not have. */
    if (op == NoopOp || op == BreakPointOp) { b->p++; return true; }

    if (op == ReturnOp) {
        AmlExprBegin(a, b->p + 1, b->end, AML_AFTER_RETURN);
        return true;
    }

    if (op == StoreOp) {
        AmlExprBegin(a, b->p + 1, b->end, AML_AFTER_STORE);
        return true;
    }

    if (op == IfOp) {
        const uint8_t *q  = b->p + 1;
        uint32_t       cb = 0;
        uint32_t       pk = decode_pkglen(&q, b->end, &cb);
        if (!cb || pk < cb) {
            kprintf("[AML] %s: an If has a malformed package length\n",
                    a->what);
            return false;
        }
        const uint8_t *if_end = q + pk;
        if (if_end > b->end) {
            kprintf("[AML] %s: an If runs past the end of its block\n",
                    a->what);
            return false;
        }
        a->if_end = if_end;
        AmlExprBegin(a, q + cb, if_end, AML_AFTER_IF);
        return true;
    }

    if (op == WhileOp) {
        /* DefWhile := WhileOp PkgLength Predicate TermList — ACPI 6.5
         * §20.2.5.3. The predicate lives inside the same package, ahead of
         * the body, so one block frame holds both: `pred` is where the
         * predicate begins and `end` is where the package ends. */
        const uint8_t *q  = b->p + 1;
        uint32_t       cb = 0;
        uint32_t       pk = decode_pkglen(&q, b->end, &cb);
        if (!cb || pk < cb) {
            kprintf("[AML] %s: a While has a malformed package length\n",
                    a->what);
            return false;
        }
        const uint8_t *loop_end = q + pk;
        if (loop_end > b->end) {
            kprintf("[AML] %s: a While runs past the end of its block\n",
                    a->what);
            return false;
        }
        const uint8_t *pred = q + cb;

        b->p = loop_end;                 /* before the stack below may move it */
        AmlBlock *grown =
            (AmlBlock *)AmlStackRoom(a->blk, &a->blk_room, a->blkn + 1,
                                     sizeof(AmlBlock));
        if (!grown) {
            kprintf("[AML] %s: the heap has no room for this method's "
                    "nesting — the method is abandoned, not guessed\n",
                    a->what);
            return false;
        }
        a->blk = grown;
        memset(&a->blk[a->blkn], 0, sizeof(AmlBlock));
        a->blk[a->blkn].p    = pred;
        a->blk[a->blkn].end  = loop_end;
        a->blk[a->blkn].pred = pred;
        a->blkn++;

        AmlExprBegin(a, pred, loop_end, AML_AFTER_WHILE);
        return true;
    }

    if (op == BreakOp || op == ContinueOp) {
        /* §20.2.5.3: DefBreak := BreakOp, DefContinue := ContinueOp. Both
         * name the INNERMOST enclosing loop. The blocks between here and it
         * are the bodies of Ifs entered inside that loop, and each of them
         * wrote its own "after" into the block beneath it when it was
         * entered — so leaving them owes nothing to anybody. */
        uint32_t at = a->blkn;
        while (at > 0 && !a->blk[at - 1].pred) at--;
        if (at == 0) {
            kprintf("[AML] %s: %s stands where no While loop encloses it — "
                    "the method is abandoned rather than half-run\n",
                    a->what, op == BreakOp ? "a Break" : "a Continue");
            return false;
        }
        if (op == BreakOp) {
            AmlBlocksDrop(a, at - 1);    /* the loop's own frame goes too */
            return true;
        }
        AmlBlocksDrop(a, at);            /* back to the loop's body ... */
        a->blk[at - 1].p = a->blk[at - 1].end;   /* ... this turn being over */
        return true;
    }

    if (op == NameOp) {
        /* §20.2.5.2: DefName := NameOp NameString DataRefObject. Inside a
         * method body it declares an object of the method's own. */
        const uint8_t *q = b->p + 1;
        char seg[4];
        if (!AmlBareSeg(q, b->end, seg)) {
            kprintf("[AML] %s: a Name inside a method body declares a path "
                    "rather than a single name — this evaluator gives a "
                    "method objects of its own and adds none to the "
                    "namespace\n", a->what);
            return false;
        }
        q += 4;
        AmlValue v;
        if (!AmlReadOperand(&q, b->end, a->method, a->what, &v)) return false;
        if (!AmlDeclareLocal(run, a, seg, &v, NULL)) return false;
        b->p = q;
        return true;
    }

    if (op == CreateBitFieldOp  || op == CreateByteFieldOp ||
        op == CreateWordFieldOp || op == CreateDWordFieldOp ||
        op == CreateQWordFieldOp) {
        /* §20.2.5.2: DefCreateXField := op SourceBuff Index NameString.
         * The index is a BIT index for CreateBitField and a BYTE index for
         * the other four. */
        uint32_t width = (op == CreateBitFieldOp)   ? 1u  :
                         (op == CreateByteFieldOp)  ? 8u  :
                         (op == CreateWordFieldOp)  ? 16u :
                         (op == CreateDWordFieldOp) ? 32u : 64u;
        uint32_t scale = (op == CreateBitFieldOp) ? 1u : 8u;

        const uint8_t *q = b->p + 1;
        char src[4];
        if (!AmlBareSeg(q, b->end, src)) {
            kprintf("[AML] %s: a CreateField cuts into something that is not "
                    "an object this method declared, and that is the only "
                    "buffer this evaluator can hold\n", a->what);
            return false;
        }
        AmlLocalObj *base = AmlLocalFind(a, src);
        if (!base || !base->buf) {
            kprintf("[AML] %s: a CreateField names %c%c%c%c as its buffer, and "
                    "this method declared no buffer by that name\n",
                    a->what, src[0], src[1], src[2], src[3]);
            return false;
        }
        uint32_t base_index = (uint32_t)(base - a->obj);
        q += 4;

        uint64_t index = 0;
        if (!read_const_integer(&q, b->end, &index)) {
            kprintf("[AML] %s: a CreateField's index is not a constant this "
                    "evaluator can read\n", a->what);
            return false;
        }
        char name[4];
        if (!AmlBareSeg(q, b->end, name)) {
            kprintf("[AML] %s: a CreateField names its field with a path "
                    "rather than a single name\n", a->what);
            return false;
        }
        q += 4;

        AmlValue empty;
        memset(&empty, 0, sizeof(empty));
        /* The object comes back from the declaration rather than being taken
         * as the last entry: a CreateXField inside a loop body declares the
         * same name every turn, and AmlDeclareLocal replaces that entry where
         * it already stands instead of appending a second one. */
        AmlLocalObj *f = NULL;
        if (!AmlDeclareLocal(run, a, name, &empty, &f)) return false;
        f->is_field   = true;
        f->over       = base_index;
        f->bit_offset = (uint32_t)(index * scale);
        f->bit_width  = width;
        b->p = q;
        return true;
    }

    if (op == ElseOp) {
        kprintf("[AML] %s: an Else stands where no If precedes it\n", a->what);
        return false;
    }

    if (op == ExtOpPrefix) {
        /* Mutexes, Events, Notify, Sleep, Stall, Fatal — every one of them is
         * the firmware asking this kernel to DO something rather than to read
         * something, and none of them appears in an interrupt-routing object.
         * Named rather than stepped over, because stepping over a statement
         * with a side effect is pretending it ran. */
        uint8_t     sub = ((size_t)(b->end - b->p) >= 2) ? b->p[1] : 0;
        const char *nm  = AmlExtOpName(sub);
        if (nm)
            kprintf("[AML] %s: %s acts on the platform rather than reading "
                    "it, and this evaluator only reads — the method is "
                    "abandoned rather than half-run\n", a->what, nm);
        else
            kprintf("[AML] %s: extended opcode 0x5B 0x%02x is not a statement "
                    "this evaluator runs — the method is abandoned rather "
                    "than half-run\n", a->what, sub);
        return false;
    }

    {
        uint8_t nargs = 0, ntargets = 0;
        if (AmlOperatorShape(op, &nargs, &ntargets) ||
            (aml_is_name_string_lead(op) && op != 0x00)) {
            /* An expression standing alone: a method called for its effect,
             * or an operator whose Target is the point of it. */
            AmlExprBegin(a, b->p, b->end, AML_AFTER_DISCARD);
            return true;
        }
    }

    {
        const char *nm = AmlOpcodeName(op);
        if (nm)
            kprintf("[AML] %s: %s stands at byte %lu of its body, and this "
                    "evaluator does not run it — the method is abandoned "
                    "rather than half-run\n", a->what, nm,
                    (unsigned long)(b->p - a->method->v.method.aml));
        else
            kprintf("[AML] %s: opcode 0x%02x at byte %lu of its body is not "
                    "one this evaluator runs — the method is abandoned rather "
                    "than half-run\n", a->what, op,
                    (unsigned long)(b->p - a->method->v.method.aml));
    }
    return false;
}

static bool AmlRunTo(AmlRun *run) {
    while (run->actn > 0)
        if (run->act[run->actn - 1].in_expr) {
            if (!AmlExprStep(run)) return false;
        } else {
            if (!AmlStmtStep(run)) return false;
        }
    return true;
}

/* ===========================================================
 * The one door into the machine
 * =========================================================== */

/*
 * The seven objects that may reach an address space, and the only place in
 * this file where the window is opened.
 *
 *   _PRT  ACPI 6.5 §6.2.13   where a PCI interrupt pin goes
 *   _CRS  §6.2.2             what a device currently holds
 *   _PRS  §6.2.12            what it could hold
 *   _STA  §6.3.7             whether it is there and enabled
 *   _PIC  §5.8.1             which interrupt model OSPM runs
 *   _HID  §6.1.5             what the device IS
 *   _CID  §6.1.2             what it is compatible with
 *
 * ‼ WHY _HID AND _CID EARN A WINDOW WHERE AN ARBITRARY METHOD DOES NOT.
 *
 * ACPI 6.5 §6.1.5 makes _HID "an object that ... can be a control method",
 * and §6.1.2 says the same of _CID. Firmware that decides an identifier from
 * what the OS announced — the shape Intel's reference ASL uses around OSYS —
 * writes Method (_HID), and that method reads a chipset register through an
 * OperationRegion like any other. Left outside the window, every such method
 * was refused on its first field read.
 *
 * What both objects may hand back is fixed by the specification and is not a
 * device operation: §6.1.5 allows "a 32-bit compressed EISA type ID" or a
 * string, §6.1.2 allows either of those or a package of them. An identifier
 * is an observation about the machine in exactly the sense _STA's presence
 * bits are, which is the standing that put _STA in this list. And the window
 * is a READING window only — run->may_write below is still granted to \_PIC
 * and to nothing else, so a _HID that tried to store would be refused aloud
 * by AmlStoreInto exactly as before.
 *
 * ‼ AND WHAT LEAVING THEM OUT COST is not a warning in a log. AmlRoutingAdopt
 * asks every device in the namespace what it is; a PCI root bridge whose _HID
 * is a method is then not recognised as a bridge at all, so its _PRT is never
 * read and every PCI line under it falls back to configuration byte 0x3C —
 * a byte PCI 3.0 §6.2.4 footnote 43 defines against the 8259 on a machine
 * running its IO-APIC. Six devices on one shipping desktop board were refused
 * here at once.
 */
static bool AmlNameIs(const char *name, const char *four) {
    return name[0] == four[0] && name[1] == four[1] &&
           name[2] == four[2] && name[3] == four[3];
}

static bool AmlNameOpensWindow(const char *name) {
    static const char *const routing[] =
        { "_PRT", "_CRS", "_PRS", "_STA", "_PIC", "_HID", "_CID" };
    for (uint32_t i = 0; i < sizeof(routing) / sizeof(routing[0]); i++)
        if (AmlNameIs(name, routing[i])) return true;
    return false;
}

/*
 * The value of a named object, running it first if it is a method. This is
 * how _PRT, _CRS, _STA, _HID, _BBN and _SEG are all asked: every one of them
 * is a Name on some boards and a Method on others.
 *
 * ‼ The value handed back may point into memory this run owns — a resource
 * template a method BUILT is the ordinary case — so it is valid until
 * AmlRunDone, and every caller consumes it before then.
 */
static bool AmlEvalIn(AmlRun *run, aml_object_t *node, const AmlValue *args,
                       uint8_t argc, AmlValue *out) {
    /* ‼ NOTHING OF A FAILED EVALUATION IS STILL STANDING WHEN THE NEXT ONE
     * BEGINS.
     *
     * AmlRunTo stops at the first refusal and AmlEvalIn returns false with
     * the activations that were running LEFT ON THE RUN — a method abandoned
     * in the middle, its blocks and its half-collected operands intact. A run
     * is shared: AmlReadDeviceIds evaluates _HID and then _CID on one,
     * continuing past a failure of the first. What stood here cleared only
     * `returned` and `result` and then pushed on top, so AmlRunTo drove the
     * abandoned frames again and a value resumed out of _HID could come back
     * under _CID's name — a device matched, or missed, as something it is
     * not. Every evaluation on a shared run starts from an empty stack, and
     * the heap those frames held goes back here rather than at AmlRunDone. */
    while (run->actn) AmlActRelease(&run->act[--run->actn]);

    /* ‼ THE ONLY ASSIGNMENT TO run->window IN THIS FILE. It is taken from the
     * name of the object being evaluated, so no caller can open a window for
     * something that is not a routing object, and the next evaluation on this
     * run replaces it with its own answer. */
    run->window = AmlNameOpensWindow(node->name);
    run->window_for = node->name;

    /* ‼ THE ONLY ASSIGNMENT TO run->may_write IN THIS FILE, and it needs both
     * halves. `proven` says the narrow gate walked this method whole; the
     * name says the object being evaluated is the one it walked. Neither on
     * its own opens anything, and a nested evaluation on a fresh run — a
     * region's base term, an interrupt link's _CRS — has `proven` false
     * because AmlRunInit zeroed it and nothing but AmlRoutingAdopt sets it. */
    run->may_write = run->proven && AmlNameIs(node->name, "_PIC");

    if (node->type != AML_OBJ_METHOD) {
        if (argc) {
            kprintf("[AML] %s was handed %u argument(s) and is not a method\n",
                    node->name, argc);
            return false;
        }
        return AmlValueOfNode(node, node->name, out);
    }

    run->returned = false;
    memset(&run->result, 0, sizeof(run->result));
    if (!AmlActPush(run, node, args, argc)) return false;
    if (!AmlRunTo(run)) return false;
    if (!out) return true;                  /* \_PIC returns nothing */
    if (!run->returned) {
        kprintf("[AML] %s ran to its end without returning a value, and no "
                "value is invented for it\n", node->name);
        return false;
    }
    *out = run->result;
    return true;
}

/* =====================================================================
 * WHERE A REGION IS — the second door into the machine, and the last one.
 * ===================================================================== */

/*
 * One TermArg, evaluated to an integer with no method around it.
 *
 * The activation this pushes has NO BODY: `blkn` is zero, so the machine
 * never takes a statement step in it, and the expression it begins ends by
 * RETURNING — which pops the activation and hands the value to the run. A
 * method the term CALLS gets an ordinary activation on top of it, so a base
 * written `ShiftLeft (RCBA, 14)` or `GNVS (Zero)` runs exactly as it would
 * inside a method body, driven by the same loop and with no C recursion.
 */
static bool AmlEvalTermArg(aml_object_t *scope, const char *what,
                            const uint8_t *from, const uint8_t *to,
                            uint64_t *out) {
    /* ‼ THE RUN IS MADE HERE AND NOWHERE ELSE, WHICH IS WHAT KEEPS THE
     * WINDOW SHUT. No caller hands one in, so no caller can hand in a run
     * that has a window open: AmlRunInit zeroes the field and only AmlEvalIn
     * ever assigns it. A Field standing in a region's base term is therefore
     * refused by AmlRegionRead's first line — before it can ask for another
     * region's base — and the nesting stops at one level by construction
     * rather than by a depth counter. */
    AmlRun run;
    AmlRunInit(&run);

    bool    ok = false;
    AmlAct *grown = (AmlAct *)AmlStackRoom(run.act, &run.act_room,
                                           run.actn + 1, sizeof(AmlAct));
    if (!grown) {
        kprintf("[AML] the heap has no room to evaluate %s\n", what);
    } else {
        run.act = grown;
        AmlAct *a = &run.act[run.actn];
        memset(a, 0, sizeof(*a));
        a->method = scope ? scope : &g_root_obj;  /* where its names live */
        a->what   = what;
        a->blkn   = 0;                            /* one term, and no body */
        AmlExprBegin(a, from, to, AML_AFTER_RETURN);
        run.actn++;

        if (AmlRunTo(&run)) {
            if (run.returned && run.result.type == AML_OBJ_INTEGER) {
                *out = run.result.integer;
                ok   = true;
            } else {
                kprintf("[AML] %s: the firmware's own term for it does not "
                        "come out as an integer\n", what);
            }
        }
    }

    AmlRunDone(&run);
    return ok;
}

/*
 * ‼ AN OPERATION REGION IS ARMED BEFORE IT IS READ, AND A REGION THAT
 *   CANNOT BE ARMED IS NEVER READ.
 *
 * ACPI 6.5 §20.2.5.2 makes RegionOffset and RegionLen TermArgs. Shipping
 * firmware uses that: `OperationRegion (RCRB, SystemMemory, \\RCBA, 0x4000)`
 * on Intel chipsets, and an offset named rather than written on AMD's. The
 * parse pass kept the bytes; this turns them into the board's own numbers,
 * ONCE, the first time anything wants to read through the region — which is
 * where ACPICA resolves them too, and for the same reason: at parse time
 * there is nothing to evaluate them with.
 *
 * What it will not do is supply a number nobody stated. A base that will not
 * resolve is a refusal with the region named, never a zero — zero is an
 * address: physical page 0 holds the real-mode interrupt vector table, I/O
 * port 0 is the 8237's channel-0 address register whose byte-pointer
 * flip-flop ADVANCES when it is read, and configuration byte 0 is a Vendor
 * ID. Any of the three would come back as an interrupt number carrying the
 * routing table's signature.
 */
static bool AmlRegionArm(aml_object_t *rg, const char *who) {
    if (rg->v.region.base_known && rg->v.region.len_known) return true;

    /* §20.2.2 makes a NameSeg four characters, so these sentences have a
     * length that is known before they are written. */
    char what[32];

    if (!rg->v.region.base_known) {
        ksnprintf(what, sizeof(what), "the base of %s", rg->name);
        if (!rg->v.region.base_term) {
            kprintf("[AML] %s: operation region %s was declared with a base "
                    "this pass could not even measure, so where it starts is "
                    "not known and nothing is read through it — refused "
                    "rather than read at address zero\n", who, rg->name);
            return false;
        }
        uint64_t v  = 0;
        bool     ok = AmlEvalTermArg(rg->v.region.declared_in, what,
                                     rg->v.region.base_term,
                                     rg->v.region.base_term_end, &v);
        if (!ok) {
            kprintf("[AML] %[E]%s: operation region %s does not say where it "
                    "starts — its base is a term the firmware computes, and "
                    "this kernel evaluates that term WITHOUT touching any "
                    "address space, so that finding out where to read can "
                    "never itself become a read. Nothing is read through %s "
                    "and no address is invented for it%[D]\n",
                    who, rg->name, rg->name);
            return false;
        }
        rg->v.region.base       = v;
        rg->v.region.base_known = true;
        kprintf("[AML] operation region %s: the firmware computes its base "
                "rather than writing one, and it comes out 0x%lx\n",
                rg->name, (unsigned long)v);
    }

    if (!rg->v.region.len_known) {
        ksnprintf(what, sizeof(what), "the length of %s", rg->name);
        if (!rg->v.region.len_term) {
            kprintf("[AML] %s: operation region %s was declared with a length "
                    "this pass could not measure, so nothing bounds a read "
                    "inside it — refused\n", who, rg->name);
            return false;
        }
        uint64_t v  = 0;
        bool     ok = AmlEvalTermArg(rg->v.region.declared_in, what,
                                     rg->v.region.len_term,
                                     rg->v.region.len_term_end, &v);
        if (!ok) {
            kprintf("[AML] %[E]%s: operation region %s does not say how long "
                    "it is — its length is a term this kernel cannot evaluate "
                    "without touching an address space, and a field read "
                    "against no bound is a read at an address nobody "
                    "stated%[D]\n", who, rg->name);
            return false;
        }
        rg->v.region.len       = v;
        rg->v.region.len_known = true;
        kprintf("[AML] operation region %s: the firmware computes its length "
                "rather than writing one, and it comes out %lu byte(s)\n",
                rg->name, (unsigned long)v);
    }
    return true;
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

/*
 * ‼ WHAT \_PIC'S ABSENCE MEANS, AND WHY IT IS NOT THE SAME AS ITS FAILURE.
 *
 * ACPI 6.5 §5.8.1 entire, because every clause of it decides something
 * below: "The \_PIC optional method is used to report to the platform
 * runtime firmware the current interrupt model used by the OS. This control
 * method returns nothing. The argument passed into the method signifies the
 * interrupt model OSPM has chosen, PIC mode, APIC mode, or SAPIC mode.
 * Notice that calling this method is optional for OSPM. If the platform CPU
 * architecture supports PIC mode and the method is never called, the
 * platform runtime firmware must assume PIC mode. It is important that the
 * platform runtime firmware save the value passed in by OSPM for later use
 * during wake operations."
 *
 * Read it in the direction it is written. The assumption belongs to the
 * FIRMWARE, not to OSPM: an untold platform is not one this kernel chooses
 * to read as PIC-mode, it is one the firmware is required to keep holding
 * the PIC-mode description out on. That is what makes reading its directly
 * numbered entries as 8259 lines a fact about the board rather than an
 * optimistic guess. The condition attached to it — "if the platform CPU
 * architecture supports PIC mode" — holds on every x86-64 machine.
 *
 * Three different facts hide behind one old boolean, and treating them alike
 * threw away every board of the first kind:
 *
 *   NOT_DECLARED — the firmware has ONE routing table and no way to switch.
 *     §5.8.1 has the FIRMWARE assume PIC mode, and on such a platform that
 *     is not a warning: the PIRQ router's output IS an interrupt-controller input, and
 *     the first sixteen IO-APIC inputs ARE the 8259's sixteen lines. That is
 *     precisely what the MADT's Interrupt Source Overrides exist to describe
 *     the exceptions to. The number is the board's own and it is the only one
 *     it has; refusing it leaves the line with config byte 0x3C, which is a
 *     WEAKER statement of the very same thing.
 *
 *   TOLD — \_PIC ran with this kernel's interrupt model. The table describes
 *     the mode actually running, which is the strongest case there is.
 *
 *   UNKNOWN — \_PIC is declared and did not run to the end. The firmware is
 *     holding a table this kernel cannot identify: it may be the PIC one,
 *     whose directly numbered entries are 8259 interrupts and mean nothing
 *     as global system interrupts. THAT is what is lost, and only that:
 *     AmlPrtLookup refuses every direct entry. An entry that names an
 *     interrupt LINK is still followed, because the link's _CRS reads the
 *     chipset's routing register rather than a table variant — see the note
 *     in AmlLinkRoute. Refusing those too would trade a register read for
 *     configuration byte 0x3C, which is a weaker statement of the very same
 *     8259 interrupt.
 */
typedef enum {
    AML_PIC_NOT_DECLARED = 0,
    AML_PIC_TOLD,
    AML_PIC_UNKNOWN,
} AmlPicState;

static AmlRootBridge *g_root_bridges  = NULL;
static AmlPrtTable   *g_prt_tables    = NULL;
static bool           g_routing_tried = false;
static AmlPicState    g_pic           = AML_PIC_NOT_DECLARED;

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
 * The seven characters an EISA id integer spells. ACPI 6.5 §19.6.37 is the
 * EISAID macro — a seven-character "UUUNNNN" string turned into "its
 * corresponding 4-byte numeric EISA ID encoding" — and §6.1.5 calls what a
 * _HID then holds "a numeric 32-bit compressed EISA type ID". Neither
 * section writes the bit layout out, so what follows is a restatement of the
 * compression every PNP ID uses rather than a quotation: three five-bit
 * letters biased by '@' followed by four hexadecimal digits, laid out so the
 * whole thing reads as a little-endian DWord. (§19.6.30, cited here before,
 * is DerefOf and has nothing to do with EISA ids.) Decoding it is how
 * "PNP0A03" is recognised without a magic constant standing in for the
 * string.
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

/* One id value — or one package of them — against the seven characters of
 * `id`. ACPI 6.5 §6.1.2 lets _CID hold a single id or a package of them,
 * listed most specific first; any of them naming the id asked for matches. */
static bool AmlIdValueIs(const AmlValue *v, const char *who, const char *id) {
    if (v->type != AML_OBJ_PACKAGE) return AmlIdTextIs(v, id);

    const uint8_t *q = v->bytes;
    for (uint32_t i = 0; i < v->count && q && q < v->bytes_end; i++) {
        AmlValue e;
        if (!AmlReadOperand(&q, v->bytes_end, v->scope, who, &e))
            return false;
        if (AmlIdTextIs(&e, id)) return true;
    }
    return false;
}

/*
 * ‼ _HID IS A NAME ON SOME BOARDS AND A METHOD ON OTHERS, AND READING ONLY
 *   THE FIRST FORM LOSES THE BRIDGE.
 *
 * ACPI 6.5 §6.1.5 makes _HID an object "OR control method", and firmware that
 * decides its id from what the OS said it was — the shape Intel's reference
 * ASL uses around OSYS — writes Method (_HID). On such a board the
 * root-bridge walk finds nothing and every PCI function falls back to config
 * byte 0x3C. Both ids are read the way every other object on this path is
 * read, and a method that cannot be run is named on the console rather than
 * turned into "this device is not a bridge".
 *
 * ‼ AND _CID COUNTS. §6.1.2: a device whose _HID is vendor-specific declares
 * what it is COMPATIBLE with in _CID, and a root bridge carrying a vendor
 * _HID with _CID EISAID("PNP0A03") is an ordinary shape on shipping firmware.
 */
typedef struct AmlDeviceIds {
    AmlValue value[2];      /* _HID, then _CID */
    bool     have[2];
} AmlDeviceIds;

static const char *const g_aml_id_seg[2] = { "_HID", "_CID" };

/*
 * ‼ HOW MANY DEVICES COULD NOT SAY WHAT THEY ARE — one line, not one alarm
 *   each.
 *
 * AmlRoutingAdopt asks every device in the namespace, so a board whose _HID
 * methods this evaluator cannot run refuses in a run: six on one shipping
 * desktop board, six copies of one sentence, and nowhere in the log the
 * number that says whether a routing table was lost or a camera stub was.
 * The evaluator has already printed WHAT stopped it — the opcode, the space,
 * the region — at the point it stopped; what is added here is the count and
 * one example to find in the lines above.
 *
 * Said only when it has grown since the last time, so a later walk that
 * refuses nothing new stays quiet and one that does is not silent.
 */
static uint32_t    g_ids_refused       = 0;
static uint32_t    g_ids_refused_said  = 0;
static const char *g_ids_refused_first = NULL;
static const char *g_ids_refused_seg   = NULL;

static void AmlDeviceIdsRefusedSay(void) {
    if (g_ids_refused == g_ids_refused_said) return;

    kprintf("[AML] %[E]%u device(s) declare _HID or _CID in a form this "
            "evaluator could not read, so what they are is not known here. A "
            "PCI root bridge among them loses its whole _PRT and every line "
            "under it falls back to configuration byte 0x3C; an interrupt "
            "link among them is not recognised as one. The first was %s.%s, "
            "and the line that says what stopped the evaluator was printed "
            "where it stopped%[D]\n",
            (unsigned)(g_ids_refused - g_ids_refused_said),
            g_ids_refused_first ? g_ids_refused_first : "an unnamed device",
            g_ids_refused_seg ? g_ids_refused_seg : "_HID");

    g_ids_refused_said  = g_ids_refused;
    g_ids_refused_first = NULL;
    g_ids_refused_seg   = NULL;
}

static void AmlReadDeviceIds(AmlRun *run, aml_object_t *dev,
                              AmlDeviceIds *out) {
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < 2; i++) {
        aml_object_t *n = AmlChild(dev, g_aml_id_seg[i]);
        if (!n) continue;
        if (!AmlEvalIn(run, n, NULL, 0, &out->value[i])) {
            if (g_ids_refused_first == NULL) {
                g_ids_refused_first = dev->name;
                g_ids_refused_seg   = g_aml_id_seg[i];
            }
            g_ids_refused++;
            continue;
        }
        out->have[i] = true;
    }
}

static bool AmlIdsName(const AmlDeviceIds *ids, const char *id) {
    for (uint32_t i = 0; i < 2; i++)
        if (ids->have[i] && AmlIdValueIs(&ids->value[i], g_aml_id_seg[i], id))
            return true;
    return false;
}

static bool AmlHidIs(aml_object_t *dev, const char *id) {
    AmlRun run;
    AmlRunInit(&run);
    AmlDeviceIds ids;
    AmlReadDeviceIds(&run, dev, &ids);
    bool yes = AmlIdsName(&ids, id);
    AmlRunDone(&run);
    return yes;
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

    AmlRun run;
    AmlRunInit(&run);
    AmlValue v;
    bool ok = AmlEvalIn(&run, n, NULL, 0, &v);
    if (ok && v.type != AML_OBJ_INTEGER) {
        kprintf("[AML] %s.%s answered %s where an integer was expected\n",
                dev->name, n->name, AmlTypeName(v.type));
        ok = false;
    }
    if (ok) { *out = v.integer; *present = true; }
    AmlRunDone(&run);
    return ok;
}

/*
 * ‼ AND SOMETHING HAS TO ANSWER THERE.
 *
 * A configuration read of a function that is not present answers all ones on
 * the wire — PCI 3.0 §6.1 makes 0xFFFF an invalid Vendor ID for exactly that
 * purpose — and so does an ECAM access to a segment or a bus the MCFG never
 * mapped. Both would otherwise come back as a perfectly ordinary register
 * value and be believed as an interrupt number. The Vendor ID is the one
 * register every function must implement, so it is what this asks: if nobody
 * is at the address the namespace led to, the region does not describe a
 * register on this machine and the walk that found it went wrong somewhere.
 * Said, not read past.
 */
static bool AmlPciFunctionAnswers(uint16_t seg, uint8_t bus, uint8_t dev,
                                   uint8_t fn, const char *who,
                                   aml_object_t *region) {
    uint64_t vendor = 0;
    if (!AmlPciUnitRead(seg, bus, dev, fn, PCI_VENDOR_ID, 2, who, &vendor))
        return false;
    if (vendor == PCI_INVALID_VENDOR) {
        kprintf("[AML] %[E]%s: operation region %s resolves to %04x:%02x:%02x"
                ".%u, and no function answers there — its Vendor ID reads "
                "0xFFFF. The register this region names is not on this "
                "machine at that address, so nothing is read through it%[D]\n",
                who, region->name, seg, bus, dev, fn);
        return false;
    }
    return true;
}

/*
 * Where in PCI configuration space a PCI_Config OperationRegion actually
 * sits. Declared above AmlRegionRead; defined here, where the root-bridge
 * list it walks to is in scope.
 *
 * The chain from the region up to a root bridge is collected first and then
 * walked DOWNWARD, because a bus number is only known once the bridge above
 * it has been read: ACPI names the devices, and PCI 3.0 §3.2.5.5 puts the
 * secondary bus number in the bridge's own configuration header at 0x19.
 */
static bool AmlPciConfigSite(aml_object_t *region, const char *who,
                              uint16_t *seg, uint8_t *bus,
                              uint8_t *dev, uint8_t *fn) {
    /* Up to the root bridge, counting the steps. */
    AmlRootBridge *rb  = NULL;
    uint32_t       len = 0;
    for (aml_object_t *n = region->parent; n; n = n->parent) {
        for (AmlRootBridge *b = g_root_bridges; b; b = b->next)
            if (b->node == n) { rb = b; break; }
        if (rb) break;
        len++;
    }
    if (!rb) {
        kprintf("[AML] %s: operation region %s addresses PCI configuration "
                "space, and no PCI root bridge stands above it in the "
                "namespace — nothing says which bus its offsets belong to\n",
                who, region->name);
        return false;
    }

    *seg = rb->segment;
    *bus = rb->bus;
    *dev = 0;
    *fn  = 0;
    /* Declared in the host bridge itself — still an address something has
     * to answer at. */
    if (len == 0) return AmlPciFunctionAnswers(*seg, *bus, *dev, *fn, who,
                                               region);

    aml_object_t **chain =
        (aml_object_t **)kmalloc((size_t)len * sizeof(aml_object_t *));
    if (!chain) {
        kprintf("[AML] %s: the heap has no room to walk %u namespace levels "
                "down to operation region %s\n", who, len, region->name);
        return false;
    }
    uint32_t i = len;
    for (aml_object_t *n = region->parent; n && n != rb->node; n = n->parent)
        chain[--i] = n;

    bool ok = true;
    bool any = false;
    for (uint32_t k = 0; k < len && ok; k++) {
        aml_object_t *n = chain[k];
        if (n->type != AML_OBJ_DEVICE) continue;
        aml_object_t *adr = AmlChild(n, "_ADR");
        if (!adr) continue;
        if (adr->type != AML_OBJ_INTEGER) {
            /* Said, not followed: see the note on AmlPciConfigSite's
             * declaration for why a method _ADR is not run here. */
            kprintf("[AML] %s: device %s above operation region %s declares "
                    "its _ADR as %s rather than as an object, and this kernel "
                    "will not run a method to find out where to perform an "
                    "address-space access\n",
                    who, n->name, region->name, AmlTypeName(adr->type));
            ok = false;
            break;
        }
        /* §6.1.1: the device number is the high word and the function
         * number the low word, and 0xFFFF in the low word means "all
         * functions". A region has to be read from ONE of them, and
         * function 0 is the one that is always there. */
        uint64_t hi = (adr->v.integer >> 16) & 0xFFFFu;
        uint64_t lo = adr->v.integer & 0xFFFFu;
        if (lo == 0xFFFFu) lo = 0;
        if (hi > 31u || lo > 7u) {
            kprintf("[AML] %s: device %s above operation region %s declares "
                    "_ADR 0x%lx, and PCI 3.0 §3.2.2.3.4 gives a bus 32 "
                    "devices of 8 functions\n",
                    who, n->name, region->name,
                    (unsigned long)adr->v.integer);
            ok = false;
            break;
        }
        if (any) {
            /* The previous level was a bridge; its secondary bus carries
             * everything below it. PCI 3.0 §3.2.5.5 puts that number in the
             * bridge's own configuration header at offset 0x19 — read
             * through the same one door as every other configuration access
             * here, so that it goes to the SEGMENT the root bridge named and
             * not to segment 0 because 0xCF8 has no field for one. */
            uint64_t secondary = 0;
            if (!AmlPciUnitRead(*seg, *bus, *dev, *fn,
                                PCI_BRIDGE_SECONDARY_BUS, 1, who,
                                &secondary)) {
                ok = false;
                break;
            }
            *bus = (uint8_t)secondary;
        }
        *dev = (uint8_t)hi;
        *fn  = (uint8_t)lo;
        any  = true;
    }
    kfree(chain);
    if (!ok) return false;
    return AmlPciFunctionAnswers(*seg, *bus, *dev, *fn, who, region);
}


/*
 * The current interrupt setting of one PCI Interrupt Link device, read out of
 * the resource template its _CRS returns.
 *
 * ‼ WHAT `index` COUNTS. ACPI 6.5 §6.2.13 defines a _PRT entry's Source Index,
 * when Source names a link, as the index "which indicates which resource
 * descriptor in the resource template of the device pointed to in the Source
 * field this interrupt is allocated from" — a DESCRIPTOR in the template, not
 * an interrupt inside one descriptor. The descriptor it selects must name ONE
 * interrupt: §6.2.2 makes _CRS "the system resources currently allocated to
 * this device", and a link that currently carries one interrupt says one.
 * Anything else is refused by name, with the count printed.
 *
 * ACPI 6.5 §6.4.2.1, IRQ Descriptor (small tag 0x22 or 0x23): two mask bytes
 * carrying one bit per 8259 interrupt, then an OPTIONAL Information byte —
 * "Bit[0] Interrupt Mode: 1 = Edge Triggered, 0 = Level Triggered", "Bit[3]
 * Interrupt Polarity: 0 = Active-High, 1 = Active-Low", "Bit[4] Interrupt
 * Sharing". The same section: "If byte 3 is not included, High true,
 * edge-sensitive, non-shareable is assumed."
 *
 * ACPI 6.5 §6.4.3.6, Extended Interrupt Descriptor (0x89): "Bit[1] Interrupt
 * Mode: 1 = Edge-Triggered, 0 = Level-Triggered", "Bit[2] Interrupt Polarity:
 * 0 = Active-High, 1 = Active-Low", "Bit[3] Interrupt Sharing", followed by
 * the Interrupt Table Length and that many 32-bit interrupt numbers.
 *
 * ‼ AND THE DESCRIPTOR DESCRIBES THE WIRE, WHICH THE PCI BUS RULE ONLY
 *   GUESSES AT. MEASURED ON BOTH KINDS OF MACHINE.
 *
 * It is tempting to take only the number from here and let PCI 3.0 §2.2.6 —
 * INTx# is level sensitive, asserted low, open drain — decide the rest. That
 * is wrong, and the mistake is in WHERE the §2.2.6 wire ends: at the PIRQ
 * router. What the IO-APIC input sees is the router's OUTPUT, and only the
 * platform can say what that looks like.
 *
 *   * On a legacy PIRQ-to-ISA-IRQ platform — the i440fx this desk emulates,
 *     and every board of that generation — the router drives one of the
 *     sixteen 8259 lines, and those are active HIGH. The emulator says so
 *     twice over: the link's own descriptor carries flags 0x09 (level,
 *     active high, shared), and the MADT declares an Interrupt Source
 *     Override for each PCI-capable ISA IRQ with flags 0x0D, which is the
 *     same statement in the other table. Programming §2.2.6's active low
 *     there points the redirection entry at a line that idles low — a
 *     permanently asserted input, which is the storm §2.2.6 was invoked to
 *     avoid.
 *   * On a modern PCH — the owner's B365 — PIRQA-H# reach IO-APIC inputs
 *     16 to 23 directly, those ARE active low, there is no MADT override
 *     above 15 to say so, and the link's descriptor says active low.
 *
 * One rule covers both: BELIEVE THE LINK. Its _CRS is the board's own
 * statement about the very pin, in the very interrupt mode this kernel put
 * it in, and it is the most specific thing anybody has to say. §2.2.6 stays
 * as the default for a line nothing described — a directly numbered _PRT
 * entry names a GSI and no wire, and pci.c applies the bus rule there.
 *
 * A descriptor that disagrees with §2.2.6 is still PRINTED, because on a
 * live board that disagreement is the first thing an operator needs to see.
 *
 * `needs_srs` comes back true for the one failure that has a named cure: the
 * link is not carrying an interrupt, and giving it one means writing the
 * chipset's routing register through _SRS.
 */
static bool AmlIrqFromResources(const uint8_t *p, const uint8_t *end,
                                 uint32_t index, const char *who,
                                 AmlRoute *out, bool *needs_srs) {
    uint32_t at = 0;                 /* which descriptor of the template */
    *needs_srs = false;

    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0x79) break;                       /* End Tag — §6.4.2.9 */

        const uint8_t *body;
        uint32_t       len;
        bool           small = ((tag & 0x80u) == 0);
        if (small) {
            len = (uint32_t)(tag & 0x07u);
            if (p + 1 + len > end) break;
            body = p + 1;
        } else {
            /* Large descriptor: the two bytes after the tag are its length. */
            if (p + 3 > end) break;
            len  = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
            if (p + 3 + len > end) break;
            body = p + 3;
        }

        if (at++ != index) { p = body + len; continue; }

        bool     level = false, active_low = false, shareable = false;
        uint32_t gsi   = 0;

        if (small && ((tag >> 3) & 0x0Fu) == 0x04 && len >= 2) {
            uint32_t mask = (uint32_t)body[0] | ((uint32_t)body[1] << 8);
            uint32_t seen = 0, only = 0;
            for (uint32_t bit = 0; bit < 16; bit++)
                if (mask & (1u << bit)) { seen++; only = bit; }

            if (seen == 0) {
                kprintf("[AML] interrupt link %s holds no interrupt in its "
                        "_CRS: it is not configured\n", who);
                *needs_srs = true;
                return false;
            }
            if (seen > 1) {
                kprintf("[AML] interrupt link %s: its _CRS offers %u "
                        "interrupts at once, so it names no current setting "
                        "— §6.2.2 makes _CRS the resources allocated NOW, and "
                        "picking one of them would be this kernel choosing "
                        "the link's routing instead of reading it\n",
                        who, seen);
                *needs_srs = true;
                return false;
            }
            /* ‼ A BIT IN THIS MASK IS AN 8259 INTERRUPT, NOT A GLOBAL ONE.
             * §6.4.2.1 gives the two bytes as "IRQ mask bits[15:0], _INT,
             * bit 0 is IRQ0, bit 1 is IRQ1" — the sixteen inputs of the
             * dual 8259, which is what a small IRQ descriptor was defined
             * against. What the IO-APIC calls that same line is the MADT's
             * business: §5.2.12.5's Interrupt Source Override exists
             * precisely because the mapping is the identity "except where
             * the firmware says otherwise", and the usual exception is the
             * timer on IRQ 0 arriving as GSI 2. Handing the bit number
             * straight through would be right on most boards and silently
             * wrong on the ones that publish an override — so the number is
             * put through the same translation every ISA line in this
             * kernel goes through, and a translation that MOVED it is said
             * out loud, because on a live board that is the first thing an
             * operator needs to see. The extended descriptor below needs
             * none of this: §6.4.3.6 numbers ARE global system interrupts. */
            Summons isa = SummonsForIsaLine((uint8_t)only);
            gsi = isa.gsi;
            if (isa.gsi != only)
                kprintf("[AML] interrupt link %s carries 8259 interrupt %u, "
                        "which this machine's MADT maps onto global system "
                        "interrupt %u — the routing table's number is the "
                        "8259's and the pin is the override's\n",
                        who, only, isa.gsi);
            if (len >= 3) {
                uint8_t f = body[2];
                level      = ((f & 0x01u) == 0);
                active_low = ((f & 0x08u) != 0);
                shareable  = ((f & 0x10u) != 0);
            } else {
                /* §6.4.2.1's stated default for the two-byte form. */
                level = false; active_low = false; shareable = false;
            }
        } else if (!small && tag == 0x89 && len >= 2) {
            uint8_t flags = body[0];
            uint8_t count = body[1];
            if (count == 0) {
                kprintf("[AML] interrupt link %s holds no interrupt in its "
                        "_CRS: it is not configured\n", who);
                *needs_srs = true;
                return false;
            }
            if (count > 1) {
                kprintf("[AML] interrupt link %s: the descriptor its routing "
                        "entry points at lists %u interrupts at once, so it "
                        "names no current setting — §6.2.2 makes _CRS the "
                        "resources allocated NOW\n", who, count);
                *needs_srs = true;
                return false;
            }
            if (len < 2u + 4u) {
                kprintf("[AML] interrupt link %s: its extended interrupt "
                        "descriptor says it carries one interrupt and is too "
                        "short to hold it\n", who);
                return false;
            }
            gsi = (uint32_t)body[2] | ((uint32_t)body[3] << 8) |
                  ((uint32_t)body[4] << 16) | ((uint32_t)body[5] << 24);
            level      = ((flags & 0x02u) == 0);
            active_low = ((flags & 0x04u) != 0);
            shareable  = ((flags & 0x08u) != 0);
        } else {
            kprintf("[AML] interrupt link %s: the routing entry points at "
                    "descriptor %u of its _CRS, and that descriptor (tag "
                    "0x%02x) carries no interrupt\n", who, index, tag);
            return false;
        }

        /* Said out loud when it differs from what §2.2.6 makes of the wire
         * on the PCI side of the router, because that difference is a fact
         * about the board an operator cannot get anywhere else — and because
         * it is what makes the line arrive described as active high on a
         * machine whose every other table says the same. */
        if (!level || !active_low) {
            kprintf("[AML] interrupt link %s carries interrupt %u and "
                    "describes that wire as %s, %s — not the %s, %s that PCI "
                    "3.0 §2.2.6 makes of an INTx# line on the device side of "
                    "the PIRQ router. The link is describing the router's "
                    "output, which is the pin the controller sees, so the "
                    "link is believed\n",
                    who, gsi, level ? "level" : "edge",
                    active_low ? "active low" : "active high",
                    "level", "active low");
        }

        memset(out, 0, sizeof(*out));
        out->gsi          = gsi;
        out->level        = level;
        out->active_low   = active_low;
        out->shareable    = shareable;
        out->wiring_known = true;
        out->named_by     = "the link's own _CRS";
        return true;
    }

    kprintf("[AML] interrupt link %s: its _CRS holds no descriptor number "
            "%u, so nothing in it says which interrupt the routing entry "
            "means\n", who, index);
    return false;
}

/* What a link COULD carry — §6.2.12, _PRS. Printed, never chosen from:
 * choosing is what _SRS is for, and _SRS is a write. */
static void AmlSayPossible(const uint8_t *p, const uint8_t *end,
                            const char *who) {
    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0x79) break;
        const uint8_t *body;
        uint32_t       len;
        bool           small = ((tag & 0x80u) == 0);
        if (small) {
            len = (uint32_t)(tag & 0x07u);
            if (p + 1 + len > end) break;
            body = p + 1;
        } else {
            if (p + 3 > end) break;
            len  = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
            if (p + 3 + len > end) break;
            body = p + 3;
        }
        if (small && ((tag >> 3) & 0x0Fu) == 0x04 && len >= 2) {
            uint32_t mask = (uint32_t)body[0] | ((uint32_t)body[1] << 8);
            for (uint32_t bit = 0; bit < 16; bit++)
                if (mask & (1u << bit))
                    kprintf("[AML]   %s could be given 8259 interrupt %u "
                            "(global system interrupt %u)\n", who, bit,
                            SummonsForIsaLine((uint8_t)bit).gsi);
        } else if (!small && tag == 0x89 && len >= 2) {
            uint32_t n = body[1];
            for (uint32_t i = 0; i < n && 2u + 4u * (i + 1u) <= len; i++)
                kprintf("[AML]   %s could be given interrupt %lu\n", who,
                        (unsigned long)((uint32_t)body[2 + 4 * i] |
                            ((uint32_t)body[3 + 4 * i] << 8) |
                            ((uint32_t)body[4 + 4 * i] << 16) |
                            ((uint32_t)body[5 + 4 * i] << 24)));
        }
        p = body + len;
    }
}

/* A link that carries nothing needs one chosen and PROGRAMMED, and
 * programming it is _SRS, which is a write. Named as the missing capability
 * rather than worked around in silence. */
static void AmlSaySrsIsMissing(AmlRun *run, aml_object_t *link) {
    aml_object_t *prs = AmlChild(link, "_PRS");
    if (prs) {
        AmlValue v;
        if (AmlEvalIn(run, prs, NULL, 0, &v) && v.type == AML_OBJ_BUFFER)
            AmlSayPossible(v.bytes, v.bytes_end, link->name);
    }
    kprintf("[AML] %[E]interrupt link %s carries no interrupt, and giving it "
            "one means picking a number out of its _PRS and WRITING it to the "
            "chipset's routing register through %s._SRS (ACPI 6.5 §6.2.16). "
            "That register is in SystemIO or PCI configuration space on every "
            "board, and this kernel writes in neither — the one store it "
            "makes is into memory the firmware's own map calls ACPI NVS. So "
            "_SRS is a capability it does not have, and the lines behind this "
            "link keep their weaker source%[D]\n",
            link->name, link->name);
}

static bool AmlLinkRoute(aml_object_t *link, uint32_t index, AmlRoute *out) {
    if (!AmlHidIs(link, "PNP0C0F")) {
        kprintf("[AML] a routing entry names %s as its interrupt source, but "
                "that object does not declare _HID PNP0C0F — it is not a PCI "
                "interrupt link, and this kernel reads no other kind of "
                "source\n", link->name);
        return false;
    }
    if (g_pic == AML_PIC_UNKNOWN) {
        /* ‼ WHAT \_PIC'S FAILURE DOES AND DOES NOT PUT IN DOUBT, AND WHY
         * THE LINK SURVIVES IT.
         *
         * A table variant decides what a DIRECTLY NUMBERED entry means: the
         * PIC-mode table names 8259 interrupts and the APIC-mode one names
         * global ones, and with \_PIC unrun this kernel cannot tell which it
         * was handed — so AmlPrtLookup refuses those, and still does.
         *
         * A LINK is a different fact. §6.2.2 makes its _CRS the resources
         * ALLOCATED NOW, and what it reads to find them is the chipset's own
         * PIRQ routing register: one register, holding one number, whichever
         * table the firmware would hand over. The link device is the same
         * device in both variants and its _CRS does not branch on the mode.
         * Refusing it would drop the line back to configuration byte 0x3C,
         * which — PCI 3.0 §6.2.4 footnote 43 — is firmware's note of the
         * same 8259 interrupt, written earlier and by nobody in particular.
         * Discarding a register read in favour of a note about it is not
         * caution; it is losing the better of two statements of one fact. */
        /* Said once for the machine, not once per line: the fact is about
         * the firmware, and a board with 128 routing entries would
         * otherwise print it 128 times and bury everything else. */
        static bool said_once = false;
        if (!said_once) {
            said_once = true;
            kprintf("[AML] this firmware declares \\_PIC and it could not be "
                    "run, so which of its routing tables it is holding out "
                    "is not known. Its interrupt links are still read — a "
                    "link's _CRS reads the chipset's own routing register, "
                    "which holds one number in either interrupt model. What "
                    "the unknown variant costs is every DIRECTLY numbered "
                    "entry, and those are refused; %s is the first link read "
                    "this way\n", link->name);
        }
    }

    AmlRun run;
    AmlRunInit(&run);
    bool ok = false;

    /* ‼ IS THE LINK EVEN ON. ACPI 6.5 §6.3.7 gives _STA bit 0 "the device is
     * present" and bit 1 "the device is enabled and decoding its resources".
     * A link that is present but not enabled is not carrying anything, and
     * its _CRS says so; asking anyway and believing the answer is how a
     * device gets armed on a line nothing drives. _STA is optional — §6.3.7
     * makes an absent one mean present and enabled. */
    aml_object_t *sta = AmlChild(link, "_STA");
    if (sta) {
        AmlValue v;
        if (!AmlEvalIn(&run, sta, NULL, 0, &v)) {
            kprintf("[AML] interrupt link %s: its _STA could not be read, so "
                    "whether it is enabled is not known — refused rather than "
                    "assumed\n", link->name);
            AmlRunDone(&run);
            return false;
        }
        if (v.type != AML_OBJ_INTEGER) {
            kprintf("[AML] interrupt link %s: its _STA answered %s where "
                    "§6.3.7 gives an integer of status bits\n",
                    link->name, AmlTypeName(v.type));
            AmlRunDone(&run);
            return false;
        }
        if (!(v.integer & 0x2u)) {
            kprintf("[AML] interrupt link %s answers _STA 0x%lx: %s\n",
                    link->name, (unsigned long)v.integer,
                    (v.integer & 0x1u) ? "present, and NOT enabled"
                                       : "not present at all");
            AmlSaySrsIsMissing(&run, link);
            AmlRunDone(&run);
            return false;
        }
    }

    aml_object_t *crs = AmlChild(link, "_CRS");
    if (!crs) {
        kprintf("[AML] interrupt link %s declares no _CRS, so nothing says "
                "which interrupt it currently carries\n", link->name);
        AmlRunDone(&run);
        return false;
    }

    AmlValue v;
    if (!AmlEvalIn(&run, crs, NULL, 0, &v)) {
        kprintf("[AML] interrupt link %s: its _CRS could not be evaluated to "
                "the end — the lines behind it fall back to the weaker "
                "source\n", link->name);
        AmlRunDone(&run);
        return false;
    }
    if (v.type != AML_OBJ_BUFFER) {
        kprintf("[AML] interrupt link %s: its _CRS answered %s where a "
                "resource template was expected\n",
                link->name, AmlTypeName(v.type));
        AmlRunDone(&run);
        return false;
    }

    bool needs_srs = false;
    ok = AmlIrqFromResources(v.bytes, v.bytes_end, index, link->name, out,
                             &needs_srs);
    if (!ok && needs_srs) AmlSaySrsIsMissing(&run, link);
    AmlRunDone(&run);
    return ok;
}

/*
 * A name path written as ASCII TEXT, resolved against `scope`.
 *
 * ‼ A _PRT ENTRY'S SOURCE COMES IN TWO SPELLINGS AND ONLY ONE WAS READ.
 * The ASL compiler normally emits Source as a NameString operand, which
 * AmlReadOperand hands back as the object itself. Shipping firmware also
 * emits it as a String, and ACPICA accepts one there —
 * acpi_rs_create_pci_routing_table takes ACPI_TYPE_STRING in that field — so
 * a board that writes the string form works under every other OS and had its
 * entry refused here. Together with the all-or-nothing element loop that
 * used to stand below, one such entry threw away the routing of every device
 * listed after it.
 *
 * The grammar is §20.2.2's, spelled in characters instead of bytes: an
 * optional '\' naming the root, any number of '^' each naming a parent, and
 * name segments separated by '.'. A segment shorter than four characters is
 * padded with '_', which is what makes a NameSeg four bytes to begin with.
 * §5.3's search rules apply here exactly as they do to the encoded form —
 * only a single bare segment is searched for upward.
 */
static aml_object_t *AmlNodeForTextPath(const uint8_t *text, uint32_t len,
                                         aml_object_t *scope) {
    aml_object_t *base = scope ? scope : &g_root_obj;
    uint32_t     i        = 0;
    bool         rooted   = false;
    bool         prefixed = false;

    if (i < len && text[i] == '\\') { base = &g_root_obj; rooted = true; i++; }
    while (i < len && text[i] == '^') {
        if (!base->parent) return NULL;     /* §5.3: no names above the root */
        base = base->parent;
        prefixed = true;
        i++;
    }
    if (i >= len) return base;

    uint32_t segs = 1;
    for (uint32_t k = i; k < len; k++) if (text[k] == '.') segs++;
    bool search_rules = (!rooted && !prefixed && segs == 1);

    aml_object_t *cur = base;
    while (i < len) {
        char     seg[4] = { '_', '_', '_', '_' };
        uint32_t n      = 0;
        while (i < len && text[i] != '.') {
            if (n < 4) seg[n] = (char)text[i];
            n++;
            i++;
        }
        if (i < len) i++;                            /* the '.' */
        if (n == 0 || n > 4) return NULL;

        aml_object_t *next = obj_find_child(cur, seg);
        if (!next && search_rules) {
            for (aml_object_t *up = base->parent; up; up = up->parent) {
                next = obj_find_child(up, seg);
                if (next) break;
            }
        }
        if (!next) return NULL;
        cur = next;
    }
    return cur;
}

/* One _PRT element: Package { Address, Pin, Source, SourceIndex } —
 * ACPI 6.5 §6.2.13. */
static bool AmlPrtElement(const AmlValue *row, const char *who,
                           AmlPrtEntry *e) {
    if (row->type != AML_OBJ_PACKAGE) {
        kprintf("[AML] %s._PRT: an entry is %s where a package of four was "
                "expected\n", who, AmlTypeName(row->type));
        return false;
    }
    if (row->count < 4) {
        kprintf("[AML] %s._PRT: an entry declares %u fields; §6.2.13 gives it "
                "four\n", who, row->count);
        return false;
    }

    const uint8_t *q = row->bytes;
    AmlValue f[4];
    for (uint32_t i = 0; i < 4; i++)
        if (!AmlReadOperand(&q, row->bytes_end, row->scope, who, &f[i]))
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
    } else if (f[2].type == AML_OBJ_STRING) {
        uint32_t tlen = (uint32_t)(f[2].bytes_end - f[2].bytes);
        if (tlen == 0) {
            /* A Source that is an empty string names no device, so the only
             * reading §6.2.13 leaves is its other one: the interrupt comes
             * from the global pool and Source Index is its number. ACPICA
             * and Linux read an empty Source the same way. */
            e->direct = true;
        } else {
            e->link = AmlNodeForTextPath(f[2].bytes, tlen, row->scope);
            if (!e->link) {
                kprintf("[AML] %s._PRT: an entry names \"%.*s\" as its "
                        "interrupt source and no object in the namespace "
                        "answers to that path\n",
                        who, (int)tlen, (const char *)f[2].bytes);
                return false;
            }
            e->direct = false;
        }
    } else {
        kprintf("[AML] %s._PRT: an entry's Source is %s — §6.2.13 gives it "
                "the integer zero or the name of an interrupt link\n",
                who, AmlTypeName(f[2].type));
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

    /* The run lives until every entry has been copied out of it: a _PRT that
     * is a method may have BUILT the package it returned, and those bytes are
     * this run's. */
    AmlRun run;
    AmlRunInit(&run);
    AmlValue v;
    if (!AmlEvalIn(&run, prt, NULL, 0, &v)) {
        kprintf("[AML] %s._PRT could not be read — every line it would have "
                "described falls back to the weaker source\n", dev->name);
        AmlRunDone(&run);
        return t;
    }
    if (v.type != AML_OBJ_PACKAGE) {
        kprintf("[AML] %s._PRT answered %s where a package of entries was "
                "expected\n", dev->name, AmlTypeName(v.type));
        AmlRunDone(&run);
        return t;
    }
    if (v.count == 0) {
        kprintf("[AML] %s._PRT is empty: this firmware describes no interrupt "
                "routing under it\n", dev->name);
        AmlRunDone(&run);
        return t;
    }

    /* One allocation, sized by the number of entries the FIRMWARE declares.
     * Nothing here decides how many a board may have. */
    t->entry = (AmlPrtEntry *)kmalloc((size_t)v.count * sizeof(AmlPrtEntry));
    if (!t->entry) {
        kprintf("[AML] the heap has no room for %s's %u routing entries\n",
                dev->name, v.count);
        AmlRunDone(&run);
        return t;
    }

    /*
     * ‼ ONE ENTRY THIS KERNEL CANNOT READ IS ONE ENTRY, NOT THE TABLE.
     *
     * What stood here broke out of the loop on the first refused element, so
     * a single entry naming a link that this pass missed — declared behind
     * control flow, which aml.h admits happens — discarded the routing of
     * every device listed AFTER it, and the whole bus fell back to config
     * byte 0x3C. The stand could never show it: all 128 of the emulator's
     * entries are the same shape.
     *
     * The two failures are not the same failure. A refused ELEMENT is
     * skipped and named: the entries around it are still the board's own
     * statement about other devices, and nothing about them is made less
     * true by the one that could not be read. A stream that cannot be
     * ADVANCED is the end of the table — the next element's first byte is no
     * longer known, so anything read past it would be invented.
     */
    const uint8_t *q = v.bytes;
    uint32_t got = 0, skipped = 0, seen = 0;
    while (seen < v.count && q < v.bytes_end) {
        AmlValue row;
        if (!AmlReadOperand(&q, v.bytes_end, v.scope, dev->name, &row)) {
            kprintf("[AML] %s._PRT: entry %u cannot be stepped over, so "
                    "where the next one begins is no longer known — the "
                    "table ends here\n", dev->name, seen);
            break;
        }
        seen++;
        if (AmlPrtElement(&row, dev->name, &t->entry[got])) got++;
        else skipped++;
    }
    t->count = got;
    if (got != v.count) {
        kprintf("[AML] %s._PRT declares %u entries: %u read, %u refused one "
                "by one, %u never reached\n",
                dev->name, v.count, got, skipped, v.count - seen);
    } else {
        kprintf("[AML] %s._PRT: %u routing entries read\n", dev->name, got);
    }
    AmlRunDone(&run);
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
            /* ‼ A DIRECTLY NUMBERED ENTRY IS THE ORDINARY APIC-MODE FORM, AND
             * THIS DESK NEVER SHOWS IT. §6.2.13: "If this field is the
             * integer zero, then the interrupt is allocated from the global
             * interrupt pool" and Source Index "is the global system
             * interrupt number". That is the AR00 package of the Intel
             * reference ASL and it is what the owner's B365 board answers
             * with once \_PIC(1) has been taken — while the emulator here
             * routes all 128 of its entries through a link, so nothing on
             * this desk exercises the branch below. It is written from
             * §6.2.13 alone, and the \_PIC state is the whole of its
             * condition: the number is a global system interrupt exactly
             * when the table it came out of is the one this kernel's
             * interrupt model asked for, or when the board has only one
             * table to give. */
            if (g_pic == AML_PIC_UNKNOWN) {
                kprintf("[AML] %s._PRT names interrupt %u for pin %u of "
                        "device 0x%04x, and this firmware declares \\_PIC but "
                        "would not run it — which of its two tables this "
                        "number came out of is not known\n",
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

/* =====================================================================
 * THE NARROW GATE — the whole of \_PIC, or none of it.
 *
 * ‼ WHY A GATE AT ALL, AND WHY IT IS SHAPED LIKE THIS.
 *
 * On the owner's board this file printed, and was right to print:
 *
 *   [AML] _PIC: stores into GPIC, which is a field of an operation region —
 *   this kernel reads address spaces and writes none, so the method is
 *   abandoned here rather than half-run
 *
 * The refusal was correct and the consequence was not. §5.8.1 puts the cost
 * of silence on the firmware — never told, it must assume PIC mode — so a
 * \_PIC that is declared and cannot be run leaves this kernel unable to say
 * which of two routing tables it is being handed, and every directly
 * numbered entry of that table is then refused. On a modern PCH that is the
 * ordinary form, so the loss is most of the board's own statement about its
 * interrupts.
 *
 * What was actually wrong was not the refusal but the ORDER of it: the
 * method was entered, run part-way, and abandoned at the first store. A
 * firmware method stopped in the middle is the worst of the three outcomes —
 * worse than running it and worse than leaving it alone — because the
 * platform is now in a state neither side has a name for.
 *
 * So the decision moves to before the first instruction. This walk takes
 * every byte of \_PIC and of every method it calls, takes BOTH sides of
 * every If (which branch runs depends on values this pass does not have),
 * and asks of each operation only: is this one this evaluator performs, and
 * is this store one this kernel may carry out. If the walk reaches the end,
 * the method is run for real with those stores allowed. If it stumbles
 * anywhere, it says where, and NOTHING is run — the behaviour the board
 * already had, arrived at without touching the platform first.
 *
 * ‼ WHAT IT PROVES. Every OPERATION in the method is one this evaluator
 * performs, and every STORE lands somewhere this kernel may carry it out —
 * that second question goes to AmlFieldWritable, which is the same function
 * the writer asks again on its way to the register, so the two answers
 * cannot drift apart.
 *
 * ‼ AND WHAT IT DOES NOT, WHICH WAS WRITTEN HERE TOO SMALL. What stood in
 * this paragraph said the walk "can never admit one it could not" run, and
 * named the whole residue as a register that might not answer and a divisor
 * that might come out zero. That list was short by its largest item: THIS
 * WALK HAS NO VALUES, SO IT HAS NO TYPES. The machine computes with integers
 * and with nothing else, and it says so in five places — the folder, an If's
 * predicate, and a store into a field of a region, into a namespace name, or
 * into a buffer field. Which of those fires is decided by a value the text
 * does not carry: what Arg0 was handed, what a name holds today, what a
 * called method gave back (one that returns nothing yields an object of no
 * type at all). So a method this gate passes can still be handed a buffer
 * where an integer was owed, and stop there, in the middle, with earlier
 * stores already landed. That is the very outcome the gate was built to
 * exclude, and it is narrower than it was rather than gone.
 *
 * What covers the residue is not a claim but a count — AmlRun::writes — and
 * a method that was proven and then failed anyway says out loud how many
 * stores had already landed. Closing it instead would mean carrying a type
 * for every Local, every Arg and every method's return value across both
 * sides of every If: the evaluator written a second time, and still ending
 * at "unknown" wherever the caller or the firmware supplies the value. A
 * gate that cannot close a hole is worth more saying so than claiming it.
 *
 * ‼ WHERE THE TEXT ALONE DECIDES, THIS WALK DECIDES WITH THE MACHINE. Three
 * of the machine's refusals need no values at all, and all three are made
 * here rather than left to the middle of a run: a Name whose value is
 * another name yields nothing that can be cut, because the evaluator hands
 * back the node and does not dereference it; a Buffer declared with no bytes
 * is not a buffer a CreateXField can cut; and a cut that reaches past the
 * end of the buffer it names is refused where it is written.
 *
 * ‼ AND ABOUT TEXT IT IS STRICTER THAN THE MACHINE, ON PURPOSE. Statements
 * standing after a Return in the same block are dead, and this walk proves
 * them anyway, because it does not know which Return fires; both sides of
 * every If are proven and only one of them will run; a cut nothing ever
 * reads is refused all the same. Each of those can only refuse a method the
 * machine could have carried, which is the direction an all-or-nothing gate
 * has to err in — and each is a statement about TEXT, which is the only
 * thing this walk sees.
 *
 * ‼ AND IT ADDS NO While, ALTHOUGH THE MACHINE NOW RUNS ONE. The obvious way
 * to widen a gate like this is to start walking loops, and it is not taken:
 * neither \_PIC of the two real firmware bodies this was built against
 * contains one, this walk refuses DefWhile, Break and Continue by name (see
 * AmlGateStep), and a board whose \_PIC loops is left exactly where it is
 * today. The machine refuses a loop only when it stops getting somewhere,
 * which is half-way through — and half-way through is the one outcome an
 * all-or-nothing gate exists to prevent. If that ever has to change, the
 * bound on it will be a DEADLINE taken from the clockboard on entry, not a
 * count of iterations — an iteration count is a number nobody can justify,
 * and the thing actually being protected is wall-clock time.
 * ===================================================================== */

/* What the walk still owes itself, one byte per item. The same technique the
 * parse pass uses in AmlSkipRun, and for the same reason: an operand nests
 * as deeply as the firmware wrote it, and the C stack is not where that
 * depth belongs. */
#define AML_GATE_TERM    0u      /* one TermArg   §20.2.5 */
#define AML_GATE_TARGET  1u      /* one Target    §20.2.2 */

/* One object a method declares for itself — Name, or a CreateXField view.
 *
 * ‼ A LENGTH AND NOT A FLAG, BECAUSE THE MACHINE KEEPS A LENGTH. A method's
 * own buffer is a COPY the run makes (AmlDeclareLocal), and it is made only
 * where the declaration handed it bytes: a Name whose value is another name
 * is not dereferenced and yields none, and a Buffer declared empty has none
 * to give. Carrying the same number the run will carry is what lets this
 * walk answer a CreateXField exactly as the run answers it rather than
 * approximately. Zero means there is nothing here that can be cut. */
typedef struct AmlGateName {
    char     name[4];
    uint32_t buf_len;            /* bytes the run's own copy would hold */
} AmlGateName;

/* One TermList of the method being proven: its body, or an If or Else body. */
typedef struct AmlGateBlock {
    const uint8_t *p;
    const uint8_t *end;
} AmlGateBlock;

typedef struct AmlGate {
    aml_object_t **todo;  uint32_t todon,  todo_room;  /* methods still to prove */
    aml_object_t **seen;  uint32_t seenn,  seen_room;  /* methods already proven */
    AmlGateBlock  *blk;   uint32_t blkn,   blk_room;
    AmlGateName   *obj;   uint32_t objn,   obj_room;
    uint8_t       *owed;  uint32_t owed_room;

    aml_object_t  *now;          /* the method being proven — names resolve here */
    const char    *asked_for;    /* what the whole walk is for, named in refusals */
    uint32_t       writes;
    uint32_t       reads;
    uint32_t       methods;
} AmlGate;

/* Where it stumbled, said once and in full, because on a live board this is
 * the only account anybody gets of why the platform was left untold. */
static bool AmlGateSay(AmlGate *g, const uint8_t *at, const char *what,
                        const char *tail) {
    const uint8_t *from = g->now->v.method.aml;
    kprintf("[AML] %[E]%s cannot be run whole — at byte %lu of %s's body "
            "stands %s%s. Not one byte of %s is run: a firmware method "
            "carried half-way leaves the platform in a state neither side "
            "has a name for, and this gate is all or nothing%[D]\n",
            g->asked_for,
            (unsigned long)((at >= from) ? (size_t)(at - from) : 0u),
            g->now->name, what, tail ? tail : "", g->asked_for);
    return false;
}

static bool AmlGateNoRoom(AmlGate *g, const char *forwhat) {
    kprintf("[AML] %[E]%s cannot be run whole: the heap has no room to %s, so "
            "this walk cannot finish and an unfinished proof proves nothing. "
            "Nothing is run%[D]\n", g->asked_for, forwhat);
    return false;
}

static bool AmlGateOwe(AmlGate *g, uint32_t *n, uint8_t kind) {
    uint8_t *grown = (uint8_t *)AmlStackRoom(g->owed, &g->owed_room, *n + 1, 1);
    if (!grown) return AmlGateNoRoom(g, "measure an operand nested this deep");
    g->owed = grown;
    g->owed[(*n)++] = kind;
    return true;
}

static bool AmlGatePush(AmlGate *g, const uint8_t *p, const uint8_t *end) {
    AmlGateBlock *grown = (AmlGateBlock *)AmlStackRoom(g->blk, &g->blk_room,
                                                       g->blkn + 1,
                                                       sizeof(AmlGateBlock));
    if (!grown) return AmlGateNoRoom(g, "walk into a block nested this deep");
    g->blk = grown;
    g->blk[g->blkn].p   = p;
    g->blk[g->blkn].end = end;
    g->blkn++;
    return true;
}

static bool AmlGateHasName(AmlGate *g, const char seg[4], uint32_t *buf_len) {
    for (uint32_t i = 0; i < g->objn; i++) {
        AmlGateName *o = &g->obj[i];
        if (o->name[0] == seg[0] && o->name[1] == seg[1] &&
            o->name[2] == seg[2] && o->name[3] == seg[3]) {
            if (buf_len) *buf_len = o->buf_len;
            return true;
        }
    }
    return false;
}

static bool AmlGateDeclare(AmlGate *g, const char seg[4], uint32_t buf_len) {
    AmlGateName *grown = (AmlGateName *)AmlStackRoom(g->obj, &g->obj_room,
                                                     g->objn + 1,
                                                     sizeof(AmlGateName));
    if (!grown) return AmlGateNoRoom(g, "hold the objects this method declares");
    g->obj = grown;
    g->obj[g->objn].name[0] = seg[0]; g->obj[g->objn].name[1] = seg[1];
    g->obj[g->objn].name[2] = seg[2]; g->obj[g->objn].name[3] = seg[3];
    g->obj[g->objn].buf_len = buf_len;
    g->objn++;
    return true;
}

/* A method this walk must prove as well, because the one being walked calls
 * it. Queued once: a method already proven, or already queued, is not queued
 * again — which is also what keeps a method that calls itself from queueing
 * work for ever. */
static bool AmlGateOwesMethod(AmlGate *g, aml_object_t *m) {
    for (uint32_t i = 0; i < g->seenn; i++) if (g->seen[i] == m) return true;
    for (uint32_t i = 0; i < g->todon; i++) if (g->todo[i] == m) return true;
    aml_object_t **grown = (aml_object_t **)AmlStackRoom(g->todo, &g->todo_room,
                                                         g->todon + 1,
                                                         sizeof(aml_object_t *));
    if (!grown) return AmlGateNoRoom(g, "hold the methods this one calls");
    g->todo = grown;
    g->todo[g->todon++] = m;
    return true;
}

/*
 * One literal, measured exactly and checked against what the evaluator will
 * make of it. `*was` comes back false when these bytes are not a literal at
 * all, and `*p` is then untouched.
 */
static bool AmlGateLiteral(AmlGate *g, const uint8_t **p, const uint8_t *end,
                            bool *was, uint32_t *buf_len) {
    *was = false;
    if (buf_len) *buf_len = 0;
    if (*p >= end) return true;
    const uint8_t *at = *p;
    uint8_t op = **p;

    switch (op) {
    case ZeroOp: case OneOp: case OnesOp:
    case BytePrefix: case WordPrefix: case DWordPrefix: case QWordPrefix: {
        *was = true;
        uint64_t v = 0;
        if (!read_const_integer(p, end, &v))
            return AmlGateSay(g, at, "an integer whose encoding runs past the "
                                     "end of the body", NULL);
        return true;
    }
    case StringPrefix: {
        *was = true;
        (*p)++;
        while (*p < end && **p) (*p)++;
        if (*p >= end)
            return AmlGateSay(g, at, "a string that runs past the end of the "
                                     "body", NULL);
        (*p)++;                                       /* the NUL */
        return true;
    }
    case BufferOp: case PackageOp: case VarPackageOp: {
        *was = true;
        (*p)++;
        uint32_t cb = 0;
        uint32_t pklen = decode_pkglen(p, end, &cb);
        if (!cb || pklen < cb)
            return AmlGateSay(g, at, "a buffer or package whose length is "
                                     "malformed", NULL);
        const uint8_t *body_start = *p + cb;
        const uint8_t *body_end   = *p + pklen;
        if (body_end > end)
            return AmlGateSay(g, at, "a buffer or package that runs past the "
                                     "end of the body", NULL);
        /* §20.2.5.4: a Buffer's size and a VarPackage's element count are
         * TermArgs, and the evaluator reads only a constant there — a count
         * it cannot read leaves the elements with no known start. A plain
         * Package's count is one ByteData and is always readable. */
        const uint8_t *q = body_start;
        uint64_t n = 0;
        if (op == PackageOp) {
            if (q >= body_end)
                return AmlGateSay(g, at, "a package that declares no element "
                                         "count", NULL);
        } else if (!read_const_integer(&q, body_end, &n)) {
            return AmlGateSay(g, at,
                              (op == BufferOp)
                                ? "a buffer whose size is not a constant this "
                                  "evaluator can read"
                                : "a variable package whose element count is "
                                  "not a constant, so its elements have no "
                                  "known start", NULL);
        }
        /* ‼ AND HOW MANY BYTES OF IT THE RUN WOULD ACTUALLY HOLD. §20.2.5.4
         * makes BufferSize the LOGICAL length and lets the ByteList be
         * shorter, the tail being zero; AmlReadOperand therefore keeps the
         * smaller of the two, and AmlDeclareLocal copies exactly that many
         * bytes — and copies nothing at all when there are none, leaving a
         * name that no CreateXField can cut. The same arithmetic taken here
         * is what keeps this walk from proving a cut the run will refuse. */
        if (buf_len && op == BufferOp) {
            uint64_t have = (uint64_t)(body_end - q);
            *buf_len = (uint32_t)((have < n) ? have : n);
        }
        *p = body_end;
        return true;
    }
    default:
        return true;                                  /* not a literal */
    }
}

/* One DataRefObject, as the evaluator reads the value of a Name: a literal,
 * or a name that yields the object it names without dereferencing it. */
static bool AmlGateData(AmlGate *g, const uint8_t **p, const uint8_t *end,
                         uint32_t *buf_len) {
    bool was = false;
    if (!AmlGateLiteral(g, p, end, &was, buf_len)) return false;
    if (was) return true;

    if (*p >= end)
        return AmlGateSay(g, end, "a declaration whose value is missing", NULL);
    uint8_t op = **p;
    if (aml_is_name_string_lead(op) && op != 0x00) {
        const uint8_t *at = *p;
        aml_object_t *o = resolve_name(p, end, g->now, false, NULL);
        if (!o)
            return AmlGateSay(g, at, "a declaration whose value names "
                                     "something this namespace does not hold",
                              NULL);
        /* ‼ AND IT YIELDS NOTHING THAT CAN BE CUT, WHATEVER IT NAMES. What
         * stood here read the named object's type and called the declaration
         * a buffer whenever that object was one — which is not what the run
         * does: AmlReadOperand hands back the NODE and no value, so the copy
         * AmlDeclareLocal would make has no bytes, and a CreateXField over
         * this name is refused in the middle of the method. The zero
         * AmlGateLiteral already left in *buf_len is the truth for a name
         * too, and it is left standing on purpose. */
        return true;
    }
    return AmlGateSay(g, *p, "a declaration whose value this evaluator does "
                             "not read", NULL);
}

/*
 * A read of an address space, judged on what the namespace already says:
 * that it is a Field of a real region, in one of the three spaces this
 * kernel reaches, and not one whose access protocol needs a write first.
 * Where the region turns out to BE, and whether anything answers there, are
 * facts about the running machine and belong to AmlRegionRead.
 */
static bool AmlGateMayRead(AmlGate *g, aml_object_t *field,
                            const uint8_t *at) {
    if (field->v.field.index || field->v.field.data)
        return AmlGateSay(g, at, "a read of an IndexField, which would mean "
                                 "writing its index register first: ",
                          field->name);
    if (field->v.field.bank)
        return AmlGateSay(g, at, "a read of a BankField, which would mean "
                                 "writing its bank selector first: ",
                          field->name);
    aml_object_t *rg = field->v.field.region;
    if (!rg || rg->type != AML_OBJ_REGION)
        return AmlGateSay(g, at, "a field of something this pass never "
                                 "recorded as an operation region: ",
                          field->name);
    if (rg->v.region.space > 0x02) {
        const char *nm = AmlSpaceName(rg->v.region.space);
        return AmlGateSay(g, at, "a read of an operation region in an address "
                                 "space this kernel reaches over no transport "
                                 "of its own: ",
                          nm ? nm : "one the specification has not named");
    }
    g->reads++;
    return true;
}

/*
 * One TermArg, or one Target, walked without producing a value. Left to
 * right, so what a nested operand needs is not a stack of positions but a
 * list of what is still OWED — the same shape AmlSkipRun uses in the parse
 * pass, and for the same reason: no C recursion, and no ceiling on nesting
 * but the heap.
 */
static bool AmlGateRun(AmlGate *g, const uint8_t **p, const uint8_t *end,
                        uint8_t first) {
    uint32_t n = 0;
    if (!AmlGateOwe(g, &n, first)) return false;

    while (n) {
        uint8_t kind = g->owed[--n];
        if (*p >= end)
            return AmlGateSay(g, end,
                              (kind == AML_GATE_TARGET)
                                ? "a Store that names no target"
                                : "a term that ends where an operand was "
                                  "expected", NULL);
        const uint8_t *at = *p;
        uint8_t        op = **p;

        if (kind == AML_GATE_TARGET) {
            if (op == 0x00)                { (*p)++; continue; }  /* NullName */
            if (op >= Local0Op && op <= Arg6Op) { (*p)++; continue; }
            if (op == ExtOpPrefix) {
                if ((size_t)(end - *p) >= 2 && (*p)[1] == ExtDebugOp) {
                    *p += 2;                          /* Debug: discarded */
                    continue;
                }
                return AmlGateSay(g, at, "a Store into an extended target "
                                         "this evaluator does not write",
                                  NULL);
            }
            char seg[4];
            if (AmlBareSeg(*p, end, seg) && AmlGateHasName(g, seg, NULL)) {
                (*p) += 4;                            /* the method's own */
                continue;
            }
            if (!aml_is_name_string_lead(op))
                return AmlGateSay(g, at, "a Store whose target is not a name",
                                  NULL);
            aml_object_t *o = resolve_name(p, end, g->now, false, NULL);
            if (!o)
                return AmlGateSay(g, at, "a Store into a name this namespace "
                                         "does not hold", NULL);
            if (o->type == AML_OBJ_FIELD) {
                /* ‼ THE GATE ITSELF. Asked here, with no side effect, about
                 * every store in the method — so a method holding one store
                 * this kernel may not carry out is refused before the stores
                 * it may are carried out. */
                AmlFieldReach reach;
                uint64_t      phys = 0;
                if (!AmlFieldWritable(o, g->asked_for, &reach, &phys))
                    return AmlGateSay(g, at, "a store this kernel may not "
                                             "carry out — the reason is on "
                                             "the line above — into ",
                                      o->name);
                g->writes++;
                continue;
            }
            if (o->type == AML_OBJ_REGION)
                return AmlGateSay(g, at, "a Store into an operation region "
                                         "named whole, which has no width and "
                                         "no offset of its own: ", o->name);
            if (o->type != AML_OBJ_INTEGER && o->type != AML_OBJ_NAME &&
                o->type != AML_OBJ_UNINIT)
                return AmlGateSay(g, at, "a Store that would overwrite "
                                         "something this evaluator does not "
                                         "convert: ", AmlTypeName(o->type));
            continue;
        }

        /* A TermArg. Literals first — none of their opcodes can begin a
         * name, so the order below settles nothing that is ambiguous. */
        bool was = false;
        if (!AmlGateLiteral(g, p, end, &was, NULL)) return false;
        if (was) continue;

        if (op >= Local0Op && op <= Arg6Op) { (*p)++; continue; }

        char seg[4];
        if (AmlBareSeg(*p, end, seg) && AmlGateHasName(g, seg, NULL)) {
            (*p) += 4;                                /* the method's own */
            continue;
        }

        uint8_t nargs = 0, ntargets = 0;
        if (AmlOperatorShape(op, &nargs, &ntargets)) {
            (*p)++;
            /* Pushed in reverse: the stream gives the arguments first and
             * the targets after, so the arguments come off the top first. */
            for (uint8_t i = 0; i < ntargets; i++)
                if (!AmlGateOwe(g, &n, AML_GATE_TARGET)) return false;
            for (uint8_t i = 0; i < nargs; i++)
                if (!AmlGateOwe(g, &n, AML_GATE_TERM)) return false;
            continue;
        }

        if (aml_is_name_string_lead(op) && op != 0x00) {
            aml_object_t *o = resolve_name(p, end, g->now, false, NULL);
            if (!o)
                return AmlGateSay(g, at, "a name this namespace does not "
                                         "hold, and no value is invented for "
                                         "one", NULL);
            if (o->type == AML_OBJ_METHOD) {
                /* A call. Its body has to be proven too, and its arguments
                 * follow it in the stream. */
                if (!AmlGateOwesMethod(g, o)) return false;
                uint8_t argc = (uint8_t)(o->v.method.flags & 0x7u);
                for (uint8_t i = 0; i < argc; i++)
                    if (!AmlGateOwe(g, &n, AML_GATE_TERM)) return false;
                continue;
            }
            if (o->type == AML_OBJ_FIELD) {
                if (!AmlGateMayRead(g, o, at)) return false;
                continue;
            }
            if (o->type == AML_OBJ_PACKAGE && !o->v.package.body)
                return AmlGateSay(g, at, "a package whose element count this "
                                         "pass could not read, so its "
                                         "elements have no known start: ",
                                  o->name);
            if (o->type == AML_OBJ_INTEGER || o->type == AML_OBJ_STRING ||
                o->type == AML_OBJ_BUFFER  || o->type == AML_OBJ_PACKAGE)
                continue;
            return AmlGateSay(g, at, "a name holding no value this evaluator "
                                     "can read: it is ", AmlTypeName(o->type));
        }

        if (op == ExtOpPrefix) {
            uint8_t     sub = ((size_t)(end - *p) >= 2) ? (*p)[1] : 0;
            const char *nm  = AmlExtOpName(sub);
            return AmlGateSay(g, at, "an extended opcode where a value was "
                                     "expected: ",
                              nm ? nm : "one under 0x5B this pass cannot name");
        }

        {
            const char *nm = AmlOpcodeName(op);
            return AmlGateSay(g, at, "an operand this evaluator does not "
                                     "evaluate: ",
                              nm ? nm : "an opcode this pass cannot name");
        }
    }
    return true;
}

/* One statement of the method being proven. `*done` comes back true when its
 * last block has been walked to the end. */
static bool AmlGateStep(AmlGate *g, bool *done) {
    if (g->blkn == 0) { *done = true; return true; }
    AmlGateBlock *b = &g->blk[g->blkn - 1];
    if (b->p >= b->end) { g->blkn--; return true; }

    uint8_t op = *b->p;

    /* §20.2.5.3 makes both of these nothing at all on a machine with no
     * debugger, and the machine below performs them as nothing — so the walk
     * passes them, or it would refuse a method the run could carry whole. */
    if (op == NoopOp || op == BreakPointOp) { b->p++; return true; }

    if (op == ReturnOp) {
        const uint8_t *q = b->p + 1;
        if (!AmlGateRun(g, &q, b->end, AML_GATE_TERM)) return false;
        b->p = q;
        return true;
    }

    if (op == StoreOp) {
        const uint8_t *q = b->p + 1;
        if (!AmlGateRun(g, &q, b->end, AML_GATE_TERM))   return false;
        if (!AmlGateRun(g, &q, b->end, AML_GATE_TARGET)) return false;
        b->p = q;
        return true;
    }

    if (op == IfOp) {
        /* DefIfElse := IfOp PkgLength Predicate TermList DefElse — §20.2.5.3. */
        const uint8_t *q  = b->p + 1;
        uint32_t       cb = 0;
        uint32_t       pk = decode_pkglen(&q, b->end, &cb);
        if (!cb || pk < cb)
            return AmlGateSay(g, b->p, "an If with a malformed package length",
                              NULL);
        const uint8_t *if_end = q + pk;
        if (if_end > b->end)
            return AmlGateSay(g, b->p, "an If that runs past the end of its "
                                       "block", NULL);
        const uint8_t *body = q + cb;
        if (!AmlGateRun(g, &body, if_end, AML_GATE_TERM)) return false;

        const uint8_t *else_body = NULL, *else_end = NULL;
        const uint8_t *after     = if_end;
        if (if_end < b->end && *if_end == ElseOp) {
            const uint8_t *e   = if_end + 1;
            uint32_t       ecb = 0;
            uint32_t       epk = decode_pkglen(&e, b->end, &ecb);
            if (!ecb || epk < ecb)
                return AmlGateSay(g, if_end, "an Else with a malformed "
                                             "package length", NULL);
            const uint8_t *ee = e + epk;
            if (ee > b->end)
                return AmlGateSay(g, if_end, "an Else that runs past the end "
                                             "of its block", NULL);
            else_body = e + ecb;
            else_end  = ee;
            after     = ee;
        }

        b->p = after;                 /* before the stack below may move it */
        /* ‼ BOTH SIDES, BECAUSE THE PREDICATE IS A VALUE AND THIS WALK HAS
         * NO VALUES. A gate that proved only the branch it guessed would be
         * guessing about the one that runs. */
        if (else_body && else_body < else_end &&
            !AmlGatePush(g, else_body, else_end)) return false;
        if (body < if_end && !AmlGatePush(g, body, if_end)) return false;
        return true;
    }

    if (op == NameOp) {
        /* ‼ A DECLARATION INSIDE A BRANCH IS REFUSED. §20.2.5.2 puts a
         * method's Name under the running method, so whether it exists after
         * the If depends on which side ran — and this walk takes both. Rather
         * than let a later Store find a name that may not be there, a board
         * that declares one this way is left where it is. */
        if (g->blkn > 1)
            return AmlGateSay(g, b->p, "a Name declared inside an If or an "
                                       "Else, so whether it exists afterwards "
                                       "depends on a branch this walk cannot "
                                       "choose between", NULL);
        const uint8_t *q = b->p + 1;
        char seg[4];
        if (!AmlBareSeg(q, b->end, seg))
            return AmlGateSay(g, b->p, "a Name inside a method body that "
                                       "declares a path rather than a single "
                                       "name", NULL);
        q += 4;
        uint32_t buf_len = 0;
        if (!AmlGateData(g, &q, b->end, &buf_len)) return false;
        if (!AmlGateDeclare(g, seg, buf_len))      return false;
        b->p = q;
        return true;
    }

    if (op == CreateBitFieldOp  || op == CreateByteFieldOp ||
        op == CreateWordFieldOp || op == CreateDWordFieldOp ||
        op == CreateQWordFieldOp) {
        /* §20.2.5.2: DefCreateXField := op SourceBuff Index NameString. */
        if (g->blkn > 1)
            return AmlGateSay(g, b->p, "a CreateField inside an If or an "
                                       "Else, so whether the field exists "
                                       "afterwards depends on a branch this "
                                       "walk cannot choose between", NULL);
        /* The index is a BIT index for CreateBitField and a BYTE index for
         * the other four — AmlStmtStep's own arithmetic, repeated here
         * because the two passes have to land on the same bits or this one
         * is proving a cut the other will not make. */
        uint32_t width = (op == CreateBitFieldOp)   ? 1u  :
                         (op == CreateByteFieldOp)  ? 8u  :
                         (op == CreateWordFieldOp)  ? 16u :
                         (op == CreateDWordFieldOp) ? 32u : 64u;
        uint32_t scale = (op == CreateBitFieldOp) ? 1u : 8u;

        const uint8_t *q = b->p + 1;
        char src[4];
        uint32_t buf_len = 0;
        if (!AmlBareSeg(q, b->end, src))
            return AmlGateSay(g, b->p, "a CreateField cutting into something "
                                       "other than a single named object",
                              NULL);
        if (!AmlGateHasName(g, src, &buf_len) || buf_len == 0)
            return AmlGateSay(g, b->p, "a CreateField naming a buffer this "
                                       "method never declared, or one it "
                                       "declared with no bytes in it", NULL);
        q += 4;
        uint64_t index = 0;
        if (!read_const_integer(&q, b->end, &index))
            return AmlGateSay(g, b->p, "a CreateField whose index is not a "
                                       "constant this evaluator can read",
                              NULL);
        /* ‼ AND THE CUT HAS TO FIT, ANSWERED WHERE IT IS WRITTEN RATHER THAN
         * WHERE IT IS USED. AmlBufFieldRead and AmlBufFieldWrite refuse a
         * field that reaches past the end of the buffer it was cut from, and
         * they refuse it at the read or the write — which in a method this
         * gate has passed is the middle of the run, with earlier stores
         * already landed. Every number in the question is in the text, so
         * the answer is taken here instead. A method that declares such a
         * cut and then never touches it is refused although the run would
         * have survived it: that is the same direction this walk already
         * errs in everywhere else. */
        uint64_t bits = (uint64_t)buf_len * 8u;
        if (index > bits || index * scale + width > bits)
            return AmlGateSay(g, b->p, "a CreateField whose cut reaches past "
                                       "the end of the buffer it names", NULL);
        char name[4];
        if (!AmlBareSeg(q, b->end, name))
            return AmlGateSay(g, b->p, "a CreateField naming its field with a "
                                       "path rather than a single name", NULL);
        q += 4;
        /* The view itself holds no bytes of its own — the run gives it an
         * empty value and a window onto the buffer — so nothing may be cut
         * out of it in turn, which is what the zero says. */
        if (!AmlGateDeclare(g, name, 0)) return false;
        b->p = q;
        return true;
    }

    if (op == ElseOp)
        return AmlGateSay(g, b->p, "an Else with no If before it", NULL);

    if (op == ExtOpPrefix) {
        uint8_t     sub = ((size_t)(b->end - b->p) >= 2) ? b->p[1] : 0;
        const char *nm  = AmlExtOpName(sub);
        return AmlGateSay(g, b->p, "a statement that acts on the platform "
                                   "rather than reading it: ",
                          nm ? nm : "an extended opcode under 0x5B this pass "
                                    "cannot name");
    }

    {
        uint8_t nargs = 0, ntargets = 0;
        if (AmlOperatorShape(op, &nargs, &ntargets) ||
            (aml_is_name_string_lead(op) && op != 0x00)) {
            /* An expression standing alone: a method called for its effect,
             * or an operator whose Target is the point of it. */
            const uint8_t *q = b->p;
            if (!AmlGateRun(g, &q, b->end, AML_GATE_TERM)) return false;
            b->p = q;
            return true;
        }
    }

    if (op == WhileOp || op == BreakOp || op == ContinueOp) {
        /* ‼ THE EVALUATOR RUNS THESE AND THIS WALK REFUSES THEM, ON PURPOSE.
         * A loop's turns are decided by values and this walk has none — it
         * takes both sides of every If for exactly that reason. It could walk
         * a body once, but what it promises is not "every opcode is one this
         * evaluator knows", it is "this method will run from beginning to
         * END" — and that is what earns \_PIC its store. A loop that the run
         * would refuse half-way (see AmlLoopTurn) would leave the firmware
         * told half of something, which is the one outcome the narrow gate
         * exists to prevent. No board's \_PIC has a loop in it; one that does
         * is refused whole, before its first store. */
        return AmlGateSay(g, b->p, "a loop, whose number of turns is decided "
                                   "by values this walk does not have: ",
                          op == WhileOp ? "While"
                                        : (op == BreakOp ? "Break"
                                                         : "Continue"));
    }

    {
        const char *nm = AmlOpcodeName(op);
        return AmlGateSay(g, b->p, "a statement this evaluator does not run: ",
                          nm ? nm : "an opcode this pass cannot name");
    }
}

static void AmlGateForget(AmlGate *g) {
    if (g->todo) kfree(g->todo);
    if (g->seen) kfree(g->seen);
    if (g->blk)  kfree(g->blk);
    if (g->obj)  kfree(g->obj);
    if (g->owed) kfree(g->owed);
    memset(g, 0, sizeof(*g));
}

/*
 * Walk `method` and everything it calls. True only if every operation in all
 * of them is one this evaluator performs and every store is one this kernel
 * may carry out; the refusal has already been printed otherwise.
 */
static bool AmlGateProve(aml_object_t *method, const char *asked_for,
                          uint32_t *writes, uint32_t *reads,
                          uint32_t *methods) {
    AmlGate g;
    memset(&g, 0, sizeof(g));
    g.asked_for = asked_for;
    g.now       = method;          /* so a refusal before the first pop names it */

    bool ok = AmlGateOwesMethod(&g, method);
    while (ok && g.todon) {
        aml_object_t *m = g.todo[--g.todon];

        bool again = false;
        for (uint32_t i = 0; i < g.seenn; i++) if (g.seen[i] == m) again = true;
        if (again) continue;

        aml_object_t **grown =
            (aml_object_t **)AmlStackRoom(g.seen, &g.seen_room, g.seenn + 1,
                                          sizeof(aml_object_t *));
        if (!grown) {
            ok = AmlGateNoRoom(&g, "remember which methods it has proven");
            break;
        }
        g.seen = grown;
        /* Remembered BEFORE its body is walked, so a method that calls
         * itself is not queued again and the walk cannot circle. */
        g.seen[g.seenn++] = m;

        g.now  = m;
        g.objn = 0;                /* a method's own objects are its own */
        g.blkn = 0;
        if (!m->v.method.aml || m->v.method.aml_len == 0) {
            kprintf("[AML] %[E]%s cannot be run whole: %s has an empty body, "
                    "so there is nothing to prove and nothing to run%[D]\n",
                    asked_for, m->name);
            ok = false;
            break;
        }
        if (!AmlGatePush(&g, m->v.method.aml,
                         m->v.method.aml + m->v.method.aml_len)) {
            ok = false;
            break;
        }
        g.methods++;

        bool done = false;
        while (!done)
            if (!AmlGateStep(&g, &done)) { ok = false; break; }
    }

    *writes  = g.writes;
    *reads   = g.reads;
    *methods = g.methods;
    AmlGateForget(&g);
    return ok;
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


    for (aml_object_t *n = AmlWalkNext(&g_root_obj, &g_root_obj); n;
         n = AmlWalkNext(n, &g_root_obj)) {
        if (n->type != AML_OBJ_DEVICE) continue;
        /* ACPI 6.5 §6.1.5 / Table 6.6: PNP0A03 is a PCI host bridge and
         * PNP0A08 a PCI Express one. Both carry a _PRT. Read once per
         * device, asked about twice — a Method form is run here and nowhere
         * else in this loop. */
        AmlRun run;
        AmlRunInit(&run);
        AmlDeviceIds ids;
        AmlReadDeviceIds(&run, n, &ids);
        bool is_bridge = AmlIdsName(&ids, "PNP0A03") ||
                         AmlIdsName(&ids, "PNP0A08");
        AmlRunDone(&run);
        if (!is_bridge) continue;

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

    /* The whole namespace has just been asked what each of its devices is.
     * Whatever could not answer is said here, once, with a count — before the
     * "no root bridge" line below, because on a board where that line is the
     * consequence this is the cause. */
    AmlDeviceIdsRefusedSay();

    if (!g_root_bridges) {
        kprintf("[AML] this firmware's namespace holds no PCI root bridge "
                "(PNP0A03 or PNP0A08), so there is no routing table to read\n");
    }

    /* ‼ THE BRIDGES ARE FOUND BEFORE \_PIC IS TOLD, AND \_PIC BEFORE ANY
     * _PRT IS READ. §5.8.1's ordering constraint is against the ROUTING
     * TABLE, and nothing above reads one: a bridge's _HID, _CID, _SEG and
     * _BBN say the same thing in either interrupt mode. Doing the walk first
     * is what lets a board whose \_PIC reaches a PCI configuration register
     * — which is how a chipset's interrupt-mode bit is usually set — find a
     * root bridge to resolve that register against. A _PRT is read lazily,
     * in AmlPrtTableFor, long after this returns.
     *
     * ACPI 6.5 §5.8.1, \_PIC: "The \_PIC optional method is used to report
     * to the platform runtime firmware the current interrupt model used by
     * the OS ... The argument passed into the method signifies the interrupt
     * model OSPM has chosen, PIC mode, APIC mode, or SAPIC mode ... If the
     * platform CPU architecture supports PIC mode and the method is never
     * called, the platform runtime firmware must assume PIC mode."
     *
     * The models are numbered in the order that sentence names them, which
     * is what `model` below carries: 0 PIC, 1 APIC, 2 SAPIC. Only the first
     * two exist on this architecture.
     */
    uint64_t model = (irqchip_get_type() == IRQCHIP_APIC) ? 1u : 0u;
    aml_object_t *pic = AmlChild(&g_root_obj, "_PIC");
    if (!pic) {
        /* ‼ AN ABSENT \_PIC IS A LEGAL BOARD, NOT A FAULT — see the note on
         * AmlPicState. The firmware has one routing table, it is the one it
         * is holding out, and refusing it leaves the line with config byte
         * 0x3C, which is a weaker statement of the same thing. */
        g_pic = AML_PIC_NOT_DECLARED;
        kprintf("[AML] this firmware declares no \\_PIC. ACPI 6.5 §5.8.1 makes "
                "the method optional and puts the consequence on the firmware "
                "rather than on us — never told, it must assume PIC mode — so "
                "the one routing table it is holding out is the PIC-mode one; "
                "on such a platform the 8259's inputs and the first sixteen "
                "IO-APIC inputs are the same lines, so its numbers are read "
                "as global system interrupts and the MADT's source overrides "
                "are what describe any exception\n");
    } else if (pic->type != AML_OBJ_METHOD) {
        g_pic = AML_PIC_UNKNOWN;
        kprintf("[AML] %[E]\\_PIC is %s, and §5.8.1 makes it a control method "
                "— the interrupt model was not announced, so a directly "
                "numbered entry of the table it hands over is not read as a "
                "global system interrupt%[D]\n", AmlTypeName(pic->type));
    } else {
        /* ‼ THE GATE FIRST, AND THE PLATFORM AFTERWARDS. Nothing below this
         * line touches the machine until the walk above has proved that the
         * whole of \_PIC — and of every method it calls — is something this
         * evaluator can carry out from beginning to end. A board it cannot
         * prove is left exactly where it stood before any of this existed. */
        uint32_t owed_writes = 0, owed_reads = 0, owed_methods = 0;
        if (!AmlGateProve(pic, "\\_PIC", &owed_writes, &owed_reads,
                          &owed_methods)) {
            g_pic = AML_PIC_UNKNOWN;
            kprintf("[AML] %[E]\\_PIC was not run at all, so this kernel does "
                    "not know which of the firmware's routing tables it is "
                    "being handed. §5.8.1 makes that assumption the "
                    "firmware's and not ours: a platform runtime firmware "
                    "that was never told must assume PIC mode, and since this "
                    "kernel cannot tell whether it was told, it reads what it "
                    "is handed as the PIC-mode table, whose directly numbered "
                    "entries are 8259 interrupts — those are refused. An "
                    "entry that names an interrupt link is still followed: "
                    "the link's _CRS reads the chipset's routing register, "
                    "which says the same thing in either mode%[D]\n");
        } else {
            kprintf("[AML] \\_PIC: the dry walk took every byte of it and of "
                    "%u method(s) it calls, went down both sides of every If, "
                    "and found every operation one this evaluator performs — "
                    "%u store(s) into memory this board's own map calls ACPI "
                    "NVS, %u read(s) of an address space. It may be run for "
                    "real, with those stores allowed and nothing else\n",
                    owed_methods, owed_writes, owed_reads);

            AmlRun run;
            AmlRunInit(&run);
            /* ‼ THE ONLY ASSIGNMENT TO proven IN THIS FILE, on a run that is
             * a local variable of this function — so the permission it
             * carries ends when this function returns. */
            run.proven = true;
            AmlValue arg;
            memset(&arg, 0, sizeof(arg));
            arg.type    = AML_OBJ_INTEGER;
            arg.integer = model;
            if (AmlEvalIn(&run, pic, &arg, 1, NULL)) {
                g_pic = AML_PIC_TOLD;
                /* ‼ AND THIS IS WHERE THE KERNEL LEARNS WHICH TABLE IT WILL
                 * BE HANDED. Not by reading back the name the firmware
                 * branches on — that name is the board's own and this file
                 * knows none of them — but from §5.8.1's own terms: the
                 * argument names the model, the firmware is required to save
                 * it, and what it describes from here on is the model it was
                 * given. AmlPrtLookup reads AML_PIC_TOLD as exactly that. */
                kprintf("[AML] \\_PIC(%u) ran to its end%s — this kernel runs "
                        "its %s and the firmware has now been told so, before "
                        "a single routing table has been read. What it hands "
                        "over from here is the %s-mode table, so a directly "
                        "numbered _PRT entry in it is a global system "
                        "interrupt and is believed\n",
                        (unsigned)model,
                        run.writes ? "" : " and placed no value anywhere",
                        model ? "IO-APIC" : "8259 pair",
                        model ? "APIC" : "PIC");
            } else {
                g_pic = AML_PIC_UNKNOWN;
                if (run.writes) {
                    /* What the gate never promised, said plainly rather than
                     * reported as a clean failure: whether a register
                     * answers, and what type a value turns out to have, are
                     * facts about the running machine and not about the
                     * method's text. */
                    kprintf("[AML] %[E]\\_PIC was proved runnable and then did "
                            "not finish, after %u value(s) had already been "
                            "placed. The dry walk proves every OPERATION is "
                            "one this evaluator performs; it cannot promise a "
                            "register will answer, nor that every value "
                            "reaching an operation is the integer that "
                            "operation takes — the line above says which of "
                            "those it was. The firmware has been told half of "
                            "something, and which routing table it now holds "
                            "out is not known%[D]\n", run.writes);
                } else {
                    kprintf("[AML] %[E]\\_PIC was proved runnable and then did "
                            "not finish, before any value had been placed — "
                            "so the firmware is exactly as untold as it was, "
                            "and which of its routing tables it is holding out "
                            "is not known. Its directly numbered entries are "
                            "refused; an entry naming an interrupt link is "
                            "still followed%[D]\n");
                }
            }
            AmlRunDone(&run);
        }
    }

}

/*
 * One namespace Device under `parent` whose _ADR names this function.
 *
 * ACPI 6.5 §6.1.1: _ADR for a PCI device is the device number in the high
 * word and the function number in the low word; 0xFFFF in the low word means
 * every function.
 *
 * ‼ _ADR IS AN OBJECT OR A CONTROL METHOD, AND "NOT FOUND" IS NOT THE SAME
 *   FACT AS "NOT DECLARED".
 *
 * What stood here read the node raw — `adr->type != AML_OBJ_INTEGER`, skip —
 * and said nothing. A bridge whose _ADR is a method therefore had no Device
 * as far as this walk was concerned: its own _PRT was never asked, the §9.1
 * rotation answered in its place, and PciSummons stamped the result "the
 * ACPI routing table". A wrong GSI carrying the board's own signature is
 * worse than no answer at all, which is why the two outcomes are now
 * separated. The method form is run; and if any sibling's _ADR cannot be
 * read, "no Device here" becomes UNCERTAIN — said aloud, and the caller
 * refuses instead of rotating past a bridge that may have answered.
 */
static aml_object_t *AmlDeviceByAdr(aml_object_t *parent, uint8_t device,
                                     uint8_t function, bool *uncertain) {
    *uncertain = false;
    if (!parent) return NULL;
    uint64_t want_exact = ((uint64_t)device << 16) | function;
    uint64_t want_any   = ((uint64_t)device << 16) | 0xFFFFu;
    bool unreadable = false;

    for (aml_object_t *c = parent->child; c; c = c->sibling) {
        if (c->type != AML_OBJ_DEVICE) continue;
        aml_object_t *adr = AmlChild(c, "_ADR");
        if (!adr) continue;            /* §6.1.1 leaves _ADR optional */
        AmlRun run;
        AmlRunInit(&run);
        AmlValue v;
        bool ok = AmlEvalIn(&run, adr, NULL, 0, &v) &&
                  v.type == AML_OBJ_INTEGER;
        uint64_t got = ok ? v.integer : 0;
        AmlRunDone(&run);
        if (!ok) {
            kprintf("[AML] device %s under %s declares an _ADR this "
                    "evaluator cannot read as an integer, so whether it is "
                    "the bridge being asked about is not known\n",
                    c->name, parent->name);
            unreadable = true;
            continue;
        }
        if (got == want_exact || got == want_any) return c;
    }

    *uncertain = unreadable;
    return NULL;
}

/* One level of the walk from the root bus down to the function asked about:
 * the namespace Device that owns that bus, and whether "no Device here" is
 * the firmware's silence or this kernel's failure to read one. */
typedef struct AmlBusOwner {
    aml_object_t *node;
    bool          uncertain;
} AmlBusOwner;

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
     * of its own, and the swizzle below is then the answer. `uncertain`
     * keeps those two apart from the third case: a bridge this kernel could
     * not recognise because an _ADR beside it would not read. */
    AmlBusOwner *owner =
        (AmlBusOwner *)kmalloc((size_t)depth * sizeof(*owner));
    if (!owner) {
        kprintf("[AML] the heap has no room to walk %u bridge levels\n", depth);
        return false;
    }
    owner[0].node      = rb->node;
    owner[0].uncertain = false;
    for (uint32_t i = 1; i < depth; i++)
        owner[i].node = AmlDeviceByAdr(owner[i - 1].node, path[i - 1].device,
                                       path[i - 1].function,
                                       &owner[i].uncertain);

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
        /* ‼ An UNCERTAIN level is not a silent one. The bridge at this step
         * may be declared with a _PRT of its own that this kernel could not
         * find, and rotating the pin past it would hand back a number
         * belonging to a different device — under the routing table's name,
         * which is the most trusted source there is. */
        if (!owner[i].node && owner[i].uncertain) {
            kprintf("[AML] segment %u bus %u: the namespace Device for the "
                    "bridge at %u.%u could not be identified, because an "
                    "_ADR beside it would not read — so it may be there with "
                    "a routing table of its own, and rotating the pin past "
                    "it would answer with another device's interrupt. "
                    "Refused\n",
                    (unsigned)segment, (unsigned)root_bus,
                    (unsigned)path[i - 1].device,
                    (unsigned)path[i - 1].function);
            kfree(owner);
            return false;
        }
        AmlPrtTable *t = owner[i].node ? AmlPrtTableFor(owner[i].node) : NULL;
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

    /* Following a table's entry asks interrupt links what they are, so this
     * walk can add to the tally the adoption already reported. Said only if
     * it grew. */
    AmlDeviceIdsRefusedSay();

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
    parse_term_list((uint8_t*)h + hdr, h->length - hdr, &g_root_obj);
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
    g_parse_stmts    = 0;
    g_parse_unwalked = 0;
    g_parse_guessed  = 0;

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

    /* ‼ HOW WIDE AN INTEGER IS ON THIS BOARD, TAKEN FROM THE BOARD.
     * ACPI 6.5 §19.6.29: a DSDT whose ComplianceRevision is less than 2
     * restricts every integer to 32 bits, and one of 2 or above gets full
     * 64-bit integers. The DSDT's own header carries that revision, and the
     * same clause says it "sets the global integer width for all integers,
     * including integers in SSDTs". Getting it wrong makes Ones the wrong number and takes Not,
     * ShiftLeft and every comparison against Ones with it — and the emulator
     * on this desk ships revision 1 while the owner's board ships 2, so it
     * is not a difference that can be left to a default. */
    g_int_bits = (dsdt->revision >= 2) ? 64u : 32u;

    parse_term_list((uint8_t*)dsdt + sizeof(acpi_sdt_header_t),
                    dsdt->length - sizeof(acpi_sdt_header_t),
                    &g_root_obj);

    /* Every SSDT contributes additional namespace. */
    acpi_for_each_table("SSDT", ssdt_cb, NULL);

    /* The parse pass is over. Its operand list is scratch, not namespace. */
    AmlSkipForget();

    g_loaded = true;
    /* kprintf: the object count is the first thing to compare against the
     * same firmware's namespace on another OS when a method cannot be found,
     * and it is how a truncated build is spotted from a photograph of the
     * screen. */
    kprintf("[AML] namespace loaded: %u objects across DSDT + SSDTs%s\n",
            g_pool_used,
            g_pool_exhausted ? " (TRUNCATED — the heap ran out, see above)"
                             : "");
    if (g_parse_stmts)
        kprintf("[AML] %u term(s) at TermList level were statements rather "
                "than declarations — an If or a While guarding a block of "
                "them, a Return, a method called for its effect. Each was "
                "measured exactly rather than resynchronised through, and the "
                "body of every If, Else and While among them was walked for "
                "what it declares — both sides, because which side a board "
                "takes is decided by a value this pass has no frame to "
                "compute — except %u of them, whose body this pass did not "
                "enter because where it begins was not known\n",
                g_parse_stmts, g_parse_unwalked);
    /* ‼ THE EXCEPTION, WITH WHAT IT COSTS. A body this pass did not enter
     * holds declarations that are missing from this namespace, and a method
     * naming one of them will not find it. */
    if (g_parse_guessed)
        kprintf("[AML] %[E]%u term(s) named something this pass had not seen "
                "declared, so how far they reach was an assumption; wherever "
                "that assumption decided where a BODY begins, the body was "
                "stepped over unread rather than walked from a byte that may "
                "stand in the middle of a predicate. Whatever those bodies "
                "declare is missing from this namespace%[D]\n",
                g_parse_guessed);
    /* Announced on the tag bus with the object count as payload. The reader
     * that binds to this namespace is AmlRouteForPciPin below, and it asks
     * its question later — the first time a driver has a PCI interrupt line
     * to arm — because only by then has the machine chosen the interrupt
     * model \_PIC must be told about. */
    uint32_t cnt = g_pool_used;
    TouchPublish("aml:ready", &cnt, sizeof(cnt));
    return AML_OK;
}
