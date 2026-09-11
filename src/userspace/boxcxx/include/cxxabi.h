#ifndef BOXCXX_CXXABI_H
#define BOXCXX_CXXABI_H


#include <stddef.h>

namespace __cxxabiv1 {

extern "C" char *__cxa_demangle(const char *mangled, char *buf, size_t *n, int *status);

}

using __cxxabiv1::__cxa_demangle;

#endif