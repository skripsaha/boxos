#ifndef PMTIMER_H
#define PMTIMER_H

#include "ktypes.h"
#include "acpi.h"


#define PMTIMER_FREQ_HZ  3579545u

bool pmtimer_adopt_from_fadt(const acpi_fadt_t *fadt);

bool pmtimer_is_present(void);

bool pmtimer_read(uint32_t *out);

uint32_t pmtimer_mask(void);

bool pmtimer_busy_wait_us(uint64_t us);

#endif