/*
 * unwind_level1.cpp — Itanium C++ ABI Level 1 over the boxcxx DWARF
 * engine (dwarf_cfi.cpp).
 *
 * Two-phase model ([abi.eh] §1.3): phase 1 walks the stack calling each
 * frame's personality with _UA_SEARCH_PHASE until one claims the
 * exception (its CFA is cached in exc->private_1); phase 2 re-walks with
 * _UA_CLEANUP_PHASE, running cleanups, and installs the handler context
 * when the personality answers _URC_INSTALL_CONTEXT.
 *
 * exc->private_2 accumulates the number of frames skipped in phase 2 —
 * Phase 4B's CET integration consumes it for INCSSPQ before the final
 * UnwRestoreContext transfer.
 */

#include "unwind_internal.h"

#include <unwind.h>

namespace unw = boxcxx::unwind;

namespace {

// The concrete _Unwind_Context: register file + decoded frame info.
struct UnwindContext {
    unw::UnwRegisterFile file;
    unw::FrameInfo       frame;
    bool                 frame_valid = false;
};

UnwindContext *FromAbi(_Unwind_Context *ctx)
{
    return reinterpret_cast<UnwindContext *>(ctx);
}
_Unwind_Context *ToAbi(UnwindContext *ctx)
{
    return reinterpret_cast<_Unwind_Context *>(ctx);
}

// Decode the frame containing the context's current IP. The lookup key
// is IP-1: the saved RA points AFTER the call, which may be the first
// byte of the next function's FDE.
bool DecodeFrame(UnwindContext *ctx)
{
    ctx->frame_valid =
        unw::DwarfFindFrame(ctx->file.regs[unw::kRegRa] - 1, &ctx->frame);
    return ctx->frame_valid;
}

// Step to the caller. Returns false at end of stack.
bool StepFrame(UnwindContext *ctx, uint64_t *cfa_out)
{
    return unw::DwarfStep(ctx->frame, &ctx->file, cfa_out);
}

[[noreturn]] void InstallContext(UnwindContext *ctx, uint64_t target_ra)
{
    // Resuming at a call site that pushed outgoing stack arguments:
    // the normal return path would pop them with `add rsp, N`; the
    // landing-pad path must do it here (DW_CFA_GNU_args_size).
    ctx->file.regs[unw::kRegRsp] += ctx->frame.state.args_size;

    // CET shadow-stack reconciliation. The handler frame's own shadow
    // entry holds its ORIGINAL return address (target_ra, captured
    // before the personality overwrote the context IP) — pop entries
    // until it surfaces. Shadow-stack pages are readable by plain
    // loads; rdsspq is a NOP (leaves 0) when shadow stacks are off.
    uint64_t ssp = 0;
    __asm__ volatile("xor %0, %0\n\trdsspq %0" : "=r"(ssp));
    if (ssp != 0) {
        uint64_t guard = uint64_t(1) << 16;
        while (*reinterpret_cast<const uint64_t *>(ssp) != target_ra) {
            __asm__ volatile("incsspq %0" :: "r"(uint64_t(1)));
            ssp += 8;
            if (--guard == 0)
                boxcxx::Panic("CET unwind: handler RA not found on "
                              "shadow stack");
        }
    }

    unw::UnwRestoreContext(&ctx->file);
}

} // namespace

// ── context accessors ───────────────────────────────────────────────────

extern "C" _Unwind_Word _Unwind_GetGR(_Unwind_Context *ctx, int reg)
{
    if (reg < 0 || reg >= unw::kRegCount)
        boxcxx::Panic("_Unwind_GetGR: register out of range");
    return FromAbi(ctx)->file.regs[reg];
}

extern "C" void _Unwind_SetGR(_Unwind_Context *ctx, int reg,
                              _Unwind_Word value)
{
    if (reg < 0 || reg >= unw::kRegCount)
        boxcxx::Panic("_Unwind_SetGR: register out of range");
    FromAbi(ctx)->file.regs[reg] = value;
}

extern "C" _Unwind_Word _Unwind_GetIP(_Unwind_Context *ctx)
{
    return FromAbi(ctx)->file.regs[unw::kRegRa];
}

extern "C" _Unwind_Word _Unwind_GetIPInfo(_Unwind_Context *ctx,
                                          int *ip_before_insn)
{
    if (ip_before_insn) *ip_before_insn = 0;
    return _Unwind_GetIP(ctx);
}

extern "C" void _Unwind_SetIP(_Unwind_Context *ctx, _Unwind_Word value)
{
    FromAbi(ctx)->file.regs[unw::kRegRa] = value;
}

extern "C" _Unwind_Word _Unwind_GetCFA(_Unwind_Context *ctx)
{
    // CFA of the current frame = where RSP will point after stepping out.
    UnwindContext probe = *FromAbi(ctx);
    uint64_t cfa = 0;
    if (probe.frame_valid) (void)unw::DwarfStep(probe.frame, &probe.file, &cfa);
    return cfa;
}

extern "C" _Unwind_Word
_Unwind_GetLanguageSpecificData(_Unwind_Context *ctx)
{
    return FromAbi(ctx)->frame.lsda;
}

extern "C" _Unwind_Word _Unwind_GetRegionStart(_Unwind_Context *ctx)
{
    return FromAbi(ctx)->frame.pc_begin;
}

// ── raise / resume ──────────────────────────────────────────────────────

// Phase-2 walker shared by RaiseException and Resume.
static _Unwind_Reason_Code
_Unwind_RaiseException_Phase2(_Unwind_Exception *exc, void *opaque_ctx);

extern "C" _Unwind_Reason_Code
_Unwind_RaiseException(_Unwind_Exception *exc)
{
    // ── phase 1: search ────────────────────────────────────────────────
    UnwindContext ctx;
    unw::UnwCaptureContext(&ctx.file);

    uint64_t handler_cfa = 0;
    for (;;) {
        if (!DecodeFrame(&ctx)) return _URC_END_OF_STACK;

        if (ctx.frame.cie.personality) {
            auto personality =
                reinterpret_cast<__personality_routine>(ctx.frame.cie.personality);
            _Unwind_Reason_Code r =
                personality(1, _UA_SEARCH_PHASE, exc->exception_class, exc,
                            ToAbi(&ctx));
            if (r == _URC_HANDLER_FOUND) {
                uint64_t cfa = 0;
                UnwindContext probe = ctx;
                (void)unw::DwarfStep(probe.frame, &probe.file, &cfa);
                handler_cfa = cfa;
                break;
            }
            if (r != _URC_CONTINUE_UNWIND) return _URC_FATAL_PHASE1_ERROR;
        }

        uint64_t cfa = 0;
        if (!StepFrame(&ctx, &cfa)) return _URC_END_OF_STACK;
    }

    exc->private_1 = handler_cfa;
    exc->private_2 = 0;

    // ── phase 2: cleanup + install ─────────────────────────────────────
    unw::UnwCaptureContext(&ctx.file);
    return _Unwind_RaiseException_Phase2(exc, &ctx);
}

static _Unwind_Reason_Code
_Unwind_RaiseException_Phase2(_Unwind_Exception *exc, void *opaque_ctx)
{
    UnwindContext *ctx = static_cast<UnwindContext *>(opaque_ctx);

    for (;;) {
        if (!DecodeFrame(ctx)) return _URC_FATAL_PHASE2_ERROR;

        uint64_t cfa = 0;
        {
            UnwindContext probe = *ctx;
            (void)unw::DwarfStep(probe.frame, &probe.file, &cfa);
        }
        bool handler_frame = (cfa == exc->private_1);

        if (ctx->frame.cie.personality) {
            // The frame's real RA — needed for shadow-stack reconciliation
            // BEFORE the personality redirects the IP to a landing pad.
            uint64_t preserved_ra = ctx->file.regs[unw::kRegRa];
            auto personality =
                reinterpret_cast<__personality_routine>(ctx->frame.cie.personality);
            _Unwind_Action actions = _UA_CLEANUP_PHASE;
            if (handler_frame) actions |= _UA_HANDLER_FRAME;
            _Unwind_Reason_Code r =
                personality(1, actions, exc->exception_class, exc, ToAbi(ctx));
            if (r == _URC_INSTALL_CONTEXT) InstallContext(ctx, preserved_ra);
            if (r != _URC_CONTINUE_UNWIND) return _URC_FATAL_PHASE2_ERROR;
        }

        if (handler_frame)
            return _URC_FATAL_PHASE2_ERROR;   // handler frame must install

        uint64_t stepped_cfa = 0;
        if (!StepFrame(ctx, &stepped_cfa)) return _URC_FATAL_PHASE2_ERROR;
        exc->private_2++;   // one more frame skipped (CET INCSSP count)
    }
}

extern "C" void _Unwind_Resume(_Unwind_Exception *exc)
{
    UnwindContext ctx;
    unw::UnwCaptureContext(&ctx.file);

    // Resume continues phase 2 from a cleanup landing pad: step out of
    // the cleanup's frame first, then keep unwinding toward private_1.
    (void)_Unwind_RaiseException_Phase2(exc, &ctx);
    boxcxx::Panic("_Unwind_Resume: unwinding failed");
}

extern "C" _Unwind_Reason_Code
_Unwind_Resume_or_Rethrow(_Unwind_Exception *exc)
{
    // Foreign/forced exceptions would need the stop-fn path; for native
    // exceptions rethrow is a fresh raise.
    return _Unwind_RaiseException(exc);
}

extern "C" _Unwind_Reason_Code
_Unwind_ForcedUnwind(_Unwind_Exception *exc, _Unwind_Stop_Fn stop,
                     void *stop_arg)
{
    (void)exc;
    (void)stop;
    (void)stop_arg;
    // No consumer in BoxOS yet (no pthread_cancel analogue). Implemented
    // when a real caller appears — failing loudly beats a silent stub.
    boxcxx::Panic("_Unwind_ForcedUnwind: no consumer wired yet");
}

extern "C" void _Unwind_DeleteException(_Unwind_Exception *exc)
{
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}

// ── backtrace ───────────────────────────────────────────────────────────

extern "C" _Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace,
                                                 void *arg)
{
    UnwindContext ctx;
    unw::UnwCaptureContext(&ctx.file);

    for (;;) {
        if (!DecodeFrame(&ctx)) return _URC_END_OF_STACK;

        _Unwind_Reason_Code r = trace(ToAbi(&ctx), arg);
        if (r != _URC_NO_REASON) return r;

        uint64_t cfa = 0;
        if (!StepFrame(&ctx, &cfa)) return _URC_END_OF_STACK;
    }
}
