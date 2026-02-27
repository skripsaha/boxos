#include "box/time.h"
#include "box/notify.h"
#include "box/result.h"

#define OPCODE_RTC_GET_TIME   0x15
#define OPCODE_RTC_GET_UNIX64 0x16
#define OPCODE_RTC_GET_UPTIME 0x17
#define OPCODE_TIMER_GET_MS   0x11

static uint64_t read_u64_be(const uint8_t* p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] <<  8) |  (uint64_t)p[7];
}

static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int time_get(time_t* out) {
    if (!out) return BOX_ERR_NULL_POINTER;

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, OPCODE_RTC_GET_TIME);
    event_id_t eid = notify_execute();
    if (eid == 0) return BOX_ERR_UNKNOWN;

    result_entry_t result;
    if (!result_wait(&result, 50000)) return BOX_ERR_TIMEOUT;
    if (result.error_code != BOX_OK) return (int)result.error_code;

    const uint8_t* p = result.payload;
    out->seconds = read_u64_be(p + 0);
    out->nanosec = read_u32_be(p + 8);
    out->year    = read_u16_be(p + 12);
    out->month   = p[14];
    out->day     = p[15];
    out->hour    = p[16];
    out->minute  = p[17];
    out->second  = p[18];
    out->weekday = p[19];
    return BOX_OK;
}

int time_get_secs(uint64_t* out_seconds) {
    if (!out_seconds) return BOX_ERR_NULL_POINTER;

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, OPCODE_RTC_GET_UNIX64);
    event_id_t eid = notify_execute();
    if (eid == 0) return BOX_ERR_UNKNOWN;

    result_entry_t result;
    if (!result_wait(&result, 50000)) return BOX_ERR_TIMEOUT;
    if (result.error_code != BOX_OK) return (int)result.error_code;

    *out_seconds = read_u64_be(result.payload);
    return BOX_OK;
}

int time_uptime_ms(uint64_t* out_ms) {
    if (!out_ms) return BOX_ERR_NULL_POINTER;

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, OPCODE_TIMER_GET_MS);
    event_id_t eid = notify_execute();
    if (eid == 0) return BOX_ERR_UNKNOWN;

    result_entry_t result;
    if (!result_wait(&result, 50000)) return BOX_ERR_TIMEOUT;
    if (result.error_code != BOX_OK) return (int)result.error_code;

    *out_ms = read_u64_be(result.payload);
    return BOX_OK;
}

int time_uptime_ns(uint64_t* out_ns) {
    if (!out_ns) return BOX_ERR_NULL_POINTER;

    notify_prepare();
    notify_add_prefix(BOX_DECK_HARDWARE, OPCODE_RTC_GET_UPTIME);
    event_id_t eid = notify_execute();
    if (eid == 0) return BOX_ERR_UNKNOWN;

    result_entry_t result;
    if (!result_wait(&result, 50000)) return BOX_ERR_TIMEOUT;
    if (result.error_code != BOX_OK) return (int)result.error_code;

    *out_ns = read_u64_be(result.payload);
    return BOX_OK;
}

static void write2(char* buf, uint8_t v) {
    buf[0] = (char)('0' + v / 10);
    buf[1] = (char)('0' + v % 10);
}

static void write4(char* buf, uint16_t v) {
    buf[0] = (char)('0' + v / 1000);
    buf[1] = (char)('0' + (v / 100) % 10);
    buf[2] = (char)('0' + (v / 10) % 10);
    buf[3] = (char)('0' + v % 10);
}

int time_format(const time_t* t, char* buf, size_t buf_size) {
    if (!t || !buf) return BOX_ERR_NULL_POINTER;
    if (buf_size < 20) return BOX_ERR_BUFFER_TOO_SMALL;

    write4(buf, t->year);
    buf[4] = '-';
    write2(buf + 5, t->month);
    buf[7] = '-';
    write2(buf + 8, t->day);
    buf[10] = ' ';
    write2(buf + 11, t->hour);
    buf[13] = ':';
    write2(buf + 14, t->minute);
    buf[16] = ':';
    write2(buf + 17, t->second);
    buf[19] = '\0';
    return BOX_OK;
}

int64_t time_diff(const time_t* a, const time_t* b) {
    return (int64_t)a->seconds - (int64_t)b->seconds;
}

void time_add_ms(const time_t* t, int64_t ms, time_t* out) {
    time_t tmp = *t;

    int64_t extra_ns = (ms % 1000) * 1000000LL;
    int64_t new_ns = (int64_t)tmp.nanosec + extra_ns;

    if (new_ns < 0) {
        int64_t borrow = (-new_ns + 999999999LL) / 1000000000LL;
        new_ns += borrow * 1000000000LL;
        tmp.seconds = (uint64_t)((int64_t)tmp.seconds - borrow);
    }

    int64_t carry = new_ns / 1000000000LL;
    tmp.nanosec = (uint32_t)(new_ns % 1000000000LL);
    tmp.seconds = (uint64_t)((int64_t)tmp.seconds + ms / 1000 + carry);

    *out = tmp;
}
