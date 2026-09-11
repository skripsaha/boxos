#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"


#define AML_MAX_SANE_PKG_LENGTH (1024u * 1024u)

static uint32_t decode_pkg_length(const uint8_t* aml, uint32_t* bytes_consumed) {
    uint8_t lead_byte = aml[0];
    uint8_t follow_count = (uint8_t)((lead_byte >> 6) & 0x03);

    *bytes_consumed = 1u + follow_count;

    if (follow_count == 0)
        return lead_byte & 0x3F;

    uint32_t length = lead_byte & 0x0F;
    for (uint8_t i = 0; i < follow_count; i++)
        length |= ((uint32_t)aml[1 + i]) << (4 + 8 * i);

    if (length > AML_MAX_SANE_PKG_LENGTH) {
        debug_printf("[ACPI] _S5 package length %u exceeds %u, clamping\n",
                     length, AML_MAX_SANE_PKG_LENGTH);
        length = AML_MAX_SANE_PKG_LENGTH;
    }
    return length;
}

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
            if (aml_remaining < 2) { *bytes_consumed = 1; return 0; }
            *bytes_consumed = 2;
            return aml[1];

        case AML_WORD_PREFIX:
            if (aml_remaining < 3) { *bytes_consumed = 1; return 0; }
            *bytes_consumed = 3;
            return (uint32_t)aml[1] | ((uint32_t)aml[2] << 8);

        case AML_DWORD_PREFIX:
            if (aml_remaining < 5) { *bytes_consumed = 1; return 0; }
            *bytes_consumed = 5;
            return (uint32_t)aml[1]         |
                   ((uint32_t)aml[2] <<  8) |
                   ((uint32_t)aml[3] << 16) |
                   ((uint32_t)aml[4] << 24);

        default:
            if (prefix <= 0x01) {
                *bytes_consumed = 1;
                return prefix;
            }
            *bytes_consumed = 1;
            return 0;
    }
}

bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len) {
    if (!aml) return false;

    if (aml_len < 9) return false;

    for (uint32_t i = 0; i + 8 < aml_len; i++) {
        if (aml[i] != AML_NAME_OP)
            continue;

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

        if (pos + 1 > aml_len) continue;
        uint32_t pkg_bytes = 0;
        uint32_t pkg_len = decode_pkg_length(&aml[pos], &pkg_bytes);
        (void)pkg_len;
        if (pkg_bytes == 0 || pos + pkg_bytes >= aml_len) continue;
        pos += pkg_bytes;

        if (pos >= aml_len) continue;
        uint8_t num_elements = aml[pos++];
        if (num_elements < 2) {
            debug_printf("[ACPI] _S5_ package has %u elements (<2)\n",
                         num_elements);
            continue;
        }

        uint32_t value_bytes = 0;
        uint32_t a_val = extract_integer_value(&aml[pos], aml_len - pos,
                                               &value_bytes);
        if (value_bytes == 0 || pos + value_bytes >= aml_len) continue;
        pos += value_bytes;

        uint32_t b_val = extract_integer_value(&aml[pos], aml_len - pos,
                                               &value_bytes);
        if (value_bytes == 0) continue;

        g_acpi.slp_typa = (uint16_t)(a_val & 0x07);
        g_acpi.slp_typb = (uint16_t)(b_val & 0x07);
        g_acpi.s5_found = true;

        debug_printf("[ACPI] _S5_ extracted: SLP_TYPa=0x%x SLP_TYPb=0x%x\n",
                     g_acpi.slp_typa, g_acpi.slp_typb);
        return true;
    }

    return false;
}