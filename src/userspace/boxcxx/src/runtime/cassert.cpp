/*
 * cassert.cpp — what assert() calls when the expression is false.
 *
 * [assertions.assert]/2 asks for "information about the particular call that
 * failed" in an implementation-defined format, and then for a call to abort().
 * Both halves matter and the second one is the reason this is not simply a
 * printf at the macro site: abort() runs no atexit callbacks and no static
 * destructors, so an assertion failure cannot be papered over by teardown code
 * that then does something else. Before <csignal> existed, every fatal path in
 * boxcxx went through exit(), which runs all of it.
 */

#include <__bits/c_terminate>

#include "box/print.h"

namespace {

// The diagnostic is two lines on purpose: a real console is 80 columns, and
// an expression plus a path plus a function name is routinely more than that.
// Wrapping in the middle of the expression is what makes the interesting half
// scroll off.
constexpr const char *kNoFunction = "<unknown>";

} // namespace

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
