/*
 * BoxOS — UEFI Capsule Services bridge (UEFI 2.10 §8.5.3-4).
 *
 * UpdateCapsule delivers a capsule (typically a firmware update payload)
 * to the firmware. Capsules are described by an array of
 * EFI_CAPSULE_HEADER pointers; the optional scatter-gather list lets a
 * single capsule occupy non-contiguous physical pages.
 *
 * QueryCapsuleCapabilities asks firmware ahead of time whether a given
 * capsule (or batch of them) can be processed and, if so, what reset
 * mode the platform requires to apply it.
 *
 * Real-HW considerations:
 *   - The scatter-gather list, when used, MUST live in memory that
 *     survives reset. Firmware-side ESRT-driven flows traditionally
 *     stage capsules in EfiBootServicesData allocations rolled into
 *     persistent RAM by the OS before reboot. We do not allocate
 *     capsules in this driver — callers pass in fully populated
 *     headers and SG lists, typically from an OS-update tool.
 *   - Per UEFI 2.10 §8.5.3.1, every EFI_CAPSULE_HEADER must satisfy
 *     header_size >= sizeof(EFI_CAPSULE_HEADER) and
 *     capsule_image_size >= header_size. We validate before dispatch
 *     to catch caller bugs early; firmware would otherwise return
 *     INVALID_PARAMETER without diagnostic context.
 *   - CAPSULE_FLAGS_INITIATE_RESET is honoured by firmware: if set,
 *     the call may not return (the system resets). Callers expecting
 *     to continue execution should keep that flag clear and use
 *     ResetSystem themselves after a successful UpdateCapsule.
 *
 * References:
 *   UEFI 2.10 §8.5.3.1 — EFI_CAPSULE_HEADER
 *   UEFI 2.10 §8.5.3   — UpdateCapsule
 *   UEFI 2.10 §8.5.4   — QueryCapsuleCapabilities
 *   UEFI 2.10 §8.1     — Runtime services reentrancy
 */

#include "efi.h"
#include "efi_runtime_internal.h"
#include "klib.h"
#include "touch.h"

static bool capsule_headers_valid(EfiCapsuleHeader **arr, uint64_t count)
{
    if (!arr || count == 0) return false;
    if (count > 64) return false;       /* sanity cap */

    for (uint64_t i = 0; i < count; i++) {
        EfiCapsuleHeader *h = arr[i];
        if (!h) return false;
        if (h->header_size < sizeof(EfiCapsuleHeader)) return false;
        if (h->capsule_image_size < h->header_size) return false;
        if (h->capsule_image_size > (1ULL << 30)) return false;  /* >1 GB rejected */
    }
    return true;
}

EfiStatus efi_query_capsule_capabilities(EfiCapsuleHeader **capsule_header_array,
                                         uint64_t           capsule_count,
                                         uint64_t          *maximum_capsule_size,
                                         uint32_t          *reset_type)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->query_capsule_caps) return EFI_STATUS_UNSUPPORTED;
    /* UEFI 2.0+: skip on rev < 2.0. */
    if (rt->hdr.revision < ((2U << 16) | 0)) return EFI_STATUS_UNSUPPORTED;

    if (!capsule_headers_valid(capsule_header_array, capsule_count)) {
        return EFI_STATUS_INVALID_PARAMETER;
    }

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    uint64_t max_sz = 0;
    uint32_t rtype  = 0;
    EfiStatus s = rt->query_capsule_caps(capsule_header_array,
                                          (EfiUintn)capsule_count,
                                          &max_sz, &rtype);
    efi_rt_watch_end("QueryCapsuleCapabilities", t0);
    efi_rt_unlock(rflags);

    if (maximum_capsule_size) *maximum_capsule_size = max_sz;
    if (reset_type)           *reset_type           = rtype;
    return s;
}

EfiStatus efi_update_capsule(EfiCapsuleHeader **capsule_header_array,
                             uint64_t          capsule_count,
                             uint64_t          scatter_gather_list)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->update_capsule) return EFI_STATUS_UNSUPPORTED;
    if (rt->hdr.revision < ((2U << 16) | 0)) return EFI_STATUS_UNSUPPORTED;

    if (!capsule_headers_valid(capsule_header_array, capsule_count)) {
        return EFI_STATUS_INVALID_PARAMETER;
    }

    /* Pre-publish a Touch event with the capsule batch metadata.
     * UpdateCapsule may not return (firmware may initiate reset on
     * CAPSULE_FLAGS_INITIATE_RESET), so emit BEFORE the call. */
    struct {
        EfiGuid  first_guid;
        uint32_t flags;
        uint32_t capsule_count;
        uint64_t total_bytes;
        uint64_t sg_list;
    } __attribute__((packed)) ev = {0};
    ev.first_guid    = capsule_header_array[0]->capsule_guid;
    ev.flags         = capsule_header_array[0]->flags;
    ev.capsule_count = (uint32_t)capsule_count;
    ev.sg_list       = scatter_gather_list;
    for (uint64_t i = 0; i < capsule_count; i++) {
        ev.total_bytes += capsule_header_array[i]->capsule_image_size;
    }
    TouchPublish("efi:capsule:armed", &ev, sizeof(ev));
    debug_printf("[EFI] UpdateCapsule: %lu capsule(s), %lu bytes, flags=0x%x, sg=0x%lx\n",
                 (unsigned long)capsule_count,
                 (unsigned long)ev.total_bytes,
                 ev.flags,
                 (unsigned long)scatter_gather_list);

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiStatus s = rt->update_capsule(capsule_header_array,
                                      (EfiUintn)capsule_count,
                                      scatter_gather_list);
    /* If we get here, firmware did not auto-reset. */
    efi_rt_watch_end("UpdateCapsule", t0);
    efi_rt_unlock(rflags);

    if (EFI_IS_ERROR(s)) {
        struct {
            uint64_t status;
        } fail_ev = { .status = (uint64_t)s };
        TouchPublish("efi:capsule:set-fail", &fail_ev, sizeof(fail_ev));
    } else {
        struct {
            uint64_t status;
        } ok_ev = { .status = 0 };
        TouchPublish("efi:capsule:delivered", &ok_ev, sizeof(ok_ev));
    }
    return s;
}
