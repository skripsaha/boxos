#ifndef BOXCXX_UNWIND_INTERNAL_H
#define BOXCXX_UNWIND_INTERNAL_H

#include <cstdint>
#include <cstddef>

namespace boxcxx {

[[noreturn]] void Panic(const char *msg);

namespace unwind {

inline constexpr int kRegRsp   = 7;
inline constexpr int kRegRa    = 16;
inline constexpr int kRegCount = 17;

struct UnwRegisterFile {
    uint64_t regs[kRegCount];
};

extern "C" {
void UnwCaptureContext(UnwRegisterFile *out);
[[noreturn]] void UnwRestoreContext(const UnwRegisterFile *ctx);
}


struct DwarfCie {
    const uint8_t *initial_begin;
    const uint8_t *initial_end;
    uint64_t       code_align;
    int64_t        data_align;
    uint8_t        ra_reg;
    uint8_t        fde_enc;
    uint8_t        lsda_enc;
    bool           has_z;
    void          *personality;
};


enum class RegRuleKind : uint8_t {
    Unset,
    Undefined,
    SameValue,
    Offset,
    ValOffset,
    Register,
    Expression,
    ValExpression
};

struct RegRule {
    RegRuleKind    kind = RegRuleKind::Unset;
    int64_t        value = 0;
    const uint8_t *expr = nullptr;
};

struct CfaState {
    bool           cfa_is_expr = false;
    uint8_t        cfa_reg     = 0;
    int64_t        cfa_off     = 0;
    const uint8_t *cfa_expr    = nullptr;
    uint64_t       args_size   = 0;
    RegRule        rules[kRegCount];
};


struct FrameInfo {
    DwarfCie cie;
    uint64_t pc_begin = 0;
    uint64_t pc_end   = 0;
    uint64_t lsda     = 0;
    CfaState state;
};

bool DwarfFindFrame(uint64_t pc, FrameInfo *out);
bool DwarfStep(const FrameInfo &frame, UnwRegisterFile *file,
               uint64_t *cfa_out);

}
}

#endif