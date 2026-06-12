// boxcxx — shared internals of the DWARF unwinder (Level 1).
#ifndef BOXCXX_UNWIND_INTERNAL_H
#define BOXCXX_UNWIND_INTERNAL_H

#include <cstdint>
#include <cstddef>

namespace boxcxx {

[[noreturn]] void Panic(const char *msg);

namespace unwind {

inline constexpr int kRegRsp   = 7;
inline constexpr int kRegRa    = 16;   // DWARF return-address column
inline constexpr int kRegCount = 17;

// Matches unwind_context.asm.
struct UnwRegisterFile {
    uint64_t regs[kRegCount];
};

extern "C" {
void UnwCaptureContext(UnwRegisterFile *out);
[[noreturn]] void UnwRestoreContext(const UnwRegisterFile *ctx);
}

// ── parsed CIE ──────────────────────────────────────────────────────────

struct DwarfCie {
    const uint8_t *initial_begin;
    const uint8_t *initial_end;
    uint64_t       code_align;
    int64_t        data_align;
    uint8_t        ra_reg;
    uint8_t        fde_enc;       // DW_EH_PE encoding for FDE pc fields
    uint8_t        lsda_enc;      // DW_EH_PE_omit if absent
    bool           has_z;         // FDEs carry an augmentation-length field
    void          *personality;   // nullptr if absent
};

// ── register recovery rules ─────────────────────────────────────────────

enum class RegRuleKind : uint8_t {
    Unset,        // callee keeps caller's value (untouched)
    Undefined,
    SameValue,
    Offset,       // *(CFA + value)
    ValOffset,    //  (CFA + value)
    Register,     //  old regs[value]
    Expression,   // *(eval(expr))
    ValExpression //  eval(expr)
};

struct RegRule {
    RegRuleKind    kind = RegRuleKind::Unset;
    int64_t        value = 0;
    const uint8_t *expr = nullptr;   // points at ULEB length prefix
};

struct CfaState {
    bool           cfa_is_expr = false;
    uint8_t        cfa_reg     = 0;
    int64_t        cfa_off     = 0;
    const uint8_t *cfa_expr    = nullptr;
    uint64_t       args_size   = 0;     // DW_CFA_GNU_args_size latch
    RegRule        rules[kRegCount];
};

// ── per-frame decode result ─────────────────────────────────────────────

struct FrameInfo {
    DwarfCie cie;
    uint64_t pc_begin = 0;
    uint64_t pc_end   = 0;
    uint64_t lsda     = 0;
    CfaState state;          // rules active at the frame's current IP
};

// dwarf_cfi.cpp
bool DwarfFindFrame(uint64_t pc, FrameInfo *out);
// Applies `state` to `file`: computes CFA, recovers registers, sets
// regs[rsp]=CFA and regs[ra→rip]. Returns false at end-of-stack
// (no RA rule / RA==0). cfa_out receives the computed CFA.
bool DwarfStep(const FrameInfo &frame, UnwRegisterFile *file,
               uint64_t *cfa_out);

} // namespace unwind
} // namespace boxcxx

#endif // BOXCXX_UNWIND_INTERNAL_H
