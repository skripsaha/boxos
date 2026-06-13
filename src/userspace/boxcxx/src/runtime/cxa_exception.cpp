/*
 * cxa_exception.cpp — Itanium Level 2 exception object management:
 * __cxa_allocate_exception / __cxa_throw / __cxa_begin_catch /
 * __cxa_end_catch / __cxa_rethrow (+ emergency pool for OOM throws).
 *
 * One thread per cabin — the "caught stack" and counters are plain
 * globals.
 */

#include "cxxabi_typeinfo.h"

#include <unwind.h>
#include <cstdint>
#include <cstddef>

#include "box/print.h"

extern "C" {
void *_malloc_impl(size_t size);
void  free(void *ptr);
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace std {
[[noreturn]] void terminate() noexcept;
}

namespace {

// "BOXSC++\0" — vendor(4) + language(4); low byte 0 marks the base class.
constexpr uint64_t kExceptionClass =
    (uint64_t('B') << 56) | (uint64_t('O') << 48) | (uint64_t('X') << 40) |
    (uint64_t('S') << 32) | (uint64_t('C') << 24) | (uint64_t('+') << 16) |
    (uint64_t('+') << 8);

} // namespace

namespace __cxxabiv1 {

extern "C" void *__cxa_allocate_exception(size_t thrown_size) noexcept;
extern "C" void __cxa_free_exception(void *thrown_object) noexcept;
extern "C" void __cxa_increment_exception_refcount(void *obj) noexcept;
extern "C" void __cxa_decrement_exception_refcount(void *obj) noexcept;

// Itanium __cxa_exception with the refcount extension; the thrown object
// immediately follows this header (unwindHeader is last on purpose).
struct CxaException {
    size_t            referenceCount;
    std::type_info   *exceptionType;
    void            (*exceptionDestructor)(void *);
    void             *unexpectedHandler;   // legacy slots, kept for layout
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

CxaException *g_caught_stack   = nullptr;
int           g_uncaught_count = 0;

// Emergency pool: enough for a bad_alloc cascade when the heap is gone.
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
    // Vendor+language match, any low-byte variant.
    return (exc->exception_class & ~0xFFull) == kExceptionClass;
}

void ExceptionCleanup(_Unwind_Reason_Code, _Unwind_Exception *exc)
{
    CxaException *header = FromUnwind(exc);
    if (header->exceptionDestructor)
        header->exceptionDestructor(header + 1);
    __cxa_free_exception(header + 1);
}

// A dependent exception (rethrow_exception) reuses the CxaException layout
// but carries no payload: the referenceCount slot holds the primary object
// pointer and the low class byte is 0x01.
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

} // namespace

extern "C" int *__boxcxx_uncaught_count()
{
    return &g_uncaught_count;
}

extern "C" void *__cxa_allocate_exception(size_t thrown_size) noexcept
{
    void *raw = _malloc_impl(sizeof(CxaException) + thrown_size);
    if (!raw) {
        if (thrown_size <= kEmergencySlotPayload) {
            for (auto &slot : g_emergency) {
                if (!slot.used) {
                    slot.used = true;
                    raw       = slot.bytes;
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
            slot.used = false;
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

    // Unwinding only comes back on failure.
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
        // Foreign exception: only catch(...) reaches here; no object
        // pointer to give and no caught-stack bookkeeping beyond count.
        return nullptr;
    }

    CxaException *header = FromUnwind(exc);

    // Negative handlerCount marks a rethrown exception in flight.
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
        // Rethrown: leave it alive; the rethrow owns the lifetime now.
        if (++header->handlerCount == 0) g_caught_stack = header->nextException;
        return;
    }

    if (--header->handlerCount == 0) {
        g_caught_stack = header->nextException;
        if (IsDependent(header)) {
            void *primary = reinterpret_cast<void *>(header->referenceCount);
            __cxa_decrement_exception_refcount(primary);
            free(header);
        } else if (--header->referenceCount == 0) {
            if (header->exceptionDestructor)
                header->exceptionDestructor(header + 1);
            __cxa_free_exception(header + 1);
        }
    }
}

// ── exception_ptr ABI ([propagation] — refcounted primary + dependent) ──

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

extern "C" [[noreturn]] void __cxa_rethrow_primary_exception(void *obj)
{
    if (!obj) std::terminate(); // rethrowing a null exception_ptr

    CxaException *primary = static_cast<CxaException *>(obj) - 1;
    CxaException *dep =
        static_cast<CxaException *>(_malloc_impl(sizeof(CxaException)));
    if (!dep)
        boxcxx::Panic("__cxa_rethrow_primary_exception: out of memory");

    __builtin_memset(dep, 0, sizeof(CxaException));
    dep->referenceCount       = reinterpret_cast<size_t>(obj); // primary obj
    dep->exceptionType        = primary->exceptionType;
    dep->exceptionDestructor  = primary->exceptionDestructor;
    __cxa_increment_exception_refcount(obj); // the dependent owns a ref
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

    header->handlerCount = -header->handlerCount;   // mark rethrown
    g_uncaught_count++;

    _Unwind_Reason_Code code =
        _Unwind_RaiseException(&header->unwindHeader);

    printf("[boxcxx] rethrow failed (unwind code %d)\n", int(code));
    std::terminate();
}

// noexcept violations / legacy dynamic-spec mismatches land here via the
// personality's filter<0 path.
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

} // namespace __cxxabiv1
