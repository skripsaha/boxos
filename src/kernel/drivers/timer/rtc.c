#include "rtc.h"
#include "io.h"
#include "pit.h"

#define CMOS_ADDR       0x70
#define CMOS_DATA       0x71

#define CMOS_REG_SEC    0x00
#define CMOS_REG_MIN    0x02
#define CMOS_REG_HOUR   0x04
#define CMOS_REG_WDAY   0x06
#define CMOS_REG_DAY    0x07
#define CMOS_REG_MON    0x08
#define CMOS_REG_YEAR   0x09
#define CMOS_REG_STA    0x0A
#define CMOS_REG_STB    0x0B
#define CMOS_REG_CENT   0x32

#define STA_UIP         0x80
#define STB_BIN         0x04
#define STB_24H         0x02

typedef struct {
    uint64_t base_seconds;
    uint64_t base_pit_ticks;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
} rtc_state_t;

static rtc_state_t rtc_state;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, 0x80 | reg);
    return inb(CMOS_DATA);
}

static void wait_not_uip(void) {
    while (cmos_read(CMOS_REG_STA) & STA_UIP)
        ;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint64_t days_since_epoch(uint16_t y, uint8_t m, uint8_t d) {
    uint32_t yy = y, mm = m;
    if (mm <= 2) { yy--; mm += 9; } else { mm -= 3; }
    uint64_t era = yy / 400;
    uint32_t yoe = yy - (uint32_t)(era * 400);
    uint32_t doy = (153 * mm + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468ULL;
}

static uint64_t calendar_to_unix(uint16_t y, uint8_t mo, uint8_t d,
                                  uint8_t h, uint8_t mi, uint8_t s) {
    return days_since_epoch(y, mo, d) * 86400ULL
         + (uint64_t)h * 3600
         + (uint64_t)mi * 60
         + s;
}

void rtc_init(void) {
    wait_not_uip();

    uint8_t sec  = cmos_read(CMOS_REG_SEC);
    uint8_t min  = cmos_read(CMOS_REG_MIN);
    uint8_t hour = cmos_read(CMOS_REG_HOUR);
    uint8_t wday = cmos_read(CMOS_REG_WDAY);
    uint8_t day  = cmos_read(CMOS_REG_DAY);
    uint8_t mon  = cmos_read(CMOS_REG_MON);
    uint8_t year = cmos_read(CMOS_REG_YEAR);
    uint8_t cent = cmos_read(CMOS_REG_CENT);
    uint8_t stb  = cmos_read(CMOS_REG_STB);

    wait_not_uip();

    uint8_t sec2  = cmos_read(CMOS_REG_SEC);
    uint8_t min2  = cmos_read(CMOS_REG_MIN);
    uint8_t hour2 = cmos_read(CMOS_REG_HOUR);
    uint8_t wday2 = cmos_read(CMOS_REG_WDAY);
    uint8_t day2  = cmos_read(CMOS_REG_DAY);
    uint8_t mon2  = cmos_read(CMOS_REG_MON);
    uint8_t year2 = cmos_read(CMOS_REG_YEAR);

    if (sec2 != sec || min2 != min || hour2 != hour ||
        wday2 != wday || day2 != day || mon2 != mon || year2 != year) {
        sec  = sec2;
        min  = min2;
        hour = hour2;
        wday = wday2;
        day  = day2;
        mon  = mon2;
        year = year2;
    }

    bool binary_mode = (stb & STB_BIN) != 0;
    bool mode_24h    = (stb & STB_24H) != 0;

    if (!binary_mode) {
        bool pm = (!mode_24h) && (hour & 0x80);
        hour = bcd_to_bin(hour & 0x7F);
        if (!mode_24h) {
            if (pm && hour != 12) hour += 12;
            if (!pm && hour == 12) hour = 0;
        }
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        wday = bcd_to_bin(wday);
    } else {
        if (!mode_24h) {
            bool pm = (hour & 0x80) != 0;
            hour &= 0x7F;
            if (pm && hour != 12) hour += 12;
            if (!pm && hour == 12) hour = 0;
        }
    }

    uint8_t century = (cent != 0) ? cent : 20;
    if (!binary_mode && cent != 0) {
        century = bcd_to_bin(cent);
    }

    uint16_t full_year = (uint16_t)century * 100 + year;

    if (wday == 0) wday = 7;
    uint8_t weekday_0sun = wday - 1;

    rtc_state.year    = full_year;
    rtc_state.month   = mon;
    rtc_state.day     = day;
    rtc_state.hour    = hour;
    rtc_state.minute  = min;
    rtc_state.second  = sec;
    rtc_state.weekday = weekday_0sun;

    rtc_state.base_seconds  = calendar_to_unix(full_year, mon, day, hour, min, sec);
    rtc_state.base_pit_ticks = pit_get_ticks();

    outb(CMOS_ADDR, 0x00);

    debug_printf("[RTC] Initialized: %04u-%02u-%02u %02u:%02u:%02u (unix=%llu)\n",
                 full_year, mon, day, hour, min, sec,
                 rtc_state.base_seconds);
}

void rtc_get_boxtime(time_t* out) {
    out->year    = rtc_state.year;
    out->month   = rtc_state.month;
    out->day     = rtc_state.day;
    out->hour    = rtc_state.hour;
    out->minute  = rtc_state.minute;
    out->second  = rtc_state.second;
    out->weekday = rtc_state.weekday;
    out->seconds = rtc_state.base_seconds;
    out->nanosec = 0;
}

uint64_t rtc_get_unix64(void) {
    uint32_t freq = pit_get_frequency();
    if (freq == 0) {
        return rtc_state.base_seconds;
    }
    uint64_t elapsed_ticks = pit_get_ticks() - rtc_state.base_pit_ticks;
    return rtc_state.base_seconds + elapsed_ticks / freq;
}

uint64_t rtc_get_uptime_ns(void) {
    uint32_t freq = pit_get_frequency();
    if (freq == 0) {
        return 0;
    }
    uint64_t ticks = pit_get_ticks() - rtc_state.base_pit_ticks;
    uint64_t secs = ticks / freq;
    uint64_t sub  = ticks % freq;
    return secs * 1000000000ULL + (sub * 1000000000ULL) / freq;
}
