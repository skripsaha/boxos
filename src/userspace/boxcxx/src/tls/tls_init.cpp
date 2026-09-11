
#include <cstdint>
#include <cstddef>

#include "box/cpu.h"
#include "box/system.h"

#include "tls_mold.h"

extern "C" {
void *malloc(size_t size);
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

[[gnu::used]] alignas(boxcxx::__tls::kAlign) thread_local char kAnchor;

static_assert(__alignof__(kAnchor) == boxcxx::__tls::kAlign,
              "kAnchor must stay alignas(kAlign): it is what pins PT_TLS p_align");

struct TlsControlBlock {
    void *self;
    void *reserved;
};

}

extern "C" void __boxcxx_tls_bootstrap()
{
    namespace tls = boxcxx::__tls;

    const tls::Mold mold = tls::Take();
    const uint64_t total = mold.below_tcb + sizeof(TlsControlBlock);

    void *raw = malloc(total + tls::kAlign);
    if (!raw) boxcxx::Panic("TLS bootstrap: heap exhausted");

    uintptr_t block = tls::AlignUp((uintptr_t)raw, tls::kAlign);

    tls::Pour((void *)block, mold);

    TlsControlBlock *tcb = (TlsControlBlock *)(block + mold.below_tcb);
    tcb->self     = tcb;
    tcb->reserved = nullptr;

    if (cpu_has_fsgsbase()) {
        __asm__ volatile("wrfsbase %0" :: "r"(tcb));
    } else {
        if (tls_set_fsbase((uint64_t)(uintptr_t)tcb) != 0)
            boxcxx::Panic("TLS bootstrap: SYSTEM_OP_TLS_FSBASE failed");
        yield();
    }

    kAnchor = 1;
}

[[gnu::constructor(101)]] static void BoxCxxTlsCtor()
{
    __boxcxx_tls_bootstrap();
}