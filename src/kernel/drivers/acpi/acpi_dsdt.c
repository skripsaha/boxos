#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"

// AML package length encoding (ACPI spec 20.2.4):
// bits 7-6 of lead byte: follow byte count (0=1 byte total, 1=2, 2=3, 3=4)
// bits 5-0 of lead byte: low nibble of length
// following bytes: high nibbles
#define AML_MAX_SANE_PKG_LENGTH (1024 * 1024)  // 1MB sanity limit

uint32_t decode_pkg_length(uint8_t* aml, uint32_t* bytes_consumed) {
    uint8_t lead_byte = aml[0];
    uint8_t follow_count = (lead_byte >> 6) & 0x03;

    *bytes_consumed = 1 + follow_count;

    if (follow_count == 0) {
        return lead_byte & 0x3F;
    }

    uint32_t length = lead_byte & 0x0F;

    for (uint8_t i = 0; i < follow_count; i++) {
        length |= ((uint32_t)aml[1 + i]) << (4 + 8 * i);
    }

    if (length > AML_MAX_SANE_PKG_LENGTH) {
        debug_printf("[ACPI] WARNING: Package length %u exceeds sanity limit (%u), clamping\n",
                     length, AML_MAX_SANE_PKG_LENGTH);
        length = AML_MAX_SANE_PKG_LENGTH;
    }

    return length;
}

// AML integer encoding (ACPI spec 20.2.3):
// 0x00-0x01: ZeroOp/OneOp (1 byte)
// 0x0A: BytePrefix + data (2 bytes)
// 0x0B: WordPrefix + data (3 bytes, little-endian)
// 0x0C: DWordPrefix + data (5 bytes, little-endian)
uint32_t extract_integer_value(uint8_t* aml, uint32_t* bytes_consumed) {
    uint8_t prefix = aml[0];

    switch (prefix) {
        case AML_BYTE_PREFIX:
            *bytes_consumed = 2;
            return aml[1];

        case AML_WORD_PREFIX:
            *bytes_consumed = 3;
            return aml[1] | (aml[2] << 8);

        case AML_DWORD_PREFIX:
            *bytes_consumed = 5;
            return aml[1] | (aml[2] << 8) | (aml[3] << 16) | (aml[4] << 24);

        default:
            if (prefix <= 0x01) {
                *bytes_consumed = 1;
                return prefix;
            }
            *bytes_consumed = 1;
            return 0;
    }
}

typedef struct {
    uint16_t slp_typa;
    uint16_t slp_typb;
} s5_fallback_t;

static s5_fallback_t s5_fallback_table[] = {
    {0x05, 0x05},
    {0x00, 0x00},
    {0x07, 0x07},
    {0x03, 0x03}
};

acpi_error_t acpi_extract_s5(uint32_t dsdt_addr, uint32_t dsdt_len) {
    uint8_t* dsdt = (uint8_t*)acpi_map_physical(dsdt_addr, dsdt_len);
    if (!dsdt) {
        debug_printf("[ACPI] Failed to map DSDT\n");
        return ACPI_ERR_MAP_FAILED;
    }

    uint8_t* aml_start = dsdt + sizeof(acpi_sdt_header_t);
    uint32_t aml_len = dsdt_len - sizeof(acpi_sdt_header_t);

    debug_printf("[ACPI] Parsing DSDT AML (%u bytes)...\n", aml_len);

    for (uint32_t i = 0; i < aml_len - 8; i++) {
        if (aml_start[i] == AML_NAME_OP &&
            aml_start[i + 1] == '_' &&
            aml_start[i + 2] == 'S' &&
            aml_start[i + 3] == '5' &&
            aml_start[i + 4] == '_') {

            debug_printf("[ACPI] Found _S5_ object at offset %u\n", i);

            uint32_t pos = i + 5;

            if (aml_start[pos] != AML_PACKAGE_OP) {
                debug_printf("[ACPI] _S5_ is not a package (opcode 0x%02x)\n", aml_start[pos]);
                continue;
            }

            pos++;

            // Worst case: decode_pkg_length(4) + num_elements(1) + 2*extract_integer(5*2) = 15 bytes
            if (pos + 15 >= aml_len) {
                debug_printf("[ACPI] WARNING: _S5_ package truncated (insufficient buffer space)\n");
                continue;
            }

            uint32_t pkg_bytes;
            uint32_t pkg_len __unused = decode_pkg_length(&aml_start[pos], &pkg_bytes);
            pos += pkg_bytes;

            uint8_t num_elements = aml_start[pos++];

            if (num_elements < 2) {
                debug_printf("[ACPI] _S5_ package has only %u elements\n", num_elements);
                continue;
            }

            uint32_t value_bytes;
            g_acpi.slp_typa = extract_integer_value(&aml_start[pos], &value_bytes);
            pos += value_bytes;

            g_acpi.slp_typb = extract_integer_value(&aml_start[pos], &value_bytes);

            g_acpi.s5_found = true;

            debug_printf("[ACPI] _S5_ extracted: SLP_TYPa=0x%x, SLP_TYPb=0x%x\n",
                         g_acpi.slp_typa, g_acpi.slp_typb);

            return ACPI_OK;
        }
    }

    debug_printf("[ACPI] _S5_ not found in DSDT, using fallback\n");

    s5_fallback_t* fallback = &s5_fallback_table[0];
    g_acpi.slp_typa = fallback->slp_typa;
    g_acpi.slp_typb = fallback->slp_typb;
    g_acpi.s5_found = false;

    debug_printf("[ACPI] Using fallback: SLP_TYPa=0x%x, SLP_TYPb=0x%x\n",
                 g_acpi.slp_typa, g_acpi.slp_typb);

    return ACPI_OK;
}
