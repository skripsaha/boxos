// boxcxx — <stdatomic.h>  ([stdatomic.h.syn], P0943R6, C++23)
//
// The C spelling of <atomic>, and nothing more: _Atomic(T) becomes
// std::atomic<T>, and every name [atomics.syn] declares is re-exported into
// the global namespace. C code carrying atomics into BoxOS's C++ userspace
// compiles unchanged and gets the SAME objects the C++ side uses -- there
// is no second implementation to keep in step.
//
// The ATOMIC_*_LOCK_FREE and ATOMIC_FLAG_INIT macros need no re-export;
// <atomic> already defines them as macros, which have no namespace.
//
// Filling this in is what surfaced the missing atomic_int_least*_t /
// atomic_int_fast*_t aliases in <atomic>: [atomics.syn] has listed them
// since C++11 and boxcxx never had them. They are there now, and the
// using-declarations below would not compile if they went away again.
#ifndef BOXCXX_STDATOMIC_H
#define BOXCXX_STDATOMIC_H

#include <atomic>

// [version.syn] names this header as an owner of the macro below;
// nothing else here is declared, so nothing else is answered for.
#define BOXCXX_OWNS_stdatomic_h
#include <__bits/version_stdatomic>

#define _Atomic(T) ::std::atomic<T>

using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_consume;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

using std::atomic_flag;

using std::atomic_bool;
using std::atomic_char;
using std::atomic_char16_t;
using std::atomic_char32_t;
using std::atomic_char8_t;
using std::atomic_int;
using std::atomic_llong;
using std::atomic_long;
using std::atomic_schar;
using std::atomic_short;
using std::atomic_uchar;
using std::atomic_uint;
using std::atomic_ullong;
using std::atomic_ulong;
using std::atomic_ushort;
using std::atomic_wchar_t;

using std::atomic_int16_t;
using std::atomic_int32_t;
using std::atomic_int64_t;
using std::atomic_int8_t;
using std::atomic_uint16_t;
using std::atomic_uint32_t;
using std::atomic_uint64_t;
using std::atomic_uint8_t;

using std::atomic_int_least16_t;
using std::atomic_int_least32_t;
using std::atomic_int_least64_t;
using std::atomic_int_least8_t;
using std::atomic_uint_least16_t;
using std::atomic_uint_least32_t;
using std::atomic_uint_least64_t;
using std::atomic_uint_least8_t;

using std::atomic_int_fast16_t;
using std::atomic_int_fast32_t;
using std::atomic_int_fast64_t;
using std::atomic_int_fast8_t;
using std::atomic_uint_fast16_t;
using std::atomic_uint_fast32_t;
using std::atomic_uint_fast64_t;
using std::atomic_uint_fast8_t;

using std::atomic_intmax_t;
using std::atomic_intptr_t;
using std::atomic_ptrdiff_t;
using std::atomic_size_t;
using std::atomic_uintmax_t;
using std::atomic_uintptr_t;

using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_strong_explicit;
using std::atomic_compare_exchange_weak;
using std::atomic_compare_exchange_weak_explicit;
using std::atomic_exchange;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_and;
using std::atomic_fetch_and_explicit;
using std::atomic_fetch_or;
using std::atomic_fetch_or_explicit;
using std::atomic_fetch_sub;
using std::atomic_fetch_sub_explicit;
using std::atomic_fetch_xor;
using std::atomic_fetch_xor_explicit;
using std::atomic_is_lock_free;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_store;
using std::atomic_store_explicit;

using std::atomic_flag_clear;
using std::atomic_flag_clear_explicit;
using std::atomic_flag_test_and_set;
using std::atomic_flag_test_and_set_explicit;

using std::atomic_signal_fence;
using std::atomic_thread_fence;

#endif // BOXCXX_STDATOMIC_H
