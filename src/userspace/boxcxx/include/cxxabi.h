#ifndef BOXCXX_CXXABI_H
#define BOXCXX_CXXABI_H

/* boxcxx — <cxxabi.h>: the Itanium C++ ABI's own header, not a standard one.
 *
 * It exists here for one entry point so far. __cxa_demangle turns the name a
 * linker keeps into the name a person reads, and BoxOS needs that wherever an
 * address is named: std::stacktrace descriptions, typeid().name() output a
 * program wants to print, a future shell command over an image's Nameplate.
 *
 * The implementation (src/runtime/demangle.cpp) is measured against the
 * reference rather than believed: every C++ name in the shipped images is
 * demangled and compared byte for byte with libiberty's answer.
 */

#include <stddef.h>

namespace __cxxabiv1 {

/* Demangle `mangled`.
 *
 * buf/n follow the ABI: pass (nullptr, nullptr) to have the result malloc'd,
 * or a buffer and its size to have it reused when it fits. The caller frees
 * whatever comes back.
 *
 * *status is 0 on success, -1 out of memory, -2 not a valid mangled name,
 * -3 bad arguments. A name this implementation cannot parse returns nullptr
 * with -2, which is the honest answer and leaves the caller free to print the
 * mangled form it already has.
 */
extern "C" char *__cxa_demangle(const char *mangled, char *buf, size_t *n, int *status);

}   // namespace __cxxabiv1

using __cxxabiv1::__cxa_demangle;

#endif /* BOXCXX_CXXABI_H */
