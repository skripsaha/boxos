
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

    uint64_t rflags, t0;
    efi_rt_lock(&rflags);
    efi_rt_watch_start(&t0);

    EfiStatus s = rt->set_wakeup_time(enabled, time);
    efi_rt_watch_end("SetWakeupTime", t0);
    efi_rt_unlock(rflags);

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