/*
 * csignal.cpp — [support.signal] and the one function specified in terms of
 * it, std::abort.
 *
 * There is no asynchronous half. BoxOS raises nothing at a process: its event
 * road is Touch, and a second one shaped like Unix signals is exactly what the
 * system exists not to be. What lives here is the synchronous half the
 * standard specifies — install a disposition, deliver one on this strand — and
 * it is complete.
 *
 * The dispositions live in one process-wide table. Signals here are always
 * delivered by the strand that raises them, but the TABLE is shared: one
 * strand may install a handler while another raises, so every slot is an
 * atomic and raise() reads its slot exactly once. Reading twice would let a
 * concurrent signal() split the decision from the call.
 */

#include <atomic>
#include <csignal>
#include <__bits/c_terminate>

#include "box/print.h"    // io_flush
#include "box/system.h"   // _Exit

namespace {

// Indexed by signal number. SIGTERM is 15, so sixteen slots cover every
// number this header defines, and the unused ones cost one pointer each.
constexpr int kSlots = 16;

std::atomic<__boxcxx_signal_handler *> g_disposition[kSlots] = {};

constexpr bool KnownSignal(int sig) noexcept
{
    return sig == SIGINT || sig == SIGILL || sig == SIGABRT || sig == SIGFPE ||
           sig == SIGSEGV || sig == SIGTERM;
}

// Default disposition: end the process, no cleanup. 128 + signal is the status
// convention boxcxx already used before this file existed (Panic exits 134,
// and 134 is 128 + SIGABRT). The console is flushed first: output is batched
// per strand in boxlib's console-lane frame, and a diagnostic that never left
// the frame is a diagnostic that never happened on a machine with no debugger
// attached.
[[noreturn]] void DefaultDeath(int sig) noexcept
{
    io_flush();
    _Exit(128 + sig);
}

} // namespace

namespace std {

__boxcxx_signal_handler *signal(int sig, __boxcxx_signal_handler *func) noexcept
{
    if (!KnownSignal(sig) || func == SIG_ERR) return SIG_ERR;
    return g_disposition[sig].exchange(func, memory_order_acq_rel);
}

int raise(int sig) noexcept
{
    if (!KnownSignal(sig)) return -1;

    // Once. A second load could see a disposition installed between the test
    // and the call, and then this strand would test one thing and do another.
    __boxcxx_signal_handler *handler =
        g_disposition[sig].load(memory_order_acquire);

    if (handler == SIG_IGN) return 0;
    if (handler == SIG_DFL) DefaultDeath(sig);

    // The disposition is deliberately NOT reset to SIG_DFL first (C leaves the
    // choice open). A handler that must reinstall itself on every delivery
    // races any other strand raising the same signal in the window.
    handler(sig);
    return 0;
}

// [support.start.term]/9: abort() raises SIGABRT, and the program still ends
// abnormally if the handler returns or the signal is ignored. Both of those
// fall out of the sequence below rather than being special-cased: raise()
// returns in exactly those two cases, and there is nothing after it but the
// death that was going to happen anyway.
//
// No atexit callbacks, no static destructors, no .fini_array — that is the
// difference between this and exit(), and it is the whole point of abort()
// existing. Until this file, boxcxx's fatal paths went through exit(), which
// runs all three; see boxcxx::Panic.
[[noreturn]] void abort() noexcept
{
    raise(SIGABRT);
    io_flush();
    _Exit(128 + SIGABRT);
}

} // namespace std
