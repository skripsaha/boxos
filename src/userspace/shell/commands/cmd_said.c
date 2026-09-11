#include "commands.h"
#include "box/print.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"

static char s_pail[4096];
static char s_target[128];

static int said_pour(const char *tag, const char *what, const char *name)
{
    error_t  why = OK;
    Current *log = current_open_ex(tag, CURRENT_READ, 0, 0, &why);
    if (!log) {
        if (why == ERR_UNSUPPORTED) {
            println("This kernel does not keep what it says. Build it with "
                    "PRINTTOFILE=on and it will be here.");
        } else {
            printf("%s could not be opened (error %u)\n", what, (unsigned)why);
        }
        return 1;
    }

    Current *out = NULL;
    error_t  refused = OK;
    if (name) {
        size_t nlen = strlen(name);
        if (nlen + 6 > sizeof(s_target)) {
            println("That name is too long for a file tag.");
            current_release(log);
            return 1;
        }
        memcpy(s_target, "file:", 5);
        memcpy(s_target + 5, name, nlen);
        s_target[5 + nlen] = '\0';

        out = current_open_ex(s_target, CURRENT_WRITE, 0,
                              CURRENT_CREATE | CURRENT_TRUNCATE, &why);
        if (!out) {
            refused = why;
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
            printf("\n%s stopped being readable after %lu byte(s) "
                   "(error %d)\n", what, (unsigned long)poured, -n);
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
        printf("\n%lu byte(s) had already been overwritten in the ring "
               "before this could read them.\n", (unsigned long)lost);
    }

    if (failed) return 1;

    if (poured == 0) {
        printf("Nothing to read: %s is empty. For a previous run that means "
               "either this is the first one, or power was removed rather "
               "than the machine reset, or this firmware rewrites memory on "
               "every start.\n", what);
        return 0;
    }

    if (name && out) {
        printf("%lu byte(s) of %s written to %s\n",
               (unsigned long)poured, what, name);
        return 0;
    }

    printf("\n-- %lu byte(s): %s --\n", (unsigned long)poured, what);

    if (name && refused != OK) {
        printf("-- and %s was NOT written: error %u --\n",
               name, (unsigned)refused);
    }
    return 0;
}

int cmd_said(int argc, char *argv[])
{
    const char *tag  = "log:file";
    const char *what = "what this boot has said";
    const char *name = NULL;
    int         at   = 1;

    if (argc > 1 && strcmp(argv[1], "before") == 0) {
        tag  = "log:previous";
        what = "what the run before this one said";
        at   = 2;
    }

    if (argc > at && argv[at][0] != '\0') {
        name = argv[at];
    }

    return said_pour(tag, what, name);
}