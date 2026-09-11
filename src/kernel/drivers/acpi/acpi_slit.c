#include "acpi_internal.h"
#include "klib.h"


void acpi_parse_slit(void) {
    memset(&g_acpi.slit, 0, sizeof(g_acpi.slit));

    acpi_slit_t* slit = (acpi_slit_t*)acpi_find_table("SLIT");
    if (!slit) return;

    if (slit->locality_count == 0) {
        debug_printf("[ACPI] SLIT locality_count=0; ignoring\n");
        return;
    }
    if (slit->locality_count > ACPI_NUMA_MAX_DOMAINS) {
        debug_printf("[ACPI] SLIT locality_count=%lu > %u, truncating\n",
                     (unsigned long)slit->locality_count,
                     ACPI_NUMA_MAX_DOMAINS);
    }

    uint64_t n_full = slit->locality_count;
    uint8_t  n      = (n_full > ACPI_NUMA_MAX_DOMAINS)
                      ? ACPI_NUMA_MAX_DOMAINS
                      : (uint8_t)n_full;

    if (n_full > 0xFFFFu) {
        debug_printf("[ACPI] SLIT locality_count=%lu cannot be described by a "
                     "32-bit length — table malformed, ignoring\n",
                     (unsigned long)n_full);
        return;
    }

    uint64_t matrix_total = n_full * n_full;
    uint64_t expected = (uint64_t)sizeof(acpi_slit_t) + matrix_total;
    if ((uint64_t)slit->header.length < expected) {
        debug_printf("[ACPI] SLIT header.length=%u < required %lu\n",
                     slit->header.length, (unsigned long)expected);
        return;
    }

    uint8_t* row = (uint8_t*)slit + sizeof(acpi_slit_t);
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < n; j++) {
            g_acpi.slit.matrix[i * ACPI_NUMA_MAX_DOMAINS + j] =
                row[(uint32_t)i * (uint32_t)n_full + j];
        }
    }
    g_acpi.slit.locality_count = n;
    g_acpi.slit.present = true;

    debug_printf("[ACPI] SLIT parsed: %u localities (capped at %u)\n",
                 n, ACPI_NUMA_MAX_DOMAINS);
}

const acpi_slit_info_t *acpi_get_slit(void) {
    return g_acpi.slit.present ? &g_acpi.slit : NULL;
}