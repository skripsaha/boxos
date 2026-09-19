#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"

/*
 * BoxOS mini-AML scanner for _S5_.
 *
 * A full AML interpreter (uACPI/ACPICA) is the right long-term answer — _S5
 * may live behind If/Else, may reference DerefOf, etc. This scanner handles
 * the overwhelmingly common case: a literal `Name (_S5_, Package () { ... })`
 * in either the DSDT or any loaded SSDT body. ACPI 6.5 §5.2.11.2 explicitly
 * permits _S5 to be defined in an SSDT, which is why this function takes a
 * raw AML buffer rather than reaching into the DSDT directly.
 *
 * False positives (the byte sequence `\_S5_` showing up inside a Buffer or
 * String literal) are improbable in practice and would fail the subsequent
 * PackageOp check, returning false instead of programming bogus PM1 values.
 */

/*
 * AML package length encoding — ACPI 6.5 §20.2.4.
 *
 * `*bytes_consumed == 0` means "this is not a package length that fits in
 * what is left of the table", and the caller must treat it as a refusal.
 *
 * What stood here was a 1 MiB ceiling with "sanity only" written beside it,
 * and a malformed length was CLAMPED to it — a number from nobody, applied
 * on the live path that decides how this machine powers off. The bound is
 * not a number to pick: §20.2.4 has the PkgLength count from its own first
 * byte, and the table says how long it is in its own header, so a package
 * that claims to run past the end of the bytes handed to this scanner is
 * malformed and there is nothing to salvage by shortening it. `remaining` is
 * that fact, carried in from the table header.
 */
static uint32_t decode_pkg_length(const uint8_t* aml, uint32_t remaining,
                                   uint32_t* bytes_consumed) {
    if (remaining == 0) { *bytes_consumed = 0; return 0; }

    uint8_t lead_byte = aml[0];
    uint8_t follow_count = (uint8_t)((lead_byte >> 6) & 0x03);
    if ((uint32_t)follow_count + 1u > remaining) { *bytes_consumed = 0; return 0; }

    uint32_t length;
    if (follow_count == 0) {
        length = lead_byte & 0x3F;
    } else {
        length = lead_byte & 0x0F;
        for (uint8_t i = 0; i < follow_count; i++)
            length |= ((uint32_t)aml[1 + i]) << (4 + 8 * i);
    }

    if (length > remaining) {
        debug_printf("[ACPI] _S5 package claims %u bytes with %u left in the "
                     "table — rejected, not shortened\n", length, remaining);
        *bytes_consumed = 0;
        return 0;
    }
    *bytes_consumed = 1u + follow_count;
    return length;
}

/* AML integer encoding — ACPI 6.5 §20.2.3.
 *
 * `*bytes_consumed == 0` means "this is not a constant integer". The
 * caller must treat that as a refusal: a fabricated 0 here would become
 * a fabricated SLP_TYP written to PM1a_CNT. */
static uint32_t extract_integer_value(const uint8_t* aml,
                                       uint32_t aml_remaining,
                                       uint32_t* bytes_consumed) {
    if (aml_remaining == 0) {
        *bytes_consumed = 0;
        return 0;
    }
    uint8_t prefix = aml[0];

    switch (prefix) {
        case AML_BYTE_PREFIX:
            if (aml_remaining < 2) { *bytes_consumed = 0; return 0; }
            *bytes_consumed = 2;
            return aml[1];

        case AML_WORD_PREFIX:
            if (aml_remaining < 3) { *bytes_consumed = 0; return 0; }
            *bytes_consumed = 3;
            return (uint32_t)aml[1] | ((uint32_t)aml[2] << 8);

        case AML_DWORD_PREFIX:
            if (aml_remaining < 5) { *bytes_consumed = 0; return 0; }
            *bytes_consumed = 5;
            return (uint32_t)aml[1]         |
                   ((uint32_t)aml[2] <<  8) |
                   ((uint32_t)aml[3] << 16) |
                   ((uint32_t)aml[4] << 24);

        default:
            if (prefix <= 0x01) {
                /* ZeroOp (0x00) or OneOp (0x01) — single-byte literal. */
                *bytes_consumed = 1;
                return prefix;
            }
            *bytes_consumed = 0;
            return 0;
    }
}

bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len) {
    if (!aml) return false;

    /* Need at least: NameOp + (\\)? + (^*)? + "_S5_" + PackageOp + pkglen(1)
     * + numelem(1) + one minimal integer (1) = 8 bytes. That is the
     * spec form of §7.4.2 — a Package holding a single Integer. Reject
     * shorter buffers before the loop so the bound is sane. */
    if (aml_len < 8) return false;

    for (uint32_t i = 0; i + 7 < aml_len; i++) {
        if (aml[i] != AML_NAME_OP)
            continue;

        /* Skip optional NameString prefix: one RootChar (0x5C) and any
         * number of ParentPrefixChar (0x5E). */
        uint32_t name_off = i + 1;
        if (name_off < aml_len && aml[name_off] == 0x5C) name_off++;
        while (name_off < aml_len && aml[name_off] == 0x5E) name_off++;

        if (name_off + 4 >= aml_len) continue;
        if (aml[name_off + 0] != '_' ||
            aml[name_off + 1] != 'S' ||
            aml[name_off + 2] != '5' ||
            aml[name_off + 3] != '_')
            continue;

        uint32_t pos = name_off + 4;
        if (pos >= aml_len) continue;
        if (aml[pos] != AML_PACKAGE_OP) {
            debug_printf("[ACPI] _S5_ at off %u has no PackageOp (0x%02x)\n",
                         i, aml[pos]);
            continue;
        }
        pos++;

        /* The package's own length, bounded by what is left of the table —
         * and USED, where it was decoded and thrown away. A _S5 package that
         * runs past the end of the bytes this scanner was given describes
         * nothing readable, and reading its elements anyway is how a
         * fabricated SLP_TYP reaches PM1a_CNT. */
        if (pos >= aml_len) continue;
        uint32_t pkg_bytes = 0;
        uint32_t pkg_len = decode_pkg_length(&aml[pos], aml_len - pos,
                                             &pkg_bytes);
        if (pkg_bytes == 0) continue;
        const uint32_t pkg_end = pos + pkg_len;   /* §20.2.4: counts itself */
        pos += pkg_bytes;
        if (pkg_end <= pos || pkg_end > aml_len) continue;

        if (pos >= pkg_end) continue;
        uint8_t num_elements = aml[pos++];
        if (num_elements == 0) {
            debug_printf("[ACPI] _S5_ package is empty\n");
            continue;
        }

        /* Elements are read inside the package the firmware declared, not up
         * to the end of the table: a truncated package must not be completed
         * out of whatever bytes follow it. */
        uint32_t value_bytes = 0;
        uint32_t a_val = extract_integer_value(&aml[pos], pkg_end - pos,
                                               &value_bytes);
        if (value_bytes == 0) continue;
        pos += value_bytes;

        uint32_t b_val;
        if (num_elements == 1) {
            /* ACPI 6.5 §7.4.2 / Table 7.11: the _Sx package is defined as
             * ONE Integer whose byte 0 is the PM1a_CNT.SLP_TYP value and
             * byte 1 the PM1b_CNT.SLP_TYP value (bytes 2-3 reserved).
             * ACPICA's AcpiGetSleepTypeData does exactly this split in
             * its one-element case. Firmware that emits the spec form
             * used to be discarded here, leaving the shutdown path with
             * a fabricated SLP_TYP. */
            b_val = a_val >> 8;
        } else {
            /* The vendor form most BIOSes emit: one Integer per port. */
            if (pos >= pkg_end) continue;
            b_val = extract_integer_value(&aml[pos], pkg_end - pos,
                                          &value_bytes);
            if (value_bytes == 0) continue;
        }

        g_acpi.slp_typa = (uint16_t)(a_val & 0x07);   /* SLP_TYP is 3 bits */
        g_acpi.slp_typb = (uint16_t)(b_val & 0x07);
        g_acpi.s5_found = true;

        debug_printf("[ACPI] _S5_ extracted: SLP_TYPa=0x%x SLP_TYPb=0x%x\n",
                     g_acpi.slp_typa, g_acpi.slp_typb);
        return true;
    }

    return false;
}
