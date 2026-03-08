#include "box/time.h"
#include "box/notify.h"
#include "box/result.h"

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
    if (!out) return ERR_NULL_POINTER;

    uint8_t buf[16] = {0};
    pocket_send(DECK_HARDWARE, 0x15, buf, sizeof(buf));

    Result result;
    if (!result_wait(&result, 50000)) return ERR_TIMEOUT;
    if (result.error_code != OK) return (int)result.error_code;

    const uint8_t* p = (const uint8_t*)(uintptr_t)result.data_addr;
    out->seconds = read_u64_be(p + 0);
    out->nanosec = read_u32_be(p + 8);
    out->year    = read_u16_be(p + 12);
    out->month   = p[14];
    out->day     = p[15];
    out->hour    = p[16];
    out->minute  = p[17];
    out->second  = p[18];
    out->weekday = p[19];
    return OK;
}

int time_get_secs(uint64_t* out_seconds) {
    if (!out_seconds) return ERR_NULL_POINTER;

    uint8_t unix_buf[16] = {0};
    pocket_send(DECK_HARDWARE, 0x16, unix_buf, sizeof(unix_buf));

    Result result;
    if (!result_wait(&result, 50000)) return ERR_TIMEOUT;
    if (result.error_code != OK) return (int)result.error_code;

    const uint8_t* p = (const uint8_t*)(uintptr_t)result.data_addr;
    *out_seconds = read_u64_be(p);
    return OK;
}

int time_uptime_ms(uint64_t* out_ms) {
    if (!out_ms) return ERR_NULL_POINTER;

    uint8_t timer_buf[8] = {0};
    pocket_send(DECK_HARDWARE, 0x11, timer_buf, sizeof(timer_buf));

    Result result;
    if (!result_wait(&result, 50000)) return ERR_TIMEOUT;
    if (result.error_code != OK) return (int)result.error_code;

    const uint8_t* p = (const uint8_t*)(uintptr_t)result.data_addr;
    *out_ms = read_u64_be(p);
    return OK;
}

int time_uptime_ns(uint64_t* out_ns) {
    if (!out_ns) return ERR_NULL_POINTER;

    uint8_t uptime_buf[16] = {0};
    pocket_send(DECK_HARDWARE, 0x17, uptime_buf, sizeof(uptime_buf));

    Result result;
    if (!result_wait(&result, 50000)) return ERR_TIMEOUT;
    if (result.error_code != OK) return (int)result.error_code;

    const uint8_t* p = (const uint8_t*)(uintptr_t)result.data_addr;
    *out_ns = read_u64_be(p);
    return OK;
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
    if (!t || !buf) return ERR_NULL_POINTER;
    if (buf_size < 20) return ERR_BUFFER_TOO_SMALL;

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
    return OK;
}

