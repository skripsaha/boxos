/*
 * dwarf_cfi.cpp — .eh_frame CIE/FDE parser, DW_CFA interpreter and the
 * DW_OP expression subset GCC emits for x86-64 frames.
 *
 * Coverage is the COMPLETE instruction set produced by GCC 15 for
 * -fasynchronous-unwind-tables code (incl. remember/restore_state,
 * def_cfa_expression and GNU_args_size). Anything outside that set
 * panics with the opcode value — loudly diagnosable, never silently
 * wrong.
 *
 * The FDE index is built lazily on the first lookup from the linker
 * bounds __eh_frame_start/__eh_frame_end (user.ld). Single thread per
 * cabin — no synchronization required.
 */

#include "unwind_internal.h"

extern "C" {
void *malloc(size_t size);
void  free(void *ptr);
extern const uint8_t __eh_frame_start[];
extern const uint8_t __eh_frame_end[];
}

namespace boxcxx::unwind {

namespace {

// ── primitive readers ───────────────────────────────────────────────────

uint64_t ReadUleb(const uint8_t **p)
{
    uint64_t result = 0;
    int      shift  = 0;
    uint8_t  byte;
    do {
        byte = *(*p)++;
        result |= uint64_t(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return result;
}

int64_t ReadSleb(const uint8_t **p)
{
    int64_t result = 0;
    int     shift  = 0;
    uint8_t byte;
    do {
        byte = *(*p)++;
        result |= int64_t(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) result |= -(int64_t(1) << shift);
    return result;
}

template <class T>
T ReadFixed(const uint8_t **p)
{
    T value;
    __builtin_memcpy(&value, *p, sizeof(T));
    *p += sizeof(T);
    return value;
}

// ── DW_EH_PE pointer decoding ───────────────────────────────────────────

constexpr uint8_t kPeOmit     = 0xFF;
constexpr uint8_t kPeUleb     = 0x01;
constexpr uint8_t kPeUdata2   = 0x02;
constexpr uint8_t kPeUdata4   = 0x03;
constexpr uint8_t kPeUdata8   = 0x04;
constexpr uint8_t kPeSleb     = 0x09;
constexpr uint8_t kPeSdata2   = 0x0A;
constexpr uint8_t kPeSdata4   = 0x0B;
constexpr uint8_t kPeSdata8   = 0x0C;
constexpr uint8_t kPePcrel    = 0x10;
constexpr uint8_t kPeIndirect = 0x80;

uint64_t ReadEncoded(const uint8_t **p, uint8_t enc)
{
    if (enc == kPeOmit) return 0;

    const uint8_t *field = *p;
    uint64_t value;
    switch (enc & 0x0F) {
    case 0x00: value = ReadFixed<uint64_t>(p); break;      // absptr
    case kPeUleb:   value = ReadUleb(p); break;
    case kPeUdata2: value = ReadFixed<uint16_t>(p); break;
    case kPeUdata4: value = ReadFixed<uint32_t>(p); break;
    case kPeUdata8: value = ReadFixed<uint64_t>(p); break;
    case kPeSleb:   value = uint64_t(ReadSleb(p)); break;
    case kPeSdata2: value = uint64_t(int64_t(ReadFixed<int16_t>(p))); break;
    case kPeSdata4: value = uint64_t(int64_t(ReadFixed<int32_t>(p))); break;
    case kPeSdata8: value = uint64_t(ReadFixed<int64_t>(p)); break;
    default:
        Panic("dwarf: unsupported DW_EH_PE value format");
    }

    switch (enc & 0x70) {
    case 0x00: break;                                       // abs
    case kPePcrel: value += uint64_t(uintptr_t(field)); break;
    default:
        Panic("dwarf: unsupported DW_EH_PE application");
    }

    if (enc & kPeIndirect) value = *reinterpret_cast<const uint64_t *>(value);
    return value;
}

// ── CIE parsing (with a tiny last-used cache: CIEs are shared) ─────────

// Per-strand (thread_local): this 1-entry CIE cache is mutable, and concurrent
// unwinds from sibling strands (std::thread bodies throwing at once) would tear
// it — a half-updated cached CIE hands back a wild pointer and faults. Per-strand
// each unwind uses its own cache; zero-init (nullptr) is a clean miss on first use.
thread_local const uint8_t *g_cached_cie_ptr = nullptr;
thread_local DwarfCie       g_cached_cie;

void ParseCie(const uint8_t *cie_start, DwarfCie *out)
{
    if (cie_start == g_cached_cie_ptr) {
        *out = g_cached_cie;
        return;
    }

    const uint8_t *p = cie_start;
    uint32_t length = ReadFixed<uint32_t>(&p);
    if (length == 0xFFFFFFFFu)
        Panic("dwarf: 64-bit DWARF length in CIE (unsupported)");
    const uint8_t *end = p + length;

    uint32_t id = ReadFixed<uint32_t>(&p);
    if (id != 0) Panic("dwarf: FDE where CIE expected");

    uint8_t version = *p++;
    if (version != 1 && version != 3 && version != 4)
        Panic("dwarf: unsupported CIE version");

    const char *aug = reinterpret_cast<const char *>(p);
    while (*p) p++;
    p++;   // NUL

    if (aug[0] == 'e' && aug[1] == 'h')
        Panic("dwarf: legacy \"eh\" augmentation");

    if (version == 4) {
        uint8_t address_size = *p++;
        uint8_t segment_size = *p++;
        if (address_size != 8 || segment_size != 0)
            Panic("dwarf: CIEv4 address/segment size");
    }

    DwarfCie cie;
    cie.code_align  = ReadUleb(&p);
    cie.data_align  = ReadSleb(&p);
    cie.ra_reg      = (version == 1) ? *p++ : uint8_t(ReadUleb(&p));
    cie.fde_enc     = 0x00;    // absptr default
    cie.lsda_enc    = kPeOmit;
    cie.has_z       = (aug[0] == 'z');
    cie.personality = nullptr;

    if (aug[0] == 'z') {
        uint64_t aug_len = ReadUleb(&p);
        const uint8_t *aug_end = p + aug_len;
        for (const char *a = aug + 1; *a; ++a) {
            switch (*a) {
            case 'R': cie.fde_enc = *p++; break;
            case 'L': cie.lsda_enc = *p++; break;
            case 'P': {
                uint8_t enc = *p++;
                cie.personality =
                    reinterpret_cast<void *>(ReadEncoded(&p, enc));
                break;
            }
            case 'S': break;   // signal frame — no special handling needed
            default:
                Panic("dwarf: unknown augmentation letter");
            }
        }
        p = aug_end;
    } else if (aug[0] != '\0') {
        Panic("dwarf: non-z augmentation");
    }

    cie.initial_begin = p;
    cie.initial_end   = end;

    g_cached_cie_ptr = cie_start;
    g_cached_cie     = cie;
    *out             = cie;
}

// ── lazy sorted FDE index ───────────────────────────────────────────────

struct FdeIndexEntry {
    uint64_t       pc_begin;
    uint64_t       pc_end;
    const uint8_t *fde;        // points at the FDE length field
    const uint8_t *cie;        // resolved CIE pointer
};

FdeIndexEntry *g_index       = nullptr;
size_t         g_index_count = 0;
// Build-once guard for concurrent unwinds. g_index is mutated only during
// BuildIndex; once built it is read-only, so concurrent IndexLookup binary
// searches are safe. State: 0=unbuilt, 1=building, 2=built. First strand to
// CAS 0->1 builds; others spin until 2. (A plain bool let two strands' first
// throws build concurrently -> torn g_index -> wild pointer / crash.)
int            g_index_state = 0;   // accessed via __atomic_* only

void IndexAppend(const FdeIndexEntry &entry, size_t *cap)
{
    if (g_index_count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 64;
        auto *grown = static_cast<FdeIndexEntry *>(
            malloc(new_cap * sizeof(FdeIndexEntry)));
        if (!grown) Panic("dwarf: FDE index allocation failed");
        for (size_t i = 0; i < g_index_count; ++i) grown[i] = g_index[i];
        free(g_index);
        g_index = grown;
        *cap    = new_cap;
    }
    g_index[g_index_count++] = entry;
}

void BuildIndex()
{
    size_t cap = 0;

    const uint8_t *p   = __eh_frame_start;
    const uint8_t *end = __eh_frame_end;

    while (p + 4 <= end) {
        const uint8_t *entry_start = p;
        uint32_t length = ReadFixed<uint32_t>(&p);
        if (length == 0) break;                    // terminator
        if (length == 0xFFFFFFFFu)
            Panic("dwarf: 64-bit DWARF length in .eh_frame");
        const uint8_t *entry_end = p + length;
        if (entry_end > end) Panic("dwarf: .eh_frame entry overruns bounds");

        uint32_t cie_pointer = ReadFixed<uint32_t>(&p);
        if (cie_pointer == 0) {                    // CIE — skip
            p = entry_end;
            continue;
        }

        // FDE: cie_pointer is a self-relative back-distance.
        const uint8_t *cie = p - 4 - cie_pointer + 0;
        DwarfCie parsed;
        ParseCie(cie, &parsed);

        uint64_t pc_begin = ReadEncoded(&p, parsed.fde_enc);
        // pc_range uses the VALUE format of fde_enc without pcrel.
        uint64_t pc_range = ReadEncoded(&p, parsed.fde_enc & 0x0F);

        FdeIndexEntry e;
        e.pc_begin = pc_begin;
        e.pc_end   = pc_begin + pc_range;
        e.fde      = entry_start;
        e.cie      = cie;
        IndexAppend(e, &cap);

        p = entry_end;
    }

    // Insertion sort — link order is already ascending in practice,
    // making this O(n); stays correct if a linker ever reorders.
    for (size_t i = 1; i < g_index_count; ++i) {
        FdeIndexEntry key = g_index[i];
        size_t j = i;
        while (j > 0 && g_index[j - 1].pc_begin > key.pc_begin) {
            g_index[j] = g_index[j - 1];
            --j;
        }
        g_index[j] = key;
    }
}

// Build the FDE index exactly once, safe across concurrent unwinds: the winner
// of the 0->1 CAS builds; everyone else spins until it publishes (state 2). The
// built index is read-only thereafter, so the binary search below races no one.
void EnsureIndexBuilt()
{
    if (__atomic_load_n(&g_index_state, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_index_state, &expected, 1,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        BuildIndex();
        __atomic_store_n(&g_index_state, 2, __ATOMIC_RELEASE);   // publish
    } else {
        while (__atomic_load_n(&g_index_state, __ATOMIC_ACQUIRE) != 2)
            __asm__ __volatile__("pause");
    }
}

const FdeIndexEntry *IndexLookup(uint64_t pc)
{
    EnsureIndexBuilt();
    size_t lo = 0;
    size_t hi = g_index_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pc < g_index[mid].pc_begin)
            hi = mid;
        else if (pc >= g_index[mid].pc_end)
            lo = mid + 1;
        else
            return &g_index[mid];
    }
    return nullptr;
}

// ── DW_OP expression evaluation (GCC x86-64 subset) ────────────────────

uint64_t EvalExpression(const uint8_t *expr, const UnwRegisterFile *file,
                        uint64_t cfa, bool cfa_valid)
{
    const uint8_t *p   = expr;
    uint64_t       len = ReadUleb(&p);
    const uint8_t *end = p + len;

    uint64_t stack[32];
    int      depth = 0;
    auto Push = [&](uint64_t v) {
        if (depth == 32) Panic("dwarf-expr: stack overflow");
        stack[depth++] = v;
    };
    auto Pop = [&]() -> uint64_t {
        if (depth == 0) Panic("dwarf-expr: stack underflow");
        return stack[--depth];
    };

    while (p < end) {
        uint8_t op = *p++;
        if (op >= 0x30 && op <= 0x4F) {            // DW_OP_lit0..31
            Push(op - 0x30);
        } else if (op >= 0x70 && op <= 0x8F) {     // DW_OP_breg0..31
            int reg = op - 0x70;
            if (reg >= kRegCount) Panic("dwarf-expr: breg out of range");
            Push(file->regs[reg] + uint64_t(ReadSleb(&p)));
        } else {
            switch (op) {
            case 0x03: Push(ReadFixed<uint64_t>(&p)); break;   // addr
            case 0x06:                                          // deref
                Push(*reinterpret_cast<const uint64_t *>(Pop()));
                break;
            case 0x08: Push(ReadFixed<uint8_t>(&p)); break;    // const1u
            case 0x0A: Push(ReadFixed<uint16_t>(&p)); break;   // const2u
            case 0x0C: Push(ReadFixed<uint32_t>(&p)); break;   // const4u
            case 0x0E: Push(ReadFixed<uint64_t>(&p)); break;   // const8u
            case 0x10: Push(ReadUleb(&p)); break;              // constu
            case 0x11: Push(uint64_t(ReadSleb(&p))); break;    // consts
            case 0x12: { uint64_t v = Pop(); Push(v); Push(v); break; } // dup
            case 0x13: Pop(); break;                           // drop
            case 0x16: {                                       // swap
                uint64_t a = Pop(), b = Pop();
                Push(a);
                Push(b);
                break;
            }
            case 0x1A: { uint64_t a = Pop(); Push(Pop() & a); break; } // and
            case 0x1C: { uint64_t a = Pop(); Push(Pop() - a); break; } // minus
            case 0x1E: { uint64_t a = Pop(); Push(Pop() * a); break; } // mul
            case 0x21: { uint64_t a = Pop(); Push(Pop() | a); break; } // or
            case 0x22: { uint64_t a = Pop(); Push(Pop() + a); break; } // plus
            case 0x23: Push(Pop() + ReadUleb(&p)); break; // plus_uconst
            case 0x24: { uint64_t a = Pop(); Push(Pop() << a); break; } // shl
            case 0x25: { uint64_t a = Pop(); Push(Pop() >> a); break; } // shr
            case 0x9C:                                          // call_frame_cfa
                if (!cfa_valid) Panic("dwarf-expr: CFA referenced in CFA rule");
                Push(cfa);
                break;
            default:
                Panic("dwarf-expr: unsupported DW_OP");
            }
        }
    }
    return Pop();
}

// ── DW_CFA interpreter ──────────────────────────────────────────────────

constexpr int kStateStackDepth = 8;

void RunInstructions(const DwarfCie &cie, const uint8_t *p,
                     const uint8_t *end, uint64_t pc_begin,
                     uint64_t target_pc, CfaState *state,
                     const CfaState *initial)
{
    uint64_t loc = pc_begin;

    CfaState saved[kStateStackDepth];
    int      saved_depth = 0;

    while (p < end && loc <= target_pc) {
        uint8_t op = *p++;
        uint8_t primary = op & 0xC0;

        if (primary == 0x40) {                       // advance_loc
            loc += uint64_t(op & 0x3F) * cie.code_align;
        } else if (primary == 0x80) {                // offset
            uint8_t reg = op & 0x3F;
            uint64_t off = ReadUleb(&p);
            if (reg < kRegCount) {
                state->rules[reg].kind  = RegRuleKind::Offset;
                state->rules[reg].value = int64_t(off) * cie.data_align;
            }
        } else if (primary == 0xC0) {                // restore
            uint8_t reg = op & 0x3F;
            if (reg < kRegCount && initial)
                state->rules[reg] = initial->rules[reg];
        } else {
            switch (op) {
            case 0x00: break;                        // nop
            case 0x01:                               // set_loc
                loc = ReadEncoded(&p, cie.fde_enc);
                break;
            case 0x02: loc += uint64_t(ReadFixed<uint8_t>(&p)) * cie.code_align; break;
            case 0x03: loc += uint64_t(ReadFixed<uint16_t>(&p)) * cie.code_align; break;
            case 0x04: loc += uint64_t(ReadFixed<uint32_t>(&p)) * cie.code_align; break;
            case 0x05: {                             // offset_extended
                uint64_t reg = ReadUleb(&p);
                uint64_t off = ReadUleb(&p);
                if (reg < kRegCount) {
                    state->rules[reg].kind  = RegRuleKind::Offset;
                    state->rules[reg].value = int64_t(off) * cie.data_align;
                }
                break;
            }
            case 0x06: {                             // restore_extended
                uint64_t reg = ReadUleb(&p);
                if (reg < kRegCount && initial)
                    state->rules[reg] = initial->rules[reg];
                break;
            }
            case 0x07: {                             // undefined
                uint64_t reg = ReadUleb(&p);
                if (reg < kRegCount)
                    state->rules[reg].kind = RegRuleKind::Undefined;
                break;
            }
            case 0x08: {                             // same_value
                uint64_t reg = ReadUleb(&p);
                if (reg < kRegCount)
                    state->rules[reg].kind = RegRuleKind::SameValue;
                break;
            }
            case 0x09: {                             // register
                uint64_t r1 = ReadUleb(&p);
                uint64_t r2 = ReadUleb(&p);
                if (r1 < kRegCount) {
                    state->rules[r1].kind  = RegRuleKind::Register;
                    state->rules[r1].value = int64_t(r2);
                }
                break;
            }
            case 0x0A:                               // remember_state
                if (saved_depth == kStateStackDepth)
                    Panic("dwarf: remember_state overflow");
                saved[saved_depth++] = *state;
                break;
            case 0x0B:                               // restore_state
                if (saved_depth == 0)
                    Panic("dwarf: restore_state underflow");
                *state = saved[--saved_depth];
                break;
            case 0x0C:                               // def_cfa
                state->cfa_is_expr = false;
                state->cfa_reg     = uint8_t(ReadUleb(&p));
                state->cfa_off     = int64_t(ReadUleb(&p));
                break;
            case 0x0D:                               // def_cfa_register
                state->cfa_is_expr = false;
                state->cfa_reg     = uint8_t(ReadUleb(&p));
                break;
            case 0x0E:                               // def_cfa_offset
                state->cfa_off = int64_t(ReadUleb(&p));
                break;
            case 0x0F: {                             // def_cfa_expression
                state->cfa_is_expr = true;
                state->cfa_expr    = p;              // points at length prefix
                uint64_t len = ReadUleb(&p);
                p += len;
                break;
            }
            case 0x10: {                             // expression
                uint64_t reg = ReadUleb(&p);
                const uint8_t *expr = p;
                uint64_t len = ReadUleb(&p);
                p += len;
                if (reg < kRegCount) {
                    state->rules[reg].kind = RegRuleKind::Expression;
                    state->rules[reg].expr = expr;
                }
                break;
            }
            case 0x11: {                             // offset_extended_sf
                uint64_t reg = ReadUleb(&p);
                int64_t  off = ReadSleb(&p);
                if (reg < kRegCount) {
                    state->rules[reg].kind  = RegRuleKind::Offset;
                    state->rules[reg].value = off * cie.data_align;
                }
                break;
            }
            case 0x12:                               // def_cfa_sf
                state->cfa_is_expr = false;
                state->cfa_reg     = uint8_t(ReadUleb(&p));
                state->cfa_off     = ReadSleb(&p) * cie.data_align;
                break;
            case 0x13:                               // def_cfa_offset_sf
                state->cfa_off = ReadSleb(&p) * cie.data_align;
                break;
            case 0x14: {                             // val_offset
                uint64_t reg = ReadUleb(&p);
                uint64_t off = ReadUleb(&p);
                if (reg < kRegCount) {
                    state->rules[reg].kind  = RegRuleKind::ValOffset;
                    state->rules[reg].value = int64_t(off) * cie.data_align;
                }
                break;
            }
            case 0x16: {                             // val_expression
                uint64_t reg = ReadUleb(&p);
                const uint8_t *expr = p;
                uint64_t len = ReadUleb(&p);
                p += len;
                if (reg < kRegCount) {
                    state->rules[reg].kind = RegRuleKind::ValExpression;
                    state->rules[reg].expr = expr;
                }
                break;
            }
            case 0x2E:                               // GNU_args_size
                state->args_size = ReadUleb(&p);
                break;
            default:
                Panic("dwarf: unsupported DW_CFA opcode");
            }
        }
    }
}

} // namespace

// ── public engine entry points ──────────────────────────────────────────

bool DwarfFindFrame(uint64_t pc, FrameInfo *out)
{
    const FdeIndexEntry *entry = IndexLookup(pc);
    if (!entry) return false;

    ParseCie(entry->cie, &out->cie);
    out->pc_begin = entry->pc_begin;
    out->pc_end   = entry->pc_end;

    // Re-walk the FDE header to position at LSDA + instructions.
    const uint8_t *p = entry->fde;
    uint32_t length = ReadFixed<uint32_t>(&p);
    const uint8_t *fde_end = p + length;
    p += 4;                                    // cie_pointer
    (void)ReadEncoded(&p, out->cie.fde_enc);   // pc_begin
    (void)ReadEncoded(&p, out->cie.fde_enc & 0x0F);

    out->lsda = 0;
    const uint8_t *instr = p;
    if (out->cie.has_z) {
        // 'z' augmentation → length-prefixed FDE augmentation data
        // (the LSDA pointer when the CIE declared 'L').
        uint64_t aug_len = ReadUleb(&p);
        const uint8_t *aug_end = p + aug_len;
        if (out->cie.lsda_enc != kPeOmit)
            out->lsda = ReadEncoded(&p, out->cie.lsda_enc);
        instr = aug_end;
    }

    // Default state + CIE initial instructions, then FDE rows up to pc.
    out->state = CfaState{};
    RunInstructions(out->cie, out->cie.initial_begin, out->cie.initial_end,
                    out->pc_begin, ~uint64_t(0), &out->state, nullptr);
    CfaState initial = out->state;
    RunInstructions(out->cie, instr, fde_end, out->pc_begin, pc, &out->state,
                    &initial);
    return true;
}

bool DwarfStep(const FrameInfo &frame, UnwRegisterFile *file,
               uint64_t *cfa_out)
{
    const CfaState &st = frame.state;

    uint64_t cfa;
    if (st.cfa_is_expr) {
        cfa = EvalExpression(st.cfa_expr, file, 0, false);
    } else {
        if (st.cfa_reg >= kRegCount) Panic("dwarf: CFA register out of range");
        cfa = file->regs[st.cfa_reg] + uint64_t(st.cfa_off);
    }
    *cfa_out = cfa;

    UnwRegisterFile next = *file;
    for (int reg = 0; reg < kRegCount; ++reg) {
        const RegRule &rule = st.rules[reg];
        switch (rule.kind) {
        case RegRuleKind::Unset:
        case RegRuleKind::SameValue:
            break;   // keep current value
        case RegRuleKind::Undefined:
            if (reg == frame.cie.ra_reg) {
                return false;   // outermost frame
            }
            break;
        case RegRuleKind::Offset:
            next.regs[reg] = *reinterpret_cast<const uint64_t *>(
                cfa + uint64_t(rule.value));
            break;
        case RegRuleKind::ValOffset:
            next.regs[reg] = cfa + uint64_t(rule.value);
            break;
        case RegRuleKind::Register:
            if (rule.value < 0 || rule.value >= kRegCount)
                Panic("dwarf: register-rule source out of range");
            next.regs[reg] = file->regs[rule.value];
            break;
        case RegRuleKind::Expression:
            next.regs[reg] = *reinterpret_cast<const uint64_t *>(
                EvalExpression(rule.expr, file, cfa, true));
            break;
        case RegRuleKind::ValExpression:
            next.regs[reg] = EvalExpression(rule.expr, file, cfa, true);
            break;
        }
    }

    // x86-64 convention: the caller's RSP at the call site IS the CFA.
    next.regs[kRegRsp] = cfa;

    uint64_t ra = next.regs[frame.cie.ra_reg];
    if (ra == 0) return false;
    next.regs[kRegRa] = ra;   // ra_reg is 16 on x86-64, but stay explicit

    *file = next;
    return true;
}

} // namespace boxcxx::unwind
