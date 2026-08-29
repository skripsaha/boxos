/*
 * cmd_said — what this machine has said, without loading anything to ask.
 *
 * `logsave` and `lastsaid` already do this, and both are still here. They are
 * ELF files on the volume, and that is exactly the assumption this command
 * exists to drop: on the board the failure being chased is "the volume mounts,
 * reports its fifty-eight files, and the shell cannot find a single one of
 * them". Every command comes back Unknown, including the two that would have
 * written the log — so at the one moment the account is worth having, nothing
 * that could write it can be started.
 *
 * This is compiled into shell.bin. It needs no lookup, no spawn and no volume
 * to run; it needs the volume only if it is asked for a file, and when the
 * volume refuses it says so and prints instead.
 *
 *   said                  what this boot has said, to the screen
 *   said NAME             the same, into NAME on the volume
 *   said before           what the boot before this one said
 *   said before NAME      the same, into NAME
 *
 * "before" reads the carry-over window (klib_logring.h). ‼ MEASURED ON THE
 * BOARD: that window is EMPTY on an i5-9400F/B365, because the firmware
 * rewrites memory on every start, warm reset included. It survives in QEMU and
 * on firmware that does not retrain; where it does not, this says so in one
 * sentence instead of handing back an empty file. That is the whole reason the
 * first half of this command — the log of the boot you are standing in — is
 * the one that matters here.
 */
#include "commands.h"
#include "box/print.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"

/* One page, which is what the kernel hands over per call regardless, and the
 * shell's stack is not somewhere to put four kilobytes. */
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
            /* Named, and then it prints anyway. A volume that will not take
             * the file is the commonest reason anybody types this at all;
             * refusing to show the log because it cannot also store it would
             * be failing in exactly the case this was written for. */
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
            /* print_bytes, not println: the log carries its own newlines and
             * adding any would change what the machine actually said. */
            print_bytes(s_pail, (size_t)n);
            poured += (uint64_t)n;
        }
    }

    /* Asked BEFORE the handle is let go. A gap that reads as continuous is
     * worse than one that is announced, because it will be believed. */
    uint64_t lost = current_lost(log);

    if (out) current_release(out);
    current_release(log);

    if (lost) {
        printf("\n%lu byte(s) had already been overwritten in the ring "
               "before this could read them.\n", (unsigned long)lost);
    }

    if (failed) return 1;

    if (poured == 0) {
        /* Zero is an ANSWER, not a failure, and for the previous boot it has
         * three ordinary causes worth naming so nobody hunts a fourth. */
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

    /*
     * ‼ SAID AGAIN, HERE, AT THE BOTTOM.
     *
     * The refusal is already printed above — and above is where the whole log
     * then gets poured on top of it. On the board that is a thousand lines, so
     * the one number that says WHY the file was not written scrolls off the top
     * of the screen before the command has finished running, at exactly the
     * moment somebody is standing there with a camera. Measured: the run
     * happened, the reason went with it, and the next session had to guess.
     *
     * Two lines of output to make a fact reachable is not a cost worth
     * thinking about.
     */
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
