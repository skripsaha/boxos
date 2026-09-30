/* klib_sort.c — one sort for the kernel.
 *
 * A heapsort, because its bound is the whole of its promise: n log n compares
 * on every input, a constant stack and nothing taken from the heap. A caller
 * sorting a hundred thousand tags on a 16 KiB kernel stack, or a small list
 * under a spinlock, uses the same routine. */
#include "klib.h"

static void KSortSwap(uint8_t* a, uint8_t* b, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        uint8_t t = a[i];
        a[i]      = b[i];
        b[i]      = t;
    }
}

/* The element at `root` sunk until the heap of `count` below it holds again. */
static void KSortSift(uint8_t* base, size_t root, size_t count, size_t size,
                      KSortOrder order, void* ctx)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= count)
            return;
        if (child + 1 < count &&
            order(base + child * size, base + (child + 1) * size, ctx) < 0)
            child++;
        if (order(base + root * size, base + child * size, ctx) >= 0)
            return;
        KSortSwap(base + root * size, base + child * size, size);
        root = child;
    }
}

void KSort(void* base, size_t count, size_t size, KSortOrder order, void* ctx)
{
    if (!base || count < 2 || size == 0 || !order)
        return;
    uint8_t* b = base;
    for (size_t root = count / 2; root-- > 0;)
        KSortSift(b, root, count, size, order, ctx);
    for (size_t end = count - 1; end > 0; end--) {
        KSortSwap(b, b + end * size, size);
        KSortSift(b, 0, end, size, order, ctx);
    }
}
