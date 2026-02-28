#ifndef BOX_CONVERT_H
#define BOX_CONVERT_H

#include "box/defs.h"

int to_int(const char* str);
unsigned int to_uint(const char* str);
int64_t to_int64(const char* str);
uint64_t to_uint64(const char* str);

// Hex string → number: "FF" or "0xFF" → 255
uint32_t hex_to_int(const char* str);

// Number → string (writes into caller-provided buffer, returns buf)
char* to_str(int value, char* buf, size_t buf_size);
char* uint_to_str(unsigned int value, char* buf, size_t buf_size);
char* to_hex(uint32_t value, char* buf, size_t buf_size);
char* to_bin(uint32_t value, char* buf, size_t buf_size);

bool is_digit(char c);
bool is_alpha(char c);
bool is_alnum(char c);
bool is_space(char c);
bool is_upper(char c);
bool is_lower(char c);
bool is_number(const char* str);
bool is_hex_string(const char* str);

char to_upper(char c);
char to_lower(char c);

#endif // BOX_CONVERT_H
