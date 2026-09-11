#ifndef BOXCXX_UNWIND_H
#define BOXCXX_UNWIND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long _Unwind_Word;
typedef long long _Unwind_Sword;
typedef unsigned long long _Unwind_Ptr;
typedef unsigned long long _Unwind_Exception_Class;

typedef enum {
    _URC_NO_REASON                = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR       = 2,
    _URC_FATAL_PHASE1_ERROR       = 3,
    _URC_NORMAL_STOP              = 4,
    _URC_END_OF_STACK             = 5,
    _URC_HANDLER_FOUND            = 6,
    _URC_INSTALL_CONTEXT          = 7,
    _URC_CONTINUE_UNWIND          = 8,
} _Unwind_Reason_Code;

typedef int _Unwind_Action;
#define _UA_SEARCH_PHASE  1
#define _UA_CLEANUP_PHASE 2
#define _UA_HANDLER_FRAME 4
#define _UA_FORCE_UNWIND  8
#define _UA_END_OF_STACK  16

struct _Unwind_Exception;

typedef void (*_Unwind_Exception_Cleanup_Fn)(_Unwind_Reason_Code,
                                             struct _Unwind_Exception *);

struct _Unwind_Exception {
    _Unwind_Exception_Class      exception_class;
    _Unwind_Exception_Cleanup_Fn exception_cleanup;
    _Unwind_Word                 private_1;
    _Unwind_Word                 private_2;
} __attribute__((aligned(16)));

struct _Unwind_Context;

typedef _Unwind_Reason_Code (*_Unwind_Stop_Fn)(int version,
                                               _Unwind_Action actions,
                                               _Unwind_Exception_Class cls,
                                               struct _Unwind_Exception *exc,
                                               struct _Unwind_Context *ctx,
                                               void *stop_arg);

typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context *ctx,
                                                void *arg);

typedef _Unwind_Reason_Code (*__personality_routine)(
    int version, _Unwind_Action actions, _Unwind_Exception_Class cls,
    struct _Unwind_Exception *exc, struct _Unwind_Context *ctx);

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *exc);
void _Unwind_Resume(struct _Unwind_Exception *exc);
_Unwind_Reason_Code
_Unwind_Resume_or_Rethrow(struct _Unwind_Exception *exc);
_Unwind_Reason_Code _Unwind_ForcedUnwind(struct _Unwind_Exception *exc,
                                         _Unwind_Stop_Fn stop,
                                         void *stop_arg);
void _Unwind_DeleteException(struct _Unwind_Exception *exc);
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace, void *arg);

_Unwind_Word _Unwind_GetGR(struct _Unwind_Context *ctx, int reg);
void _Unwind_SetGR(struct _Unwind_Context *ctx, int reg, _Unwind_Word value);
_Unwind_Word _Unwind_GetIP(struct _Unwind_Context *ctx);
_Unwind_Word _Unwind_GetIPInfo(struct _Unwind_Context *ctx,
                               int *ip_before_insn);
void _Unwind_SetIP(struct _Unwind_Context *ctx, _Unwind_Word value);
_Unwind_Word _Unwind_GetCFA(struct _Unwind_Context *ctx);
_Unwind_Word _Unwind_GetLanguageSpecificData(struct _Unwind_Context *ctx);
_Unwind_Word _Unwind_GetRegionStart(struct _Unwind_Context *ctx);

#ifdef __cplusplus
}
#endif

#endif