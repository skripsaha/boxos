#include "acpi_internal.h"
#include "klib.h"

/*
 * SLIT (System Locality Information Table) parser — ACPI 6.5 §5.2.17.
 *
 * SLIT is an N×N matrix of byte distances between NUMA proximity domains.
 *   matrix[i][i] = 10        (canonical local distance)
 *   matrix[i][j] > 10        (further from i to j)
 *   matrix[i][j] = 0xFF      (no path)
 *
 * BoxOS does not yet have a NUMA scheduler, but exposing the parsed matrix
 * keeps the firmware data available for the future scheduler audit
 * without re-walking the table.
 */

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

    uint32_t matrix_total = (uint32_t)slit->locality_count
                          * (uint32_t)slit->locality_count;
    uint32_t expected = (uint32_t)sizeof(acpi_slit_t) + matrix_total;
    if (slit->header.length < expected) {
        debug_printf("[ACPI] SLIT header.length=%u < required %u\n",
                     slit->header.length, expected);
        return;
    }

    /* Walk only the [0..n) × [0..n) sub-matrix to fit our cap. */
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
