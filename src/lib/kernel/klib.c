#include "klib.h"
#include "vga.h"
#include "io.h"
#include "serial.h"
#include "pmm.h"

#define HEAP_CANARY_MAGIC 0xDEADBEEFCAFEBABEULL
#define HEAP_GUARD_ENABLED 1  // Set to 0 to disable

typedef struct {
    uint64_t canary_start;
    size_t size;
    const char* caller;
    size_t canary_end_offset;  // Location of end canary
} heap_guard_t;

static uint8_t *memory_pool = NULL;
static size_t memory_pool_size = 0;
static mem_block_t *free_list = NULL;
static spinlock_t heap_lock = {0};

static uint8_t current_attr = TEXT_ATTR_DEFAULT;

static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

void mem_init(void)
{
    extern size_t pmm_total_pages(void);
    size_t total_pages = pmm_total_pages();
    size_t total_ram = total_pages * 4096;

    size_t heap_size = (total_ram * KLIB_HEAP_RAM_PERCENT) / 100;

    if (heap_size < KLIB_HEAP_MIN_SIZE)
    {
        heap_size = KLIB_HEAP_MIN_SIZE;
    }
    else if (heap_size > KLIB_HEAP_MAX_SIZE)
    {
        heap_size = KLIB_HEAP_MAX_SIZE;
    }

// bootloader only maps 128MB; cap heap until VMM expands mapping
#define BOOTLOADER_SAFE_HEAP_SIZE (2 * 1024 * 1024)
    if (heap_size > BOOTLOADER_SAFE_HEAP_SIZE)
    {
        debug_printf("[KLIB] Heap size capped at 2MB (bootloader mapping limit), will be %zu MB after VMM init\n",
                heap_size / (1024 * 1024));
        heap_size = BOOTLOADER_SAFE_HEAP_SIZE;
    }

    heap_size = ALIGN_UP(heap_size, 4096);
    memory_pool_size = heap_size;

    size_t pages_needed = heap_size / 4096;

    debug_printf("[KLIB] Total RAM: %zu MB, Kernel heap: %zu MB (%zu pages)\n",
            total_ram / (1024 * 1024), heap_size / (1024 * 1024), pages_needed);

    debug_printf("[KLIB] Allocating memory pool from PMM...\n");
    memory_pool = (uint8_t *)pmm_alloc_zero(pages_needed);
    if (!memory_pool)
    {
        panic("Failed to allocate kernel memory pool from PMM!");
    }
    debug_printf("[KLIB] Memory pool allocated at %p\n", memory_pool);

    debug_printf("[KLIB] Initializing free list...\n");
    free_list = (mem_block_t *)memory_pool;
    free_list->size = heap_size - sizeof(mem_block_t);
    free_list->next = NULL;
    free_list->magic = KLIB_MAGIC_NUMBER;
    debug_printf("[KLIB] Free list initialized\n");
}

static void *kmalloc_internal(size_t size)
{
    if (size == 0)
        return NULL;

    size = (size + KLIB_BLOCK_ALIGNMENT - 1) & ~(KLIB_BLOCK_ALIGNMENT - 1);

    spin_lock(&heap_lock);

    mem_block_t *curr = free_list, *prev = NULL;
    void *result = NULL;

    while (curr)
    {
        if (curr->magic != KLIB_MAGIC_NUMBER)
        {
            panic("Memory corruption detected in kmalloc!");
        }

        if ((uintptr_t)curr < (uintptr_t)memory_pool ||
            (uintptr_t)curr >= (uintptr_t)memory_pool + memory_pool_size)
        {
            panic("kmalloc corruption: free_list entry 0x%p out of heap range [0x%p - 0x%p]",
                  curr, memory_pool, (void*)((uintptr_t)memory_pool + memory_pool_size));
        }

        if (curr->size >= size)
        {
            if (curr->size > size + sizeof(mem_block_t) + KLIB_BLOCK_ALIGNMENT)
            {
                mem_block_t *new_block = (mem_block_t *)((char *)curr + sizeof(mem_block_t) + size);
                new_block->size = curr->size - size - sizeof(mem_block_t);
                new_block->next = curr->next;
                new_block->magic = KLIB_MAGIC_NUMBER;
                curr->size = size;
                curr->next = new_block;
            }

            if (prev)
            {
                prev->next = curr->next;
            }
            else
            {
                free_list = curr->next;
            }

            curr->magic = KLIB_MAGIC_NUMBER;
            result = (void *)((char *)curr + sizeof(mem_block_t));

            if ((uintptr_t)result < (uintptr_t)memory_pool ||
                (uintptr_t)result >= (uintptr_t)memory_pool + memory_pool_size)
            {
                panic("kmalloc corruption: returned pointer 0x%p out of heap range [0x%p - 0x%p]",
                      result, memory_pool, (void*)((uintptr_t)memory_pool + memory_pool_size));
            }

            break;
        }

        prev = curr;

        if (curr->next != NULL)
        {
            if ((uintptr_t)curr->next < (uintptr_t)memory_pool ||
                (uintptr_t)curr->next >= (uintptr_t)memory_pool + memory_pool_size)
            {
                panic("kmalloc corruption: free_list->next 0x%p out of heap range [0x%p - 0x%p]",
                      curr->next, memory_pool, (void*)((uintptr_t)memory_pool + memory_pool_size));
            }
        }

        curr = curr->next;
    }

    spin_unlock(&heap_lock);

    if (!result)
    {
        panic("Out of kernel memory!");
    }
    return result;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

#if HEAP_GUARD_ENABLED
    size_t total_size = sizeof(heap_guard_t) + size + sizeof(uint64_t);
    heap_guard_t* guard = (heap_guard_t*)kmalloc_internal(total_size);

    if (!guard) return NULL;

    guard->canary_start = HEAP_CANARY_MAGIC;
    guard->size = size;
    guard->caller = "kmalloc";
    guard->canary_end_offset = sizeof(heap_guard_t) + size;

    uint64_t* end_canary = (uint64_t*)((uint8_t*)guard + guard->canary_end_offset);
    *end_canary = HEAP_CANARY_MAGIC;

    return (void*)((uint8_t*)guard + sizeof(heap_guard_t));
#else
    return kmalloc_internal(size);
#endif
}

static void kfree_internal(void *ptr)
{
    if (!ptr)
        return;

    mem_block_t *block = (mem_block_t *)((char *)ptr - sizeof(mem_block_t));

    if ((uintptr_t)block < (uintptr_t)memory_pool ||
        (uintptr_t)block >= (uintptr_t)memory_pool + memory_pool_size)
    {
        panic("Invalid free: pointer out of range!");
    }

    if (block->magic != KLIB_MAGIC_NUMBER)
    {
        panic("Invalid free: bad magic number!");
    }

    spin_lock(&heap_lock);

    for (mem_block_t *curr = free_list; curr; curr = curr->next)
    {
        if (curr == block)
        {
            panic("Double free detected!");
        }
    }

    mem_block_t *curr = free_list, *prev = NULL;
    while (curr && curr < block)
    {
        prev = curr;
        curr = curr->next;
    }

    if (prev && (char *)prev + sizeof(mem_block_t) + prev->size == (char *)block)
    {
        prev->size += sizeof(mem_block_t) + block->size;
        block = prev;
    }
    else
    {
        block->next = curr;
        if (prev)
        {
            prev->next = block;
        }
        else
        {
            free_list = block;
        }
    }

    if (block->next && (char *)block + sizeof(mem_block_t) + block->size == (char *)block->next)
    {
        block->size += sizeof(mem_block_t) + block->next->size;
        block->next = block->next->next;
    }

    spin_unlock(&heap_lock);
}

void kfree(void* ptr) {
    if (!ptr) return;

#if HEAP_GUARD_ENABLED
    heap_guard_t* guard = (heap_guard_t*)((uint8_t*)ptr - sizeof(heap_guard_t));

    if (guard->canary_start != HEAP_CANARY_MAGIC) {
        debug_printf("[HEAP] CORRUPTION: Start canary destroyed at %p\n", ptr);
        debug_printf("[HEAP]   Expected: 0x%llx, Got: 0x%llx\n",
                     HEAP_CANARY_MAGIC, guard->canary_start);
        while (1) { asm volatile("cli; hlt"); }
    }

    uint64_t* end_canary = (uint64_t*)((uint8_t*)guard + guard->canary_end_offset);
    if (*end_canary != HEAP_CANARY_MAGIC) {
        debug_printf("[HEAP] CORRUPTION: End canary destroyed at %p (size=%zu)\n",
                     ptr, guard->size);
        debug_printf("[HEAP]   Expected: 0x%llx, Got: 0x%llx\n",
                     HEAP_CANARY_MAGIC, *end_canary);
        debug_printf("[HEAP]   This indicates buffer overflow!\n");
        while (1) { asm volatile("cli; hlt"); }
    }

    kfree_internal(guard);
#else
    kfree_internal(ptr);
#endif
}

void mem_stats(void)
{
    spin_lock(&heap_lock);

    size_t free_blocks = 0;
    size_t free_memory = 0;
    size_t largest_block = 0;

    mem_block_t *curr = free_list;
    while (curr)
    {
        free_blocks++;
        free_memory += curr->size;
        if (curr->size > largest_block)
        {
            largest_block = curr->size;
        }
        curr = curr->next;
    }

    size_t used_memory = memory_pool_size - free_memory - free_blocks * sizeof(mem_block_t);

    kprintf("Memory Statistics:\n");
    kprintf("  Total memory: %zu bytes\n", memory_pool_size);
    kprintf("  Used memory:  %zu bytes\n", used_memory);
    kprintf("  Free memory:  %zu bytes\n", free_memory);
    kprintf("  Free blocks:  %zu\n", free_blocks);
    kprintf("  Largest free: %zu bytes\n", largest_block);

    spin_unlock(&heap_lock);
}

__attribute__((noreturn)) void panic(const char *message, ...)
{
    va_list args;
    va_start(args, message);

    kprintf("\n%[E]KERNEL PANIC:%[D] ");

    char temp_buf[512];
    ksnprintf(temp_buf, sizeof(temp_buf), message, args);
    kprintf("%s", temp_buf);

    kprintf("\n\nDebug info:");
    kprintf("\n- Stack pointer: %p", __builtin_frame_address(0));
    kprintf("\n- Instruction pointer: %p", __builtin_return_address(0));

    va_end(args);

    asm volatile("cli");
    while (1)
    {
        asm volatile("hlt");
    }
}

int utf8_encode(uint32_t codepoint, char out[4])
{
    if (codepoint <= 0x7F)
    {
        out[0] = (char)codepoint;
        return 1;
    }
    else if (codepoint <= 0x7FF)
    {
        out[0] = (char)(0xC0 | ((codepoint >> 6) & 0x1F));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    else if (codepoint <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    else if (codepoint <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    else
    {
        return 0;
    }
}

int utf8_decode(const char *utf8, uint32_t *codepoint)
{
    if (!utf8 || !codepoint)
        return 0;

    uint8_t first = (uint8_t)utf8[0];

    if (first <= 0x7F)
    {
        *codepoint = first;
        return 1;
    }
    else if ((first & 0xE0) == 0xC0)
    {
        *codepoint = ((first & 0x1F) << 6) | ((uint8_t)utf8[1] & 0x3F);
        return 2;
    }
    else if ((first & 0xF0) == 0xE0)
    {
        *codepoint = ((first & 0x0F) << 12) |
                     (((uint8_t)utf8[1] & 0x3F) << 6) |
                     ((uint8_t)utf8[2] & 0x3F);
        return 3;
    }
    else if ((first & 0xF8) == 0xF0)
    {
        *codepoint = ((first & 0x07) << 18) |
                     (((uint8_t)utf8[1] & 0x3F) << 12) |
                     (((uint8_t)utf8[2] & 0x3F) << 6) |
                     ((uint8_t)utf8[3] & 0x3F);
        return 4;
    }

    return 0;
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

bool isdigit(int c)
{
    return c >= '0' && c <= '9';
}

bool isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

bool isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int kputnl(void)
{
    vga_clear_to_eol();
    vga_print_newline();
    vga_update_cursor();
    return 1;
}

void kputchar(char c)
{
    if (c == '\n')
    {
        serial_putchar('\r');
        serial_putchar('\n');
    }
    else
    {
        serial_putchar(c);
    }

    if (c == '\n')
    {
        kputnl();
    }
    else if (c == '\r')
    {
        vga_set_cursor_position(0, vga_get_cursor_position_y());
    }
    else if (c == '\b')
    {
        int x = vga_get_cursor_position_x();
        if (x > 0)
        {
            vga_set_cursor_position(x - 1, vga_get_cursor_position_y());
            vga_update_cursor();
        }
    }
    else
    {
        vga_print_char(c, vga_get_color());
        vga_update_cursor();
    }
}

int kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = 0;

    while (*format)
    {
        if (*format == '%')
        {
            ++format;

            if (*format == '[')
            {
                format++;
                switch (*format)
                {
                case 'E':
                    current_attr = TEXT_ATTR_ERROR;
                    break;
                case 'S':
                    current_attr = TEXT_ATTR_SUCCESS;
                    break;
                case 'H':
                    current_attr = TEXT_ATTR_HINT;
                    break;
                case 'D':
                    current_attr = TEXT_ATTR_DEFAULT;
                    break;
                case 'W':
                    current_attr = TEXT_ATTR_WARNING;
                    break;
                case 'P':
                {
                    int x = va_arg(args, int);
                    int y = va_arg(args, int);
                    vga_set_cursor_position(x, y);
                    break;
                }
                case 'U':
                {
                    uint32_t codepoint = va_arg(args, uint32_t);
                    if (codepoint <= 0xFF)
                    {
                        kputchar((char)codepoint);
                        count++;
                    }
                    else
                    {
                        kputchar('?');
                        count++;
                    }
                    break;
                }
                }
                while (*format && *format != ']')
                    format++;
                if (*format == ']')
                    format++;
                continue;
            }

            int left_align = 0, pad_zero = 0, pad_width = 0;
            if (*format == '-')
            {
                left_align = 1;
                ++format;
            }
            else if (*format == '0')
            {
                pad_zero = 1;
                ++format;
            }
            while (*format >= '0' && *format <= '9')
            {
                pad_width = pad_width * 10 + (*format - '0');
                ++format;
            }

            int longflag = 0, longlongflag = 0, sizeflag = 0;

            if (*format == 'z')
            {
                sizeflag = 1;
                ++format;
            }
            else
            {
                while (*format == 'l')
                {
                    ++format;
                    if (longflag)
                    {
                        longlongflag = 1;
                        longflag = 0;
                    }
                    else
                    {
                        longflag = 1;
                    }
                }
            }

            switch (*format)
            {
            case 'd':
            case 'i':
            {
                char buf[32];
                int negative = 0;
                int len;
                if (sizeflag)
                {
                    long long num = (long long)va_arg(args, size_t);
                    if (num < 0)
                    {
                        negative = 1;
                        num = -num;
                    }
                    utoa64((uint64_t)num, buf, 10);
                }
                else if (longlongflag)
                {
                    long long num = va_arg(args, long long);
                    if (num < 0)
                    {
                        negative = 1;
                        num = -num;
                    }
                    utoa64((uint64_t)num, buf, 10);
                }
                else if (longflag)
                {
                    long num = va_arg(args, long);
                    if (num < 0)
                    {
                        negative = 1;
                        num = -num;
                    }
                    utoa64((uint64_t)num, buf, 10);
                }
                else
                {
                    int num = va_arg(args, int);
                    if (num < 0)
                    {
                        negative = 1;
                        num = -num;
                    }
                    utoa((unsigned int)num, buf, 10);
                }
                len = strlen(buf);
                int total = len + negative;

                if (left_align)
                {
                    if (negative)
                    {
                        kputchar('-');
                        ++count;
                    }
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar(*p);
                        ++count;
                    }
                    for (int i = total; i < pad_width; ++i)
                    {
                        kputchar(' ');
                        ++count;
                    }
                }
                else
                {
                    for (int i = total; i < pad_width; ++i)
                    {
                        kputchar(pad_zero ? '0' : ' ');
                        ++count;
                    }
                    if (negative)
                    {
                        kputchar('-');
                        ++count;
                    }
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar(*p);
                        ++count;
                    }
                }
                break;
            }
            case 'u':
            {
                char buf[32];
                int len;
                if (sizeflag)
                {
                    size_t num = va_arg(args, size_t);
                    utoa64((uint64_t)num, buf, 10);
                }
                else if (longlongflag)
                {
                    unsigned long long num = va_arg(args, unsigned long long);
                    utoa64(num, buf, 10);
                }
                else if (longflag)
                {
                    unsigned long num = va_arg(args, unsigned long);
                    utoa64(num, buf, 10);
                }
                else
                {
                    unsigned int num = va_arg(args, unsigned int);
                    utoa(num, buf, 10);
                }
                len = strlen(buf);

                if (left_align)
                {
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar(*p);
                        ++count;
                    }
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(' ');
                        ++count;
                    }
                }
                else
                {
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(pad_zero ? '0' : ' ');
                        ++count;
                    }
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar(*p);
                        ++count;
                    }
                }
                break;
            }
            case 'x':
            case 'X':
            {
                char buf[32];
                int len;
                if (sizeflag)
                {
                    size_t num = va_arg(args, size_t);
                    utoa64((uint64_t)num, buf, 16);
                }
                else if (longlongflag)
                {
                    unsigned long long num = va_arg(args, unsigned long long);
                    utoa64(num, buf, 16);
                }
                else if (longflag)
                {
                    unsigned long num = va_arg(args, unsigned long);
                    utoa64(num, buf, 16);
                }
                else
                {
                    unsigned int num = va_arg(args, unsigned int);
                    utoa(num, buf, 16);
                }
                len = strlen(buf);

                if (left_align)
                {
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar((*format == 'X') ? toupper(*p) : *p);
                        ++count;
                    }
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(' ');
                        ++count;
                    }
                }
                else
                {
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(pad_zero ? '0' : ' ');
                        ++count;
                    }
                    for (char *p = buf; *p; ++p)
                    {
                        kputchar((*format == 'X') ? toupper(*p) : *p);
                        ++count;
                    }
                }
                break;
            }
            case 'p':
            {
                void *ptr = va_arg(args, void *);
                char buf[32];
                utoa64((uint64_t)(uintptr_t)ptr, buf, 16);
                kputchar('0');
                kputchar('x');
                count += 2;
                int len = strlen(buf);
                int addr_width = 16;
                for (int i = len; i < addr_width; ++i)
                {
                    kputchar('0');
                    ++count;
                }
                for (char *p = buf; *p; ++p)
                {
                    kputchar(*p);
                    ++count;
                }
                break;
            }
            case 's':
            {
                const char *str = va_arg(args, const char *);
                if (!str)
                    str = "(null)";
                int len = strlen(str);

                if (left_align)
                {
                    while (*str)
                    {
                        kputchar(*str++);
                        ++count;
                    }
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(' ');
                        ++count;
                    }
                }
                else
                {
                    for (int i = len; i < pad_width; ++i)
                    {
                        kputchar(' ');
                        ++count;
                    }
                    while (*str)
                    {
                        kputchar(*str++);
                        ++count;
                    }
                }
                break;
            }
            case 'f':
            {
                double num = va_arg(args, double);
                char buf[64];
                ftoa(num, buf, 6);
                int len = strlen(buf);
                for (int i = len; i < pad_width; ++i)
                {
                    kputchar(' ');
                    ++count;
                }
                for (char *p = buf; *p; ++p)
                {
                    kputchar(*p);
                    ++count;
                }
                break;
            }
            case 'c':
            {
                char c = (char)va_arg(args, int);
                kputchar(c);
                count++;
                break;
            }
            case '%':
            {
                kputchar('%');
                count++;
                break;
            }
            default:
            {
                kputchar('%');
                kputchar(*format);
                count += 2;
                break;
            }
            }
            ++format;
            continue;
        }
        else if (*format == '\n')
        {
            kputchar('\n');
            count++;
        }
        else if (*format == '\r')
        {
            kputchar('\r');
            count++;
        }
        else
        {
            kputchar(*format);
            count++;
        }
        ++format;
    }
    va_end(args);
    return count;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    size_t pos = 0;
    const char *p = fmt;

    while (*p && pos + 1 < size)
    {
        if (*p == '%')
        {
            p++;

            if (*p == 'd' || *p == 'i')
            {
                int val = va_arg(args, int);
                char tmp[32];
                itoa(val, tmp, 10);
                for (char *t = tmp; *t && pos + 1 < size; ++t)
                    buf[pos++] = *t;
            }
            else if (*p == 'u')
            {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[32];
                utoa(val, tmp, 10);
                for (char *t = tmp; *t && pos + 1 < size; ++t)
                    buf[pos++] = *t;
            }
            else if (*p == 'x')
            {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[32];
                utoa(val, tmp, 16);
                for (char *t = tmp; *t && pos + 1 < size; ++t)
                    buf[pos++] = *t;
            }
            else if (*p == 's')
            {
                const char *sval = va_arg(args, const char *);
                while (*sval && pos + 1 < size)
                    buf[pos++] = *sval++;
            }
            else if (*p == 'c')
            {
                char c = (char)va_arg(args, int);
                if (pos + 1 < size)
                    buf[pos++] = c;
            }
            else if (*p == '%')
            {
                if (pos + 1 < size)
                    buf[pos++] = '%';
            }
            else
            {
                if (pos + 1 < size)
                    buf[pos++] = '%';
                if (pos + 1 < size)
                    buf[pos++] = *p;
            }
            p++;
        }
        else
        {
            buf[pos++] = *p++;
        }
    }
    buf[pos] = '\0';
    va_end(args);
    return (int)pos;
}

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->saved_flags = 0;
}

void spin_lock(spinlock_t *lock)
{
    // save IRQ state and disable interrupts before acquiring to prevent deadlock with IRQ handlers
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    lock->saved_flags = flags;

    while (__sync_lock_test_and_set(&lock->locked, 1))
    {
        asm volatile("pause");
    }
}

void spin_unlock(spinlock_t *lock)
{
    __sync_lock_release(&lock->locked);
    uint64_t flags = lock->saved_flags;
    asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
}

bool spin_trylock(spinlock_t *lock)
{
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

    if (!__sync_lock_test_and_set(&lock->locked, 1))
    {
        lock->saved_flags = flags;
        return true;
    }
    else
    {
        asm volatile("push %0; popfq" ::"r"(flags) : "memory", "cc");
        return false;
    }
}

void list_init(list_t *list)
{
    if (!list)
        return;

    memset(list, 0, sizeof(list_t));
    spinlock_init(&list->lock);
}

void list_destroy(list_t *list)
{
    if (!list)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        list_node_t *next = current->next;
        kfree(current);
        current = next;
    }

    list->head = list->tail = NULL;
    list->size = 0;

    spin_unlock(&list->lock);
}

void list_push_back(list_t *list, void *data)
{
    if (!list)
        return;

    list_node_t *node = kmalloc(sizeof(list_node_t));
    if (!node)
        return;

    node->data = data;
    node->next = NULL;

    spin_lock(&list->lock);

    node->prev = list->tail;
    if (list->tail)
    {
        list->tail->next = node;
    }
    else
    {
        list->head = node;
    }
    list->tail = node;
    list->size++;

    spin_unlock(&list->lock);
}

void *list_pop_back(list_t *list)
{
    if (!list || !list->tail)
        return NULL;

    spin_lock(&list->lock);

    list_node_t *node = list->tail;
    void *data = node->data;

    if (node->prev)
    {
        node->prev->next = NULL;
        list->tail = node->prev;
    }
    else
    {
        list->head = list->tail = NULL;
    }

    list->size--;
    kfree(node);

    spin_unlock(&list->lock);
    return data;
}

void list_push_front(list_t *list, void *data)
{
    if (!list)
        return;

    list_node_t *node = kmalloc(sizeof(list_node_t));
    if (!node)
        return;

    node->data = data;
    node->prev = NULL;

    spin_lock(&list->lock);

    node->next = list->head;
    if (list->head)
    {
        list->head->prev = node;
    }
    else
    {
        list->tail = node;
    }
    list->head = node;
    list->size++;

    spin_unlock(&list->lock);
}

void *list_pop_front(list_t *list)
{
    if (!list || !list->head)
        return NULL;

    spin_lock(&list->lock);

    list_node_t *node = list->head;
    void *data = node->data;

    if (node->next)
    {
        node->next->prev = NULL;
        list->head = node->next;
    }
    else
    {
        list->head = list->tail = NULL;
    }

    list->size--;
    kfree(node);

    spin_unlock(&list->lock);
    return data;
}

void *list_front(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);
    void *data = list->head ? list->head->data : NULL;
    spin_unlock(&list->lock);

    return data;
}

void *list_back(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);
    void *data = list->tail ? list->tail->data : NULL;
    spin_unlock(&list->lock);

    return data;
}

bool list_empty(list_t *list)
{
    if (!list)
        return true;

    spin_lock(&list->lock);
    bool empty = (list->size == 0);
    spin_unlock(&list->lock);

    return empty;
}

size_t list_size(list_t *list)
{
    if (!list)
        return 0;

    spin_lock(&list->lock);
    size_t size = list->size;
    spin_unlock(&list->lock);

    return size;
}

void list_remove(list_t *list, void *data, bool (*cmp)(void *, void *))
{
    if (!list || !data || !cmp)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        if (cmp(current->data, data))
        {
            list_node_t *to_remove = current;

            if (to_remove->prev)
            {
                to_remove->prev->next = to_remove->next;
            }
            else
            {
                list->head = to_remove->next;
            }

            if (to_remove->next)
            {
                to_remove->next->prev = to_remove->prev;
            }
            else
            {
                list->tail = to_remove->prev;
            }

            list->size--;
            current = to_remove->next;
            kfree(to_remove);
        }
        else
        {
            current = current->next;
        }
    }

    spin_unlock(&list->lock);
}

void list_for_each(list_t *list, void (*func)(void *))
{
    if (!list || !func)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        func(current->data);
        current = current->next;
    }

    spin_unlock(&list->lock);
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && *s++)
        len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *ret = dest;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
        s1++, s2++;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    return (n == 0) ? 0 : (*(unsigned char *)s1 - *(unsigned char *)s2);
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c && *s)
        s++;
    return (*s == (char)c) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s)
    {
        if (*s == (char)c)
            last = s;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (!needle_len)
        return (char *)haystack;

    while (*haystack)
    {
        if (*haystack == *needle)
        {
            if (!strncmp(haystack, needle, needle_len))
            {
                return (char *)haystack;
            }
        }
        haystack++;
    }
    return NULL;
}

char *strcat(char *dest, const char *src)
{
    char *ret = dest;
    while (*dest)
        dest++;
    while ((*dest++ = *src++))
        ;
    return ret;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *ret = dest;
    while (*dest)
        dest++;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return ret;
}

// Additional string functions
static char *strtok_saveptr = NULL;

char *strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &strtok_saveptr);
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (str)
        *saveptr = str;
    if (!*saveptr)
        return NULL;

    // Skip leading delimiters
    *saveptr += strspn(*saveptr, delim);
    if (!**saveptr)
        return NULL;

    // Find end of token
    char *token = *saveptr;
    *saveptr += strcspn(*saveptr, delim);

    if (**saveptr)
    {
        **saveptr = '\0';
        (*saveptr)++;
    }
    else
    {
        *saveptr = NULL;
    }

    return token;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s && strchr(accept, *s))
    {
        s++;
        count++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s && !strchr(reject, *s))
    {
        s++;
        count++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s)
    {
        if (strchr(accept, *s))
            return (char *)s;
        s++;
    }
    return NULL;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char uc = (unsigned char)c;

    // For small sizes, just do byte-by-byte
    if (n < 8)
    {
        while (n--)
            *p++ = uc;
        return s;
    }

    // Align to 8-byte boundary
    while (((uintptr_t)p & 7) && n > 0)
    {
        *p++ = uc;
        n--;
    }

    // Fill 8 bytes at a time
    if (n >= 8)
    {
        uint64_t pattern = (uint64_t)uc | ((uint64_t)uc << 8) |
                           ((uint64_t)uc << 16) | ((uint64_t)uc << 24) |
                           ((uint64_t)uc << 32) | ((uint64_t)uc << 40) |
                           ((uint64_t)uc << 48) | ((uint64_t)uc << 56);

        uint64_t *p64 = (uint64_t *)p;
        while (n >= 8)
        {
            *p64++ = pattern;
            n -= 8;
        }
        p = (unsigned char *)p64;
    }

    // Fill remaining bytes
    while (n--)
        *p++ = uc;

    return s;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen)
{
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen)
        return NULL;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;

    for (size_t i = 0; i <= haystacklen - needlelen; i++)
    {
        if (memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }

    return NULL;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    if (d < s)
    {
        while (n--)
            *d++ = *s++;
    }
    else
    {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--)
    {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++, p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--)
    {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

char *reverse_str(char *str)
{
    if (!str)
        return NULL;

    char *end = str + strlen(str) - 1;
    while (str < end)
    {
        char tmp = *str;
        *str++ = *end;
        *end-- = tmp;
    }
    return str;
}

char *reverse_range(char *start, char *end)
{
    if (!start || !end)
        return NULL;
    while (start < end)
    {
        char tmp = *start;
        *start++ = *end;
        *end-- = tmp;
    }
    return start;
}

char *itoa(int value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *orig = str;
    bool negative = false;

    if (value < 0 && base == 10)
    {
        negative = true;
        value = -value;
    }

    do
    {
        *str++ = digits[value % base];
        value /= base;
    } while (value);

    if (negative)
        *str++ = '-';
    *str = '\0';
    return reverse_str(orig);
}

char *utoa(unsigned int value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *orig = str;

    do
    {
        *str++ = digits[value % base];
        value /= base;
    } while (value);

    *str = '\0';
    return reverse_str(orig);
}

char *itoa64(int64_t value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return NULL;
    }

    char *orig = str;

    if (value == 0)
    {
        *str++ = '0';
        *str = '\0';
        return orig;
    }

    bool negative = false;
    if (value < 0 && base == 10)
    {
        negative = true;
        value = -value;
    }

    char *ptr = str;
    do
    {
        *ptr++ = digits[value % base];
        value /= base;
    } while (value);

    if (negative)
    {
        *ptr++ = '-';
    }
    *ptr = '\0';

    reverse_range(str, ptr - 1);
    return orig;
}

char *utoa64(uint64_t value, char *str, int base)
{
    if (!str || base < 2 || base > 36)
    {
        if (str)
            *str = '\0';
        return str;
    }

    char *ptr = str, *start = str;

    if (value == 0)
    {
        *ptr++ = '0';
        *ptr = '\0';
        return str;
    }

    while (value)
    {
        *ptr++ = digits[value % base];
        value /= base;
    }
    *ptr = '\0';

    // Reverse
    for (char *a = start, *b = ptr - 1; a < b; ++a, --b)
    {
        char t = *a;
        *a = *b;
        *b = t;
    }
    return str;
}

// Safe versions
int itoa_s(int value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[33];
    char *result = itoa(value, buf, base);

    if (!result)
        return -1;

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -1;
    }

    strcpy(str, buf);
    return 0;
}

int itoa64_s(int64_t value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[65];
    char *result = itoa64(value, buf, base);

    if (!result)
    {
        str[0] = '\0';
        return -1;
    }

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -2;
    }

    strcpy(str, buf);
    return 0;
}

int utoa64_s(uint64_t value, char *str, size_t size, int base)
{
    if (!str || size == 0)
        return -1;

    char buf[65];
    char *result = utoa64(value, buf, base);

    if (!result)
    {
        str[0] = '\0';
        return -1;
    }

    size_t len = strlen(buf);
    if (len >= size)
    {
        str[0] = '\0';
        return -2;
    }

    strcpy(str, buf);
    return 0;
}

// Other conversion variants
char *ltoa(long value, char *str, int base)
{
    return itoa64((int64_t)value, str, base);
}

char *ultoa(unsigned long value, char *str, int base)
{
    return utoa64((uint64_t)value, str, base);
}

char *lltoa(long long value, char *str, int base)
{
    return itoa64((int64_t)value, str, base);
}

char *ulltoa(unsigned long long value, char *str, int base)
{
    return utoa64((uint64_t)value, str, base);
}

// Float to string conversion
void ftoa(double num, char *buf, int precision)
{
    int i = 0;

    if (num < 0)
    {
        buf[i++] = '-';
        num = -num;
    }

    int int_part = (int)num;
    double fractional_part = num - (double)int_part;

    char intbuf[32];
    itoa(int_part, intbuf, 10);
    for (char *p = intbuf; *p; ++p)
    {
        buf[i++] = *p;
    }

    if (precision > 0)
    {
        buf[i++] = '.';

        for (int j = 0; j < precision; j++)
        {
            fractional_part *= 10.0;
            int digit = (int)fractional_part;
            buf[i++] = '0' + digit;
            fractional_part -= digit;
        }
    }

    buf[i] = '\0';
}

int atoi(const char *str)
{
    int result = 0;
    int sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

long atol(const char *str)
{
    long result = 0;
    long sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

long long atoll(const char *str)
{
    long long result = 0;
    long long sign = 1;

    while (isspace(*str))
        str++;

    if (*str == '-')
    {
        sign = -1;
        str++;
    }
    else if (*str == '+')
    {
        str++;
    }

    while (isdigit(*str))
    {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

void delay(uint32_t milliseconds)
{
    // Improved delay implementation using CPU cycles
    // Note: This is still approximate and depends on CPU speed
    // For accurate timing, use PIT or HPET timer
    for (volatile uint32_t i = 0; i < milliseconds * 100000; i++)
    {
        // Add pause instruction to be more CPU-friendly
        asm volatile("pause");
    }
}

bool tag_is_wildcard(const char* tag)
{
    if (!tag) return false;
    return strstr(tag, "...") != NULL;
}

// Match a single part (key or value) with "..." as glob wildcard
static bool tag_match_part(const char* pattern, const char* text)
{
    const char* wild = strstr(pattern, "...");
    if (!wild) {
        return strcmp(pattern, text) == 0;
    }

    size_t prefix_len = wild - pattern;
    const char* suffix = wild + 3;
    size_t suffix_len = strlen(suffix);
    size_t text_len = strlen(text);

    if (text_len < prefix_len + suffix_len) return false;
    if (prefix_len > 0 && strncmp(pattern, text, prefix_len) != 0) return false;
    if (suffix_len > 0 && strcmp(text + text_len - suffix_len, suffix) != 0) return false;

    return true;
}

bool tag_match(const char* pattern, const char* tag)
{
    if (!pattern || !tag) return false;
    if (!tag_is_wildcard(pattern)) {
        return strcmp(pattern, tag) == 0;
    }

    const char* p_colon = strchr(pattern, ':');
    const char* t_colon = strchr(tag, ':');

    if (p_colon && t_colon) {
        // Both have colon: match key and value parts separately
        char p_key[64], t_key[64];
        size_t p_key_len = p_colon - pattern;
        size_t t_key_len = t_colon - tag;
        if (p_key_len >= 64) p_key_len = 63;
        if (t_key_len >= 64) t_key_len = 63;
        memcpy(p_key, pattern, p_key_len); p_key[p_key_len] = '\0';
        memcpy(t_key, tag, t_key_len); t_key[t_key_len] = '\0';

        return tag_match_part(p_key, t_key) && tag_match_part(p_colon + 1, t_colon + 1);
    }

    if (!p_colon && !t_colon) {
        // Neither has colon: simple tag match with wildcard
        return tag_match_part(pattern, tag);
    }

    // One has colon, other doesn't — no match
    return false;
}