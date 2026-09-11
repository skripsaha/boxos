
#include "mce.h"
#include "klib.h"

#define MCT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[MCE TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void McePresenceTest(void) {
    kprintf("[MCE TEST] Starting MCE presence test...\n");
    size_t pass = 0, fail = 0;

    MCT_CHECK(mce_is_initialized(),
              "mce_is_initialized() == true");
    MCT_CHECK(mce_bank_count() > 0,
              "mce_bank_count() > 0 on supported CPU");

    kprintf("[MCE TEST]   LMCE state: %s\n",
            mce_lmce_enabled() ? "active" : "dormant");

    if (fail == 0)
        kprintf("[MCE TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[MCE TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}