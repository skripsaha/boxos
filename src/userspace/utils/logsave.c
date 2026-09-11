#include "box/print.h"
#include "box/luggage.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"
#include "box/system.h"

#define LOGSAVE_DEFAULT_NAME  "watch.log"

static char s_pail[4096];
static char s_target[128];

int main(void)
{
    int argc = (int)luggage_word_count();

    const char *name = LOGSAVE_DEFAULT_NAME;
    if (argc > 1 && luggage_word(1)[0] != '\0') {
        name = luggage_word(1);
    }

    error_t  why = OK;
    Current *log = current_open_ex("log:file", CURRENT_READ, 0, 0, &why);
    if (!log) {
        if (why == ERR_UNSUPPORTED) {
            println("This kernel has no door to what it says.");
        } else {
            printf("The log could not be opened for reading (error %u)\n",
                   (unsigned)why);
        }
        exit(1);
        return 1;
    }

    size_t nlen = strlen(name);
    if (nlen + 6 > sizeof(s_target)) {
        println("That name is too long for a file tag.");
        current_release(log);
        exit(1);
        return 1;
    }
    memcpy(s_target, "file:", 5);
    memcpy(s_target + 5, name, nlen);
    s_target[5 + nlen] = '\0';

    Current *out = current_open_ex(s_target, CURRENT_WRITE, 0,
                                   CURRENT_CREATE | CURRENT_TRUNCATE, &why);
    if (!out) {
        printf("%s could not be written (error %u)\n", name, (unsigned)why);
        current_release(log);
        exit(1);
        return 1;
    }

    uint64_t poured = 0;
    int      failed = 0;

    for (;;) {
        int n = current_read(log, s_pail, sizeof(s_pail));
        if (n == CURRENT_CLOSED) break;
        if (n < 0) {
            printf("The log stopped being readable after %lu byte(s) "
                   "(error %d)\n", (unsigned long)poured, -n);
            failed = 1;
            break;
        }

        int off = 0;
        while (off < n) {
            int w = current_write(out, s_pail + off, (size_t)(n - off));
            if (w <= 0) {
                printf("%s stopped taking bytes after %lu (error %d)\n",
                       name, (unsigned long)poured, w < 0 ? -w : 0);
                failed = 1;
                break;
            }
            off    += w;
            poured += (uint64_t)w;
        }
        if (failed) break;
    }

    uint64_t lost = current_lost(log);

    current_close(out);
    current_release(out);
    current_release(log);

    if (failed) {
        exit(1);
        return 1;
    }

    printf("%lu byte(s) written to %s\n", (unsigned long)poured, name);
    if (lost) {
        printf("%lu byte(s) were said before this and the ring no longer had "
               "them — the file starts after that gap\n",
               (unsigned long)lost);
    }

    exit(0);
    return 0;
}