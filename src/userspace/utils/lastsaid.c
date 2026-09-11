#include "box/print.h"
#include "box/luggage.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"
#include "box/system.h"

static char s_pail[4096];
static char s_target[128];

int main(void)
{
    int argc = (int)luggage_word_count();

    const char *name = NULL;
    if (argc > 1 && luggage_word(1)[0] != '\0') {
        name = luggage_word(1);
    }

    error_t  why = OK;
    Current *log = current_open_ex("log:previous", CURRENT_READ, 0, 0, &why);
    if (!log) {
        if (why == ERR_UNSUPPORTED) {
            println("This kernel carries nothing through a reset. Build it with "
                    "PRINTTOFILE=on and the last run will be here.");
        } else {
            printf("The previous run's log could not be opened (error %u)\n",
                   (unsigned)why);
        }
        exit(1);
        return 1;
    }

    Current *out = NULL;
    if (name) {
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

        out = current_open_ex(s_target, CURRENT_WRITE, 0,
                              CURRENT_CREATE | CURRENT_TRUNCATE, &why);
        if (!out) {
            printf("%s could not be written (error %u) — printing it "
                   "instead\n", name, (unsigned)why);
        }
    }

    uint64_t poured = 0;
    int      failed = 0;

    for (;;) {
        int n = current_read(log, s_pail, sizeof(s_pail));
        if (n == CURRENT_CLOSED) break;
        if (n < 0) {
            printf("\nThe previous log stopped being readable after %lu "
                   "byte(s) (error %d)\n", (unsigned long)poured, -n);
            failed = 1;
            break;
        }

        if (out) {
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
        } else {
            print_bytes(s_pail, (size_t)n);
            poured += (uint64_t)n;
        }
    }

    uint64_t lost = current_lost(log);

    if (out) current_release(out);
    current_release(log);

    if (lost) {
        printf("\n%lu byte(s) of the previous run had already been "
               "overwritten in its own ring before it ended.\n",
               (unsigned long)lost);
    }

    if (failed) { exit(1); return 1; }

    if (poured == 0) {
        println("Nothing came through the last reset. Either this is the "
                "first run, or power was removed rather than the machine "
                "being reset, or this firmware scrubs memory on every start.");
    } else if (name && out) {
        printf("%lu byte(s) of the previous run written to %s\n",
               (unsigned long)poured, name);
    } else {
        printf("\n-- %lu byte(s) from the previous run --\n",
               (unsigned long)poured);
    }

    exit(0);
    return 0;
}