/*
 * tls_strand.cpp — the entry half of a strand's thread_local destructors
 * (tls_strand.h). The exit half, which runs them, lives with the registry in
 * cxa_runtime.cpp.
 */

#include "box/cxx/tls_strand.h"

extern "C" void __boxcxx_thread_storage_enter()
{
    /* The thread_local destructor head (cxa_runtime.cpp) lives in this strand's
     * own thread-local storage, which the kernel made zeroed — so it is already
     * empty. No work today; the hook exists so std::thread and any future
     * per-strand init have a single defined entry point. */
}
