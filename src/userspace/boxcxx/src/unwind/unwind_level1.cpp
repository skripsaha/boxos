
#include "unwind_internal.h"

#include <unwind.h>

namespace unw = boxcxx::unwind;

namespace {

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

bool DecodeFrame(UnwindContext *ctx)
{
    ctx->frame_valid =
        unw::DwarfFindFrame(ctx->file.regs[unw::kRegRa] - 1, &ctx->frame);
    return ctx->frame_valid;
}

bool StepFrame(UnwindContext *ctx, uint64_t *cfa_out)
{
    return unw::DwarfStep(ctx->frame, &ctx->file, cfa_out);
}

[[noreturn]] void InstallContext(UnwindContext *ctx, uint64_t target_ra)
{
    ctx->file.regs[unw::kRegRsp] += ctx->frame.state.args_size;

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

}


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


static _Unwind_Reason_Code
_Unwind_RaiseException_Phase2(_Unwind_Exception *exc, void *opaque_ctx);

extern "C" _Unwind_Reason_Code
_Unwind_RaiseException(_Unwind_Exception *exc)
{
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
            return _URC_FATAL_PHASE2_ERROR;

        uint64_t stepped_cfa = 0;
        if (!StepFrame(ctx, &stepped_cfa)) return _URC_FATAL_PHASE2_ERROR;
        exc->private_2++;
    }
}

extern "C" void _Unwind_Resume(_Unwind_Exception *exc)
{
    UnwindContext ctx;
    unw::UnwCaptureContext(&ctx.file);

    (void)_Unwind_RaiseException_Phase2(exc, &ctx);
    boxcxx::Panic("_Unwind_Resume: unwinding failed");
}

extern "C" _Unwind_Reason_Code
_Unwind_Resume_or_Rethrow(_Unwind_Exception *exc)
{
    return _Unwind_RaiseException(exc);
}

extern "C" _Unwind_Reason_Code
_Unwind_ForcedUnwind(_Unwind_Exception *exc, _Unwind_Stop_Fn stop,
                     void *stop_arg)
{
    (void)exc;
    (void)stop;
    (void)stop_arg;
    boxcxx::Panic("_Unwind_ForcedUnwind: no consumer wired yet");
}

extern "C" void _Unwind_DeleteException(_Unwind_Exception *exc)
{
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}


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