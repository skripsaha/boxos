#ifndef TSC_SYNC_H
#define TSC_SYNC_H

#include "ktypes.h"

/*
 * Every core's TSC reads the BSP's — the kernel compares TSC values taken on
 * different cores everywhere (the Waterline, deadlines, Nightwatch, a K-Core's
 * stamp against an App Core's).
 *
 * Cores of one package count from one reset and agree by themselves, unless
 * IA32_TSC_ADJUST says otherwise; so an AP first takes the BSP's
 * IA32_TSC_ADJUST. Then the AP and the BSP meet: they hand a word back and
 * forth, each reading its TSC as it sees the other's, and the round that
 * came back soonest says how far the AP stands from the BSP — to within half
 * that round. The AP corrects only a difference larger than the whole round,
 * and meets once more to see what is left. A machine without IA32_TSC_ADJUST
 * is told about, not corrected.
 *
 * It used to compute what the AP's TSC should read from the HPET's time since
 * the BSP's mark and the TSC's calibrated rate, and to write the difference
 * through IA32_TSC_ADJUST past 4 µs — the HPET and the TSC run off two
 * crystals tens of millionths apart, so a second's drift between them was
 * written into TSCs that agreed (i5-9400F: the App Cores 11 µs behind the
 * K-Core).
 */

/* The BSP, before the first AP is woken: its own IA32_TSC_ADJUST, the one
 * every AP takes. */
void TscSyncRecordBsp(void);

/* The BSP, while it waits for AP `core_index` to come online: serves the
 * meeting that AP asks for, if it asks now; returns at once otherwise. */
void TscSyncServe(uint8_t core_index);

/* AP `core_index`, interrupts off, before it says it is online. */
void TscSyncAp(uint8_t core_index);

#endif /* TSC_SYNC_H */
