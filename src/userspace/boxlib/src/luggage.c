/*
 * luggage.c — the line this program was started with, read from its cabin.
 *
 * The kernel wrote the bytes before the first instruction (system.proc.exec,
 * cabin_luggage_give); CabinInfo says where and how many. Nothing here waits
 * and nothing can arrive late. The words are cut once, on the first ask, into
 * two private copies: one cut into NUL-terminated words, one whole for the
 * tails. Strands share the cabin and so the luggage; the first strand to ask
 * does the cutting and the others see it done.
 */

#include "box/luggage.h"
#include "box/core/cabin.h"
#include "box/memory.h"
#include "box/string.h"

/* -------------------------------------------------------------------------
 * The cutter — one rule for the shell and for the luggage
 * ------------------------------------------------------------------------- */

/* Cut in place; `starts`, when given, receives each word's offset in the
 * line as typed, an opening quote included, for the tails. */
static uint32_t cut_words(char *line, char **words, uint32_t *starts, uint32_t max)
{
    uint32_t count = 0;
    char    *p     = line;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        uint32_t start = (uint32_t)(p - line);
        char    *word;
        char    *end;
        if (*p == '"') {
            word = ++p;
            end  = word;
            while (*end != '\0' && *end != '"') end++;
        } else {
            word = p;
            end  = word;
            while (*end != '\0' && *end != ' ' && *end != '\t') end++;
        }

        if (count < max) {
            words[count] = word;
            if (starts) starts[count] = start;
        }
        count++;

        if (*end == '\0') break;
        *end = '\0';
        p = end + 1;
    }
    return count;
}

uint32_t luggage_cut(char *line, char **words, uint32_t max)
{
    if (!line) return 0;
    return cut_words(line, words, NULL, max);
}

/* -------------------------------------------------------------------------
 * The cabin's line, cut once
 * ------------------------------------------------------------------------- */

static struct {
    volatile uint32_t claimed;   /* the first asker cuts */
    volatile uint32_t ready;     /* the cut is done */
    char             *whole;     /* the line as typed, NUL-terminated */
    char             *cut;       /* the same, separators turned into NULs */
    char            **words;     /* into `cut` */
    uint32_t         *starts;    /* offsets into `whole` */
    uint32_t          count;
} g_luggage;

Luggage luggage(void)
{
    const CabinInfo *ci = cabin_info();
    Luggage l;
    l.bytes  = ci->luggage_length ? (const char *)(uintptr_t)ci->luggage_addr : NULL;
    l.length = ci->luggage_length;
    return l;
}

static void luggage_prepare(void)
{
    if (__atomic_load_n(&g_luggage.ready, __ATOMIC_ACQUIRE)) return;

    if (!__sync_bool_compare_and_swap(&g_luggage.claimed, 0u, 1u)) {
        /* Another strand is cutting; its work is a few microseconds away. */
        while (!__atomic_load_n(&g_luggage.ready, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
        return;
    }

    Luggage l = luggage();
    if (l.length > 0) {
        char *whole = malloc(l.length + 1);
        char *cut   = malloc(l.length + 1);
        if (whole && cut) {
            memcpy(whole, l.bytes, l.length);
            whole[l.length] = '\0';
            memcpy(cut, whole, l.length + 1);

            /* Count first, then cut into an array of exactly that size. The
             * count pass cuts a scratch copy; the real cut runs on `cut`. */
            char *scratch = malloc(l.length + 1);
            if (scratch) {
                memcpy(scratch, whole, l.length + 1);
                uint32_t n = cut_words(scratch, NULL, NULL, 0);
                free(scratch);

                char    **words  = n ? malloc(sizeof(char *) * n) : NULL;
                uint32_t *starts = n ? malloc(sizeof(uint32_t) * n) : NULL;
                if (n == 0 || (words && starts)) {
                    cut_words(cut, words, starts, n);
                    g_luggage.whole  = whole;
                    g_luggage.cut    = cut;
                    g_luggage.words  = words;
                    g_luggage.starts = starts;
                    g_luggage.count  = n;
                    whole = cut = NULL;
                } else {
                    free(words);
                    free(starts);
                }
            }
        }
        /* Out of memory: the luggage reads as empty rather than as a crash;
         * whole/cut are either owned above or freed here. */
        free(whole);
        free(cut);
    }

    __atomic_store_n(&g_luggage.ready, 1u, __ATOMIC_RELEASE);
}

uint32_t luggage_word_count(void)
{
    luggage_prepare();
    return g_luggage.count;
}

const char *luggage_word(uint32_t index)
{
    luggage_prepare();
    if (index >= g_luggage.count) return NULL;
    return g_luggage.words[index];
}

const char *luggage_tail(uint32_t from)
{
    luggage_prepare();
    if (from >= g_luggage.count) return "";
    return g_luggage.whole + g_luggage.starts[from];
}
