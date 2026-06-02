/*
 * BoxOS — UEFI Wakeup Time bridge (UEFI 2.10 §8.3.2-3).
 *
 * GetWakeupTime / SetWakeupTime program the platform's wake-alarm RTC
 * line — the same hardware that fires the ACPI wake-from-S5 event after
 * a scheduled interval. On real hardware this is backed by the legacy
 * 0x70/0x71 RTC chip in conjunction with the chipset's RTC_ALARM logic
 * (Intel PCH "Wake from RTC Alarm" enable bit in PMC_SS_PM_CFG / SLP_S3
 * generation gate, mediated by firmware behind the SetWakeupTime ABI).
 *
 * Why a kernel wrapper instead of writing CMOS directly:
 *   - Multiple OEM SKUs route the alarm through ME/PSP firmware policy,
 *     which the legacy CMOS path bypasses → "set" appears to succeed
 *     but firmware overrides on the next AC cycle.
 *   - UEFI spec mandates firmware honour SetWakeupTime even on platforms
 *     where the wake source is on the EC or PCH PMC; the firmware knows
 *     the right gates.
 *
 * Per UEFI 2.10 §8.3 "if the platform does not support a wakeup timer,
 * the SetWakeupTime function should return EFI_UNSUPPORTED" — we just
 * propagate that status.
 */

#include "efi.h"
#include "efi_runtime_internal.h"
#include "klib.h"
#include "touch.h"

EfiStatus efi_get_wakeup_time(uint8_t *enabled, uint8_t *pending, EfiTime *time)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->get_wakeup_time) return EFI_STATUS_UNSUPPORTED;

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    uint8_t en_local = 0, pend_local = 0;
    EfiTime t_local = {0};
    EfiStatus s = rt->get_wakeup_time(&en_local, &pend_local, &t_local);
    efi_rt_watch_end("GetWakeupTime", t0);

    if (enabled) *enabled = en_local;
    if (pending) *pending = pend_local;
    if (time)    *time    = t_local;

    efi_rt_unlock(rflags);
    return s;
}

EfiStatus efi_set_wakeup_time(uint8_t enabled, EfiTime *time)
{
    EfiRuntimeServices *rt = efi_rt_get();
    if (!rt || !rt->set_wakeup_time) return EFI_STATUS_UNSUPPORTED;

    /* When disabling, UEFI 2.10 §8.3.3: "If Enable is FALSE then Time
     * is ignored and the wakeup alarm is cleared." We still pass `time`
     * through as some firmware checks it for validity even when
     * disabling — being explicit costs nothing. */
    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiStatus s = rt->set_wakeup_time(enabled, time);
    efi_rt_watch_end("SetWakeupTime", t0);
    efi_rt_unlock(rflags);

    /* Publish Touch event so userspace power-policy daemons can confirm
     * that the alarm has actually armed. Some firmware silently fails
     * SetWakeupTime when the requested time is in the past — surface
     * the firmware-returned status alongside the requested datetime. */
    struct {
        uint8_t  enabled;
        uint8_t  _pad[3];
        EfiTime  requested;
        uint64_t status;
    } __attribute__((packed)) ev = {0};
    ev.enabled  = enabled;
    if (time) ev.requested = *time;
    ev.status   = (uint64_t)s;

    TouchPublish(EFI_IS_ERROR(s) ? "efi:wakeup:set-fail"
                                  : (enabled ? "efi:wakeup:armed"
                                              : "efi:wakeup:disarmed"),
                 &ev, sizeof(ev));
    return s;
}
