#include "box/convert.h"
#include "box/string.h"

// ============================================================================
// STRING → NUMBER
// ============================================================================

int to_int(const char* str) {
    if (!str) return 0;
    while (is_space(*str)) str++;

    int sign = 1;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') { str++; }

    int result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}

unsigned int to_uint(const char* str) {
    if (!str) return 0;
    while (is_space(*str)) str++;
    if (*str == '+') str++;

    unsigned int result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (unsigned int)(*str - '0');
        str++;
    }
    return result;
}

int64_t to_int64(const char* str) {
    if (!str) return 0;
    while (is_space(*str)) str++;

    int sign = 1;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') { str++; }

    int64_t result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return sign * result;
}

uint64_t to_uint64(const char* str) {
    if (!str) return 0;
    while (is_space(*str)) str++;
    if (*str == '+') str++;

    uint64_t result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (uint64_t)(*str - '0');
        str++;
    }
    return result;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

uint32_t hex_to_int(const char* str) {
    if (!str) return 0;
    while (is_space(*str)) str++;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;

    uint32_t result = 0;
    int d;
    while ((d = hex_digit(*str)) >= 0) {
        result = (result << 4) | (uint32_t)d;
        str++;
    }
    return result;
}

// ============================================================================
// NUMBER → STRING
// ============================================================================

char* to_str(int value, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return buf;

    if (value == 0) {
        buf[0] = '0'; buf[1] = '\0';
        return buf;
    }

    char tmp[12];
    int i = 0;
    int neg = 0;
    unsigned int uval;

    if (value < 0) {
        neg = 1;
        uval = (unsigned int)(-(value + 1)) + 1;
    } else {
        uval = (unsigned int)value;
    }

    while (uval > 0 && i < 11) {
        tmp[i++] = '0' + (char)(uval % 10);
        uval /= 10;
    }

    size_t j = 0;
    if (neg && j < buf_size - 1) buf[j++] = '-';
    while (i > 0 && j < buf_size - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

char* uint_to_str(unsigned int value, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return buf;

    if (value == 0) {
        buf[0] = '0'; buf[1] = '\0';
        return buf;
    }

    char tmp[11];
    int i = 0;
    while (value > 0 && i < 10) {
        tmp[i++] = '0' + (char)(value % 10);
        value /= 10;
    }

    size_t j = 0;
    while (i > 0 && j < buf_size - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

char* to_hex(uint32_t value, char* buf, size_t buf_size) {
    if (!buf || buf_size < 4) return buf;

    const char digits[] = "0123456789abcdef";

    if (value == 0) {
        buf[0] = '0'; buf[1] = 'x'; buf[2] = '0'; buf[3] = '\0';
        return buf;
    }

    char tmp[9];
    int i = 0;
    uint32_t v = value;
    while (v > 0 && i < 8) {
        tmp[i++] = digits[v & 0xF];
        v >>= 4;
    }

    size_t j = 0;
    if (j + 2 < buf_size) { buf[j++] = '0'; buf[j++] = 'x'; }
    while (i > 0 && j < buf_size - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

char* to_bin(uint32_t value, char* buf, size_t buf_size) {
    if (!buf || buf_size < 4) return buf;

    if (value == 0) {
        buf[0] = '0'; buf[1] = 'b'; buf[2] = '0'; buf[3] = '\0';
        return buf;
    }

    int bits = 0;
    uint32_t v = value;
    while (v > 0) { bits++; v >>= 1; }

    size_t j = 0;
    if (j + 2 < buf_size) { buf[j++] = '0'; buf[j++] = 'b'; }
    for (int b = bits - 1; b >= 0 && j < buf_size - 1; b--) {
        buf[j++] = (value & (1U << b)) ? '1' : '0';
    }
    buf[j] = '\0';
    return buf;
}

// ============================================================================
// TYPE CHECKS
// ============================================================================

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_alnum(char c) { return is_digit(c) || is_alpha(c); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

bool is_number(const char* str) {
    if (!str || !*str) return false;
    if (*str == '-' || *str == '+') str++;
    if (!*str) return false;
    while (*str) { if (!is_digit(*str)) return false; str++; }
    return true;
}

bool is_hex_string(const char* str) {
    if (!str || !*str) return false;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    if (!*str) return false;
    while (*str) { if (hex_digit(*str) < 0) return false; str++; }
    return true;
}

// ============================================================================
// CHARACTER CONVERSION
// ============================================================================

char to_upper(char c) { return is_lower(c) ? (char)(c - 32) : c; }
char to_lower(char c) { return is_upper(c) ? (char)(c + 32) : c; }
