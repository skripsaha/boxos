/*
 * mutex.cpp — thread identity for std::recursive_mutex ownership.
 *
 * The identity is the address of a thread_local anchor: unique per
 * thread within a cabin (1 byte of .tbss through the boxcxx TLS
 * bootstrap), and automatically correct once the strands epic brings
 * real multi-threading. Cross-cabin identity is out of scope — mutexes
 * are cabin-private (see <mutex> header contract).
 */

namespace {
thread_local unsigned char g_thread_anchor;
}

extern "C" void *__boxcxx_thread_id() noexcept
{
    return &g_thread_anchor;
}
