/*
 * use — say what the person at this machine is doing, in tags.
 *
 *   use code cpp      the Use Context is now {code, cpp}: files are seen and
 *                     created inside it, programs wearing both tags run in the
 *                     scheduler's context tier
 *   use               the context is cleared
 *
 * The volume remembers it: what is said here is written to the mounted volume
 * and taken up again at the next boot with that volume. When the volume
 * cannot remember (no volume, a medium that will not take the write, a
 * context longer than its record holds) the context is set all the same and
 * the person is told.
 *
 * The context is the kernel's, one per machine; the shell only speaks for the
 * user. The prompt is rebuilt from what the kernel holds, never from a copy
 * of what was typed, so a nested shell shows the context it was born into.
 */

#include "commands.h"
#include "../shell.h"
#include "box/print.h"
#include "box/use.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"

int cmd_use(int argc, char *argv[])
{
    int  rc;
    bool remembered = false;

    if (argc == 1) {
        rc = use_clear(&remembered);
    } else {
        /* One comma-joined list, sized to what was typed. */
        size_t total = 0;
        for (int i = 1; i < argc; i++) total += strlen(argv[i]) + 1;

        char *list = malloc(total);
        if (!list) {
            println("use: no memory for the tag list");
            return 1;
        }
        size_t pos = 0;
        for (int i = 1; i < argc; i++) {
            size_t len = strlen(argv[i]);
            if (i > 1) list[pos++] = ',';
            memcpy(list + pos, argv[i], len);
            pos += len;
        }
        list[pos] = '\0';
        rc = use_set(list, &remembered);
        free(list);
    }

    ShellUpdatePrompt();

    if (rc < 0) {
        error_t why = box_errno_of(rc);
        if (why == ERR_ACCESS_DENIED)
            println("use: only the shell and system programs may set the Use Context");
        else if (why == ERR_INVALID_ARGUMENT)
            println("use: a tag is longer than a tag may be");
        else
            printf("use: refused (error %d)\n", (int)why);
        return 1;
    }
    /* The volume remembers the context across the night; when it cannot, the
     * person hears it now rather than at the next boot. */
    if (argc == 1)
        println(remembered ? "Context cleared"
                           : "Context cleared; this volume still remembers the old one");
    else
        println(remembered ? "Context set"
                           : "Context set; this volume will not remember it");
    return 0;
}
