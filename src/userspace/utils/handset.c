
#include "box/print.h"
#include "box/luggage.h"
#include "box/strand.h"
#include "box/touch.h"
#include "box/sync.h"
#include "box/system.h"
#include "box/string.h"

static volatile uint64_t g_deaf;

static void deaf_reader(void *arg)
{
    (void)arg;
    TouchTag ear = console_listen();
    if (ear != TOUCH_TAG_INVALID) touch_release(ear);
    __atomic_store_n(&g_deaf, 1u, __ATOMIC_RELEASE);
    for (;;) yield();
}

static int read_and_say(void)
{
    char line[256];
    int n = readline(line, sizeof(line));
    if (n < 0) {
        printf("[HANDSET] FAILED: readline said %d\n", n);
        return 1;
    }
    printf("[HANDSET] got: %s\n", line);
    return 0;
}

int main(void)
{
    const char *mode = luggage_word(1);
    if (mode && strcmp(mode, "deaf") == 0) {
        if (strand_spawn(deaf_reader, NULL) == 0) {
            printf("[HANDSET] FAILED: no strand for the deaf reader\n");
            return 1;
        }
        while (__atomic_load_n(&g_deaf, __ATOMIC_ACQUIRE) == 0) yield();
        printf("[HANDSET] deaf ear on top\n");
        addr_park((void *)&g_deaf, 1, 3000);
        return read_and_say();
    }

    printf("[HANDSET] ask\n");
    return read_and_say();
}