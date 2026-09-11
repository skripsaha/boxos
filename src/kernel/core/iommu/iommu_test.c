
#include "iommu.h"
#include "klib.h"

#define IT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[IOMMU TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void IommuPresenceTest(void) {
    kprintf("[IOMMU TEST] Starting IOMMU presence test...\n");
    size_t pass = 0, fail = 0;

    bool present = iommu_present();
    IT_CHECK(present == (iommu_get_ops() != NULL),
             "iommu_present matches g_ops state");

    IT_CHECK(iommu_domain_id(NULL) == 0xFFFFFFFFu,
             "iommu_domain_id(NULL) returns INVALID");

    if (present) {
        iommu_domain_t *dom = iommu_domain_alloc();
        if (dom) {
            IT_CHECK(iommu_domain_id(dom) != 0xFFFFFFFFu,
                     "fresh domain has valid id");
            iommu_domain_free(dom);
        } else {
            kprintf("[IOMMU TEST]   note: domain_alloc returned NULL "
                    "(backend cap reached — OK)\n");
        }
    } else {
        kprintf("[IOMMU TEST]   note: no backend registered — IOMMU dormant "
                "(firmware tables: see the [ACPI] inventory above)\n");
    }

    if (fail == 0)
        kprintf("[IOMMU TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[IOMMU TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}