
#include "cxxabi_typeinfo.h"

#include <unwind.h>
#include <cstdint>
#include <cstddef>

#include "box/print.h"

extern "C" {
void *malloc(size_t size);
void  free(void *ptr);
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace std {
[[noreturn]] void terminate() noexcept;
}

namespace {

constexpr uint64_t kExceptionClass =
    (uint64_t('B') << 56) | (uint64_t('O') << 48) | (uint64_t('X') << 40) |
    (uint64_t('S') << 32) | (uint64_t('C') << 24) | (uint64_t('+') << 16) |
    (uint64_t('+') << 8);

}

namespace __cxxabiv1 {

extern "C" void *__cxa_allocate_exception(size_t thrown_size) noexcept;
extern "C" void __cxa_free_exception(void *thrown_object) noexcept;
extern "C" void __cxa_increment_exception_refcount(void *obj) noexcept;
extern "C" void __cxa_decrement_exception_refcount(void *obj) noexcept;

struct CxaException {
    size_t            referenceCount;
    std::type_info   *exceptionType;
    void            (*exceptionDestructor)(void *);
    void             *unexpectedHandler;
    void             *terminateHandler;
    CxaException     *nextException;
    int               handlerCount;
    int               handlerSwitchValue;
    const uint8_t    *actionRecord;
    const uint8_t    *languageSpecificData;
    void             *catchTemp;
    void             *adjustedPtr;
    _Unwind_Exception unwindHeader;
};

static_assert(sizeof(_Unwind_Exception) == 32, "unwind header ABI shape");

namespace {

thread_local CxaException *g_caught_stack   = nullptr;
thread_local int           g_uncaught_count = 0;

constexpr size_t kEmergencySlotPayload = 256;
constexpr int    kEmergencySlots       = 8;
struct EmergencySlot {
    alignas(16) unsigned char
        bytes[sizeof(CxaException) + kEmergencySlotPayload];
    bool used;
};
EmergencySlot g_emergency[kEmergencySlots];

CxaException *FromUnwind(_Unwind_Exception *exc)
{
    return reinterpret_cast<CxaException *>(
        reinterpret_cast<char *>(exc + 1)) - 1;
}

bool IsNative(_Unwind_Exception *exc)
{
    return (exc->exception_class & ~0xFFull) == kExceptionClass;
}

void ExceptionCleanup(_Unwind_Reason_Code, _Unwind_Exception *exc)
{
    CxaException *header = FromUnwind(exc);
    if (header->exceptionDestructor)
        header->exceptionDestructor(header + 1);
    __cxa_free_exception(header + 1);
}

constexpr uint64_t kDependentClass = kExceptionClass | 0x01ull;

bool IsDependent(CxaException *h)
{
    return (h->unwindHeader.exception_class & 0xFFull) == 0x01;
}

void DependentCleanup(_Unwind_Reason_Code, _Unwind_Exception *exc)
{
    CxaException *dep     = FromUnwind(exc);
    void         *primary = reinterpret_cast<void *>(dep->referenceCount);
    __cxa_decrement_exception_refcount(primary);
    free(dep);
}

}

extern "C" int *__boxcxx_uncaught_count()
{
    return &g_uncaught_count;
}

extern "C" void *__cxa_allocate_exception(size_t thrown_size) noexcept
{
    void *raw = malloc(sizeof(CxaException) + thrown_size);
    if (!raw) {
        if (thrown_size <= kEmergencySlotPayload) {
            for (auto &slot : g_emergency) {
                if (!__atomic_exchange_n(&slot.used, true, __ATOMIC_ACQ_REL)) {
                    raw = slot.bytes;
                    break;
                }
            }
        }
        if (!raw)
            boxcxx::Panic("__cxa_allocate_exception: heap and emergency "
                          "pool exhausted");
    }

    auto *header = static_cast<CxaException *>(raw);
    __builtin_memset(header, 0, sizeof(CxaException));
    return header + 1;
}

extern "C" void __cxa_free_exception(void *thrown_object) noexcept
{
    auto *header = static_cast<CxaException *>(thrown_object) - 1;
    for (auto &slot : g_emergency) {
        if (static_cast<void *>(slot.bytes) == static_cast<void *>(header)) {
            __atomic_store_n(&slot.used, false, __ATOMIC_RELEASE);
            return;
        }
    }
    free(header);
}

extern "C" [[noreturn]] void __cxa_throw(void *thrown_object,
                                         std::type_info *tinfo,
                                         void (*destructor)(void *))
{
    CxaException *header = static_cast<CxaException *>(thrown_object) - 1;

    header->referenceCount      = 1;
    header->exceptionType       = tinfo;
    header->exceptionDestructor = destructor;
    header->unwindHeader.exception_class   = kExceptionClass;
    header->unwindHeader.exception_cleanup = ExceptionCleanup;

    g_uncaught_count++;

    _Unwind_Reason_Code code =
        _Unwind_RaiseException(&header->unwindHeader);

    printf("[boxcxx] unhandled exception of type %s (unwind code %d)\n",
           tinfo ? tinfo->name() : "?", int(code));
    std::terminate();
}

extern "C" void *__cxa_get_exception_ptr(void *unwind_exc) noexcept
{
    auto *exc = static_cast<_Unwind_Exception *>(unwind_exc);
    return FromUnwind(exc)->adjustedPtr;
}

extern "C" void *__cxa_begin_catch(void *unwind_exc) noexcept
{
    auto *exc = static_cast<_Unwind_Exception *>(unwind_exc);

    if (!IsNative(exc)) {
        return nullptr;
    }

    CxaException *header = FromUnwind(exc);

    header->handlerCount = header->handlerCount < 0
                               ? -header->handlerCount + 1
                               : header->handlerCount + 1;
    g_uncaught_count--;

    if (g_caught_stack != header) {
        header->nextException = g_caught_stack;
        g_caught_stack        = header;
    }
    return header->adjustedPtr;
}

extern "C" void __cxa_end_catch()
{
    CxaException *header = g_caught_stack;
    if (!header) boxcxx::Panic("__cxa_end_catch: no active exception");

    if (header->handlerCount < 0) {
        if (++header->handlerCount == 0) g_caught_stack = header->nextException;
        return;
    }

    if (--header->handlerCount == 0) {
        g_caught_stack = header->nextException;
        if (IsDependent(header)) {
            void *primary = reinterpret_cast<void *>(header->referenceCount);
            __cxa_decrement_exception_refcount(primary);
            free(header);
        } else if (__atomic_sub_fetch(&header->referenceCount, 1, __ATOMIC_ACQ_REL) == 0) {
            if (header->exceptionDestructor)
                header->exceptionDestructor(header + 1);
            __cxa_free_exception(header + 1);
        }
    }
}


extern "C" void __cxa_increment_exception_refcount(void *obj) noexcept
{
    if (!obj) return;
    CxaException *h = static_cast<CxaException *>(obj) - 1;
    __atomic_add_fetch(&h->referenceCount, 1, __ATOMIC_ACQ_REL);
}

extern "C" void __cxa_decrement_exception_refcount(void *obj) noexcept
{
    if (!obj) return;
    CxaException *h = static_cast<CxaException *>(obj) - 1;
    if (__atomic_sub_fetch(&h->referenceCount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (h->exceptionDestructor) h->exceptionDestructor(obj);
        __cxa_free_exception(obj);
    }
}

extern "C" void *__cxa_current_primary_exception() noexcept
{
    CxaException *header = g_caught_stack;
    if (!header) return nullptr;
    void *obj = IsDependent(header)
                    ? reinterpret_cast<void *>(header->referenceCount)
                    : static_cast<void *>(header + 1);
    __cxa_increment_exception_refcount(obj);
    return obj;
}

extern "C" void *__boxcxx_exception_ptr_cast(void *obj, const std::type_info *want) noexcept
{
    if (!obj || !want) return nullptr;
    CxaException         *header = static_cast<CxaException *>(obj) - 1;
    const std::type_info *thrown = header->exceptionType;
    if (!thrown) return nullptr;
    void *adjusted = obj;
    if (want->__do_catch(thrown, &adjusted, 1)) return adjusted;
    return nullptr;
}

extern "C" [[noreturn]] void __cxa_rethrow_primary_exception(void *obj)
{
    if (!obj) std::terminate();

    CxaException *primary = static_cast<CxaException *>(obj) - 1;
    CxaException *dep =
        static_cast<CxaException *>(malloc(sizeof(CxaException)));
    if (!dep)
        boxcxx::Panic("__cxa_rethrow_primary_exception: out of memory");

    __builtin_memset(dep, 0, sizeof(CxaException));
    dep->referenceCount       = reinterpret_cast<size_t>(obj);
    dep->exceptionType        = primary->exceptionType;
    dep->exceptionDestructor  = primary->exceptionDestructor;
    __cxa_increment_exception_refcount(obj);
    dep->unwindHeader.exception_class   = kDependentClass;
    dep->unwindHeader.exception_cleanup = DependentCleanup;

    g_uncaught_count++;
    _Unwind_Reason_Code code = _Unwind_RaiseException(&dep->unwindHeader);

    printf("[boxcxx] rethrow_primary failed (unwind code %d)\n", int(code));
    std::terminate();
}

extern "C" [[noreturn]] void __cxa_rethrow()
{
    CxaException *header = g_caught_stack;
    if (!header) {
        printf("[boxcxx] rethrow with no active exception\n");
        std::terminate();
    }

    header->handlerCount = -header->handlerCount;
    g_uncaught_count++;

    _Unwind_Reason_Code code =
        _Unwind_RaiseException(&header->unwindHeader);

    printf("[boxcxx] rethrow failed (unwind code %d)\n", int(code));
    std::terminate();
}

extern "C" [[noreturn]] void __cxa_call_unexpected(void *unwind_exc)
{
    auto *exc = static_cast<_Unwind_Exception *>(unwind_exc);
    if (IsNative(exc)) {
        CxaException *header = FromUnwind(exc);
        printf("[boxcxx] noexcept violated by exception of type %s\n",
               header->exceptionType ? header->exceptionType->name() : "?");
    } else {
        printf("[boxcxx] noexcept violated by foreign exception\n");
    }
    std::terminate();
}

extern "C" [[noreturn]] void __cxa_call_terminate(void *unwind_exc)
{
    auto *exc = static_cast<_Unwind_Exception *>(unwind_exc);
    if (!exc) std::terminate();
    if (IsNative(exc)) {
        CxaException *header = FromUnwind(exc);
        printf("[boxcxx] terminate: exception of type %s escaped while unwinding\n",
               header->exceptionType ? header->exceptionType->name() : "?");
        __cxa_begin_catch(unwind_exc);
    } else {
        printf("[boxcxx] terminate: a foreign exception escaped while unwinding\n");
    }
    std::terminate();
}

}