#ifndef KSTDARG_H
#define KSTDARG_H

// ============================================================================
// BOXOS KERNEL STDARG - NO STDLIB DEPENDENCIES!
// ============================================================================
// This header replaces: stdarg.h
// Provides variadic argument handling using GCC built-ins

// ========== VARIADIC ARGUMENTS TYPE ==========

// The va_list type - opaque pointer to variable arguments
typedef __builtin_va_list va_list;

// ========== VARIADIC ARGUMENTS MACROS ==========

// Initialize va_list to start of variable arguments
#define va_start(ap, last) __builtin_va_start(ap, last)

// Get next argument from va_list
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

// Clean up va_list (usually no-op on x86-64, but good practice)
#define va_end(ap)         __builtin_va_end(ap)

// Copy va_list (needed when passing va_list to another function)
#define va_copy(dest, src) __builtin_va_copy(dest, src)

// ========== NOTES ==========
// These macros use GCC/Clang built-in functions which are compiler-intrinsic
// and do not require any C standard library code. They work at the compiler
// level and generate appropriate machine code for the target architecture.
//
// On x86-64:
// - va_list is typically a struct containing pointers and offsets
// - Arguments are passed in registers (rdi, rsi, rdx, rcx, r8, r9) first
// - Remaining arguments go on the stack
// - The compiler handles all the complexity via __builtin_* functions

#endif // KSTDARG_H
