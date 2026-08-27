/* logsave — write what the kernel has said onto the volume.
 *
 * A machine on a bench has no serial cable, and its whole account of itself is
 * whatever is still on the screen. This pours the kernel's log ring into a
 * file so the account survives the scroll, the photograph and the reboot.
 *
 * It is two Currents and nothing else: `log:file` opened for reading,
 * `file:NAME` opened for writing, one poured into the other. No door of its
 * own — the kernel already had one, and the spine already knew how to carry
 * bytes.
 *
 *   logsave            -> watch.log
 *   logsave NAME       -> NAME
 *
 * A kernel built without `PRINTTOFILE=on` keeps no ring, refuses the read
 * open, and is told so by name rather than handing back an empty file.
 */
#include "box/print.h"
#include "box/ipc.h"
#include "box/current.h"
#include "box/error.h"
#include "box/string.h"
#include "box/system.h"

#define LOGSAVE_DEFAULT_NAME  "watch.log"

/* Big enough that a megabyte of log is a few hundred passes, small enough to
 * stay off a userspace stack — the ring is read a page at a time behind this
 * anyway, so a larger pail would not fill any faster. */
static char s_pail[4096];
static char s_argv[16][64];
static char s_target[128];

int main(void)
{
    int argc;
    receive_args(&argc, s_argv, 16);

    const char *name = LOGSAVE_DEFAULT_NAME;
    if (argc > 1 && s_argv[1][0] != '\0') {
        name = s_argv[1];
    }

    error_t  why = OK;
    Current *log = current_open_ex("log:file", CURRENT_READ, 0, 0, &why);
    if (!log) {
        if (why == ERR_UNSUPPORTED) {
            println("This kernel does not keep what it says. Build it with "
                    "PRINTTOFILE=on and the log will be here.");
        } else {
            printf("The log could not be opened for reading (error %u)\n",
                   (unsigned)why);
        }
        exit(1);
        return 1;
    }

    /* "file:" + the name, in a buffer of our own — the tag is what names the
     * backing, and building it wrong is how a Current opens something else. */
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
        if (n == CURRENT_CLOSED) break;          /* the log ended where we began */
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

    /* Asked BEFORE the handle is let go, and said out loud whether or not it
     * is zero-worthy: a log with a hole in it that reads as continuous is
     * worse than no log, because it will be believed. */
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
