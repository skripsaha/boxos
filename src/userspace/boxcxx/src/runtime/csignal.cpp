
#include <atomic>
#include <csignal>
#include <__bits/c_terminate>

#include "box/print.h"
#include "box/system.h"

namespace {

constexpr int kSlots = 16;

std::atomic<__boxcxx_signal_handler *> g_disposition[kSlots] = {};

constexpr bool KnownSignal(int sig) noexcept
{
    return sig == SIGINT || sig == SIGILL || sig == SIGABRT || sig == SIGFPE ||
           sig == SIGSEGV || sig == SIGTERM;
}

[[noreturn]] void DefaultDeath(int sig) noexcept
{
    io_flush();
    _Exit(128 + sig);
}

}

namespace std {

__boxcxx_signal_handler *signal(int sig, __boxcxx_signal_handler *func) noexcept
{
    if (!KnownSignal(sig) || func == SIG_ERR) return SIG_ERR;
    return g_disposition[sig].exchange(func, memory_order_acq_rel);
}

int raise(int sig) noexcept
{
    if (!KnownSignal(sig)) return -1;

    __boxcxx_signal_handler *handler =
        g_disposition[sig].load(memory_order_acquire);

    if (handler == SIG_IGN) return 0;
    if (handler == SIG_DFL) DefaultDeath(sig);

    handler(sig);
    return 0;
}

[[noreturn]] void abort() noexcept
{
    raise(SIGABRT);
    io_flush();
    _Exit(128 + SIGABRT);
}

}