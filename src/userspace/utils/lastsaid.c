/* lastsaid — what the machine said the LAST time it ran.
 *
 * `logsave` exists and is untouched: it pours THIS boot's log onto the volume.
 * That is the right tool right up until the moment it is needed most, and this
 * one was written because that moment arrived. A board wedged itself — a
 * storming interrupt, a host controller out of slots — and the volume was
 * exactly the thing that had stopped answering. There was no way to ask the
 * machine what had happened to it except to photograph a scrolling screen.
 *
 * So the kernel now carries its log through a reset in a fixed window of
 * memory it does not clear at boot (klib_logring.h), and this reads it.
 *
 *   lastsaid           -> prints it, which needs no filesystem at all
 *   lastsaid NAME      -> writes it to NAME on the volume
 *
 * The no-argument form is the point. On a machine whose volume is the failure,
 * a tool that can only write a file is a tool that cannot be used.
 *
 * ‼ WHAT IT CAN AND CANNOT SEE, so nobody trusts it further than it goes:
 *   - it sees the previous run after a WARM reset — the RESET button, a
 *     reboot, a triple fault. Memory keeps its contents across those.
 *   - it sees NOTHING after power was removed. Holding the power button for
 *     four seconds cuts the rails and takes RAM with them. On a wedged
 *     machine, press RESET, not power.
 *   - a kernel built without PRINTTOFILE=on keeps no ring at all, and says so
 *     by name rather than handing back an empty file.
 */
#include "box/print.h"
#include "box/luggage.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"
#include "box/system.h"

/* Same size and the same reason as logsave's: the kernel hands over a page per
 * call regardless, so a larger pail would not fill any faster, and this one
 * has to stay off a userspace stack. */
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
            println("This kernel does not keep what it says. Build it with "
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
            /* Named, and then it prints instead. A volume that will not take
             * the file is the commonest reason anybody runs this at all, and
             * refusing to show the log because it cannot also store it would
             * be the tool failing in exactly the case it was built for. */
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
            /* Straight to the screen, one pail at a time. write_bytes rather
             * than println because the log holds its own newlines and adding
             * any would change what the machine actually said. */
            print_bytes(s_pail, (size_t)n);
            poured += (uint64_t)n;
        }
    }

    /* Asked BEFORE the handle is let go. A gap that reads as continuous is
     * worse than a gap that is announced, because it will be believed. */
    uint64_t lost = current_lost(log);

    if (out) current_release(out);
    current_release(log);

    if (lost) {
        printf("\n%lu byte(s) of the previous run had already been "
               "overwritten in its own ring before it ended.\n",
               (unsigned long)lost);
    }

    if (failed) { exit(1); return 1; }

    /* Zero is an ANSWER, not a failure, and it has three ordinary causes
     * worth naming so the operator does not go looking for a fourth. */
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
