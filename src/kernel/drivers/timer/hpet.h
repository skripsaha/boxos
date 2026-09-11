#ifndef HPET_H
#define HPET_H

#include "ktypes.h"


#define HPET_REG_GCAP_ID         0x000
#define HPET_REG_GCONF           0x010
#define HPET_REG_GINTR_STA       0x020
#define HPET_REG_MCNT            0x0F0
#define HPET_REG_TIMER_CNF(n)    (0x100 + (uint32_t)(n) * 0x20)
#define HPET_REG_TIMER_CMP(n)    (0x108 + (uint32_t)(n) * 0x20)
#define HPET_REG_TIMER_FSB(n)    (0x110 + (uint32_t)(n) * 0x20)

#define HPET_GCAP_REV_ID_MASK    0xFFu
#define HPET_GCAP_NUM_TIM_SHIFT  8
#define HPET_GCAP_NUM_TIM_MASK   0x1F
#define HPET_GCAP_COUNT_SIZE_CAP (1u << 13)
#define HPET_GCAP_LEG_RT_CAP     (1u << 15)
#define HPET_GCAP_VENDOR_SHIFT   16
#define HPET_GCAP_PERIOD_SHIFT   32

#define HPET_GCONF_ENABLE        (1u << 0)
#define HPET_GCONF_LEG_RT        (1u << 1)

#define HPET_TIMER_INT_TYPE      (1u << 1)
#define HPET_TIMER_INT_ENB       (1u << 2)
#define HPET_TIMER_TYPE_PERIODIC (1u << 3)
#define HPET_TIMER_PER_INT_CAP   (1u << 4)
#define HPET_TIMER_SIZE_CAP      (1u << 5)
#define HPET_TIMER_VAL_SET       (1u << 6)
#define HPET_TIMER_32BIT_MODE    (1u << 8)
#define HPET_TIMER_INT_ROUTE_S   9
#define HPET_TIMER_FSB_EN        (1u << 14)
#define HPET_TIMER_FSB_CAP       (1u << 15)

bool hpet_init(void);
bool hpet_is_present(void);

bool hpet_start_legacy_tick(uint32_t hz);

bool hpet_tick_active(void);

uint64_t hpet_now_us(void);

void hpet_busy_wait_us(uint64_t us);

uint64_t hpet_period_fs(void);
uint8_t  hpet_num_comparators(void);
bool     hpet_counter_is_64bit(void);

#endif