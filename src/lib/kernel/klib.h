#ifndef KLIB_H
#define KLIB_H

#include "ktypes.h"   // Replaces stdint.h, stddef.h, stdbool.h
#include "kstdarg.h"  // Replaces stdarg.h
#include "kernel_config.h"

extern uintptr_t _kernel_end;
extern uintptr_t _kernel_start;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(val, min, max) (MAX(MIN((val), (max)), (min)))
#define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))

// Kernel heap size: DYNAMIC (calculated at runtime based on available RAM)
// Formula: 3% of total RAM, clamped to [2MB, 16MB]
#define KLIB_HEAP_MIN_SIZE    (2 * 1024 * 1024)   // 2MB minimum
#define KLIB_HEAP_MAX_SIZE    (16 * 1024 * 1024)  // 16MB maximum
#define KLIB_HEAP_RAM_PERCENT 3  // Use 3% of total RAM for kernel heap

// ALIGNMENT FIX: Must be 32 bytes for correct operation
// Reason: mem_block_t is 20 bytes (size_t=8 + ptr=8 + uint32_t=4)
// With 16-byte alignment, block splitting in kmalloc creates misaligned
// new_block pointers, causing page faults when accessing block->magic.
// 32-byte alignment ensures all block headers are properly aligned.
#define KLIB_BLOCK_ALIGNMENT  32
#define KLIB_MAGIC_NUMBER     0xDEADBEEF

typedef struct mem_block {
    size_t size;
    struct mem_block *next;
    uint32_t magic;
} mem_block_t;

typedef struct {
    uint32_t locked;
    uint64_t saved_flags;  // Saved RFLAGS (for IRQ state)
} spinlock_t;

typedef struct list_node {
    void* data;
    struct list_node* next;
    struct list_node* prev;
} list_node_t;

typedef struct {
    list_node_t* head;
    list_node_t* tail;
    size_t size;
    spinlock_t lock;
} list_t;

void list_init(list_t* list);
void list_destroy(list_t* list);
void list_push_back(list_t* list, void* data);
void list_push_front(list_t* list, void* data);
void* list_pop_back(list_t* list);
void* list_pop_front(list_t* list);
void* list_front(list_t* list);
void* list_back(list_t* list);
bool list_empty(list_t* list);
size_t list_size(list_t* list);
void list_remove(list_t* list, void* data, bool (*cmp)(void*, void*));
void list_for_each(list_t* list, void (*func)(void*));

void mem_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);
void mem_stats(void);

__attribute__((noreturn)) void panic(const char* message, ...);
int kprintf(const char* format, ...);
int ksnprintf(char* buf, size_t size, const char* fmt, ...);
void kputchar(char c);
int kputnl(void);

// Debug output macro - controlled by CONFIG_DEBUG_ENABLED
#if CONFIG_DEBUG_ENABLED
    #define debug_printf(...) kprintf(__VA_ARGS__)
#else
    #define debug_printf(...) do { if (0) kprintf(__VA_ARGS__); } while(0)
#endif

void spinlock_init(spinlock_t* lock);
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);
bool spin_trylock(spinlock_t* lock);

size_t strlen(const char* s);
size_t strnlen(const char* s, size_t maxlen);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);

void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
void* memchr(const void* s, int c, size_t n);

char* itoa(int value, char* str, int base);
char* utoa(unsigned int value, char* str, int base);
char* itoa64(int64_t value, char* str, int base);
char* utoa64(uint64_t value, char* str, int base);
char* reverse_str(char* str);
char* reverse_range(char* start, char* end);
char* ltoa(long value, char* str, int base);
char* ultoa(unsigned long value, char* str, int base);
char* lltoa(long long value, char* str, int base);
char* ulltoa(unsigned long long value, char* str, int base);

int itoa_s(int value, char* str, size_t size, int base);
int itoa64_s(int64_t value, char* str, size_t size, int base);
int utoa64_s(uint64_t value, char* str, size_t size, int base);

int atoi(const char* str);
long atol(const char* str);
long long atoll(const char* str);
void delay(uint32_t milliseconds);

void ftoa(double num, char* buf, int precision);
int toupper(int c);
int tolower(int c);
bool isdigit(int c);
bool isalpha(int c);
bool isalnum(int c);
bool isspace(int c);

int utf8_encode(uint32_t codepoint, char out[4]);
int utf8_decode(const char* utf8, uint32_t* codepoint);

char* strtok(char* str, const char* delim);
char* strtok_r(char* str, const char* delim, char** saveptr);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* s, const char* accept);

#endif // KLIB_H