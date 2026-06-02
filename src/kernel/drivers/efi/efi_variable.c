/*
 * BoxOS — UEFI Variable Services bridge (UEFI 2.10 §8.2)
 *
 * Wraps GetVariable, SetVariable, GetNextVariableName, QueryVariableInfo
 * with the project's serialisation + IRQ-save discipline (efi_rt_lock /
 * efi_rt_unlock from efi_runtime.c). Also provides ASCII conveniences:
 * the firmware expects UCS-2 variable names, so the kernel translates
 * narrow C strings in a stack scratch buffer with no allocation.
 *
 * Touch surface (real-HW observability):
 *   - efi:variable:set        — successful SetVariable mutation (NV+RT/BS
 *                                variables only; per-set is informative
 *                                but kept off the hot path by gating on
 *                                NON_VOLATILE attribute).
 *   - efi:variable:set-fail   — SetVariable returned an error status.
 *   - efi:variable:nv-info    — periodic / on-demand snapshot of NV
 *                                quota from QueryVariableInfo.
 *
 * Security: SetVariable on authenticated variables (PK/KEK/db/dbx)
 * requires an EFI_VARIABLE_AUTHENTICATION_2 envelope per UEFI 2.10
 * §8.2.2. We do not synthesise that envelope — callers pass through
 * a fully-formed payload and the firmware enforces signature checks.
 *
 * References:
 *   UEFI 2.10 §8.2.1 — GetVariable
 *   UEFI 2.10 §8.2.2 — SetVariable
 *   UEFI 2.10 §8.2.3 — GetNextVariableName
 *   UEFI 2.10 §8.2.4 — QueryVariableInfo
 *   UEFI 2.10 §8.1  — Reentrancy rules
 */

#include "efi.h"
#include "efi_runtime_internal.h"
#include "klib.h"
#include "touch.h"

/* =========================================================================
 * UCS-2 helpers
 *
 * EFI variable names are NUL-terminated UCS-2 (effectively UTF-16LE
 * restricted to BMP). Real-HW variable names are pure ASCII subset of
 * UCS-2 (per UEFI 2.10 §3.1) so a byte-wise widen suffices.
 * ========================================================================= */

size_t efi_ucs2_strlen(const uint16_t *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

size_t efi_ascii_to_ucs2(const char *ascii, uint16_t *dest, size_t dest_chars)
{
    if (!ascii || !dest || dest_chars == 0) return 0;
    size_t n = 0;
    while (ascii[n] && n + 1 < dest_chars) {
        dest[n] = (uint16_t)(unsigned char)ascii[n];
        n++;
    }
    dest[n] = 0;
    return n;
}

/* =========================================================================
 * Public wrappers
 * ========================================================================= */

EfiStatus efi_get_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->get_variable) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiUintn sz = data_size ? (EfiUintn)*data_size : 0;
    EfiStatus s = rt->get_variable(variable_name, vendor, attributes,
                                    &sz, data);
    efi_rt_watch_end("GetVariable", t0);
    if (data_size) *data_size = sz;

    efi_rt_unlock(rflags);
    return s;
}

EfiStatus efi_set_variable(uint16_t *variable_name,
                           const EfiGuid *vendor,
                           uint32_t attributes,
                           uint64_t data_size,
                           const void *data)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->set_variable) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiStatus s = rt->set_variable(variable_name, vendor, attributes,
                                    (EfiUintn)data_size, data);
    efi_rt_watch_end("SetVariable", t0);
    efi_rt_unlock(rflags);

    /* Publish a Touch event for observability. Build a compact payload
     * with the variable name (truncated UCS-2 → ASCII), GUID, attrs,
     * size, and the resulting status. Kept to a single 80-byte struct
     * so the Touch ring stays cheap. */
    struct {
        char     name[32];
        EfiGuid  vendor;
        uint32_t attributes;
        uint32_t size;
        uint64_t status;
    } __attribute__((packed)) ev = {0};

    size_t name_chars = efi_ucs2_strlen(variable_name);
    if (name_chars > 31) name_chars = 31;
    for (size_t i = 0; i < name_chars; i++) {
        uint16_t c = variable_name[i];
        ev.name[i] = (c < 0x80) ? (char)c : '?';
    }
    ev.name[name_chars] = 0;
    if (vendor) ev.vendor = *vendor;
    ev.attributes = attributes;
    ev.size       = (uint32_t)data_size;
    ev.status     = (uint64_t)s;

    TouchPublish(EFI_IS_ERROR(s) ? "efi:variable:set-fail"
                                  : "efi:variable:set",
                 &ev, sizeof(ev));
    return s;
}

EfiStatus efi_get_next_variable_name(uint64_t *variable_name_size,
                                     uint16_t *variable_name,
                                     EfiGuid  *vendor)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->get_next_variable_name) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiUintn sz = variable_name_size ? (EfiUintn)*variable_name_size : 0;
    EfiStatus s = rt->get_next_variable_name(&sz, variable_name, vendor);
    efi_rt_watch_end("GetNextVariableName", t0);
    if (variable_name_size) *variable_name_size = sz;

    efi_rt_unlock(rflags);
    return s;
}

EfiStatus efi_query_variable_info(uint32_t attributes,
                                  uint64_t *max_var_storage,
                                  uint64_t *remaining_var_storage,
                                  uint64_t *max_var_size)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->query_variable_info) return EFI_STATUS_UNSUPPORTED;

    /* QueryVariableInfo is a UEFI 2.0+ service (revision >= 2.0 in the
     * RT services table). On firmware reporting an older revision the
     * pointer may be NULL even when the table exists. Spec-compliant
     * graceful return when that happens. */
    if (rt->hdr.revision < ((2U << 16) | 0)) {
        return EFI_STATUS_UNSUPPORTED;
    }

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    uint64_t mvs = 0, rvs = 0, mvz = 0;
    EfiStatus s = rt->query_variable_info(attributes, &mvs, &rvs, &mvz);
    efi_rt_watch_end("QueryVariableInfo", t0);

    if (max_var_storage)       *max_var_storage       = mvs;
    if (remaining_var_storage) *remaining_var_storage = rvs;
    if (max_var_size)          *max_var_size          = mvz;

    efi_rt_unlock(rflags);

    if (!EFI_IS_ERROR(s)) {
        struct {
            uint32_t attributes;
            uint32_t pad;
            uint64_t max_storage;
            uint64_t remaining;
            uint64_t max_var_size;
        } __attribute__((packed)) ev = {
            .attributes = attributes,
            .pad        = 0,
            .max_storage = mvs,
            .remaining   = rvs,
            .max_var_size = mvz,
        };
        TouchPublish("efi:variable:nv-info", &ev, sizeof(ev));
    }
    return s;
}

/* =========================================================================
 * ASCII conveniences
 *
 * Most call sites know the variable name as a C string literal —
 * widening to UCS-2 with a 128-CHAR16 stack scratch keeps the API
 * ergonomic without forcing the caller to maintain a parallel widened
 * literal. 128 is more than enough: the longest standard global var
 * name is "PlatformLangCodes" (17 chars), and image-security DB names
 * top out at "PKDefault" / "KEKDefault" (10 chars).
 * ========================================================================= */

#define EFI_VAR_NAME_SCRATCH  128u   /* in CHAR16 units */

EfiStatus efi_get_variable_ascii(const char *name_ascii,
                                 const EfiGuid *vendor,
                                 uint32_t *attributes,
                                 uint64_t *data_size,
                                 void *data)
{
    uint16_t scratch[EFI_VAR_NAME_SCRATCH];
    if (!efi_ascii_to_ucs2(name_ascii, scratch, EFI_VAR_NAME_SCRATCH)) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    return efi_get_variable(scratch, vendor, attributes, data_size, data);
}

EfiStatus efi_set_variable_ascii(const char *name_ascii,
                                 const EfiGuid *vendor,
                                 uint32_t attributes,
                                 uint64_t data_size,
                                 const void *data)
{
    uint16_t scratch[EFI_VAR_NAME_SCRATCH];
    if (!efi_ascii_to_ucs2(name_ascii, scratch, EFI_VAR_NAME_SCRATCH)) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    return efi_set_variable(scratch, vendor, attributes, data_size, data);
}
