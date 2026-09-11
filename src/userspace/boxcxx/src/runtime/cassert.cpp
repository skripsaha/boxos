
#include <__bits/c_terminate>

#include "box/print.h"

namespace {

constexpr const char *kNoFunction = "<unknown>";

}

extern "C" [[noreturn]] void __boxcxx_assert_fail(const char *expr,
                                                  const char *file,
                                                  unsigned    line,
                                                  const char *func) noexcept
{
    printf("[boxcxx] assertion failed: %s\n",
           expr ? expr : "<expression unavailable>");
    printf("         at %s:%u in %s\n",
           file ? file : "<file unavailable>", line,
           func ? func : kNoFunction);

    std::abort();
}