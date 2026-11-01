/*
 * use — say what the person at this machine is doing, in tags.
 *
 *   use code cpp      the Use Context is now {code, cpp}: files are seen and
 *                     created inside it, programs wearing both tags run in the
 *                     scheduler's context tier
 *   use +draft        draft is added to it
 *   use -cpp          cpp is taken out of it
 *   use +draft -cpp   both, in one word: nobody ever sees one done and not
 *                     the other
 *   use               the context is cleared
 *
 * Plain words say the whole context; + and - change the one that stands. The
 * two do not mix in one line — which of them was meant would be a guess.
 *
 * The volume remembers it: what is said here is written to the mounted volume
 * and taken up again at the next boot with that volume. When the volume
 * cannot remember (no volume, no ground on it for the context, a medium that
 * will not take the write) the context is set all the same and the person is
 * told.
 *
 * The context is the kernel's, one per machine; the shell only speaks for the
 * user, and its prompt reads the context from the kernel before every line.
 */

#include "commands.h"
#include "../shell.h"
#include "box/print.h"
#include "box/use.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"

/* The words of the line that begin with `sign` — '+' or '-', the sign left
 * off — or, with sign 0, the words that begin with neither, comma-joined into
 * a list sized to them; "" when there are none. NULL when memory is out. */
static char *UseWords(int argc, char *argv[], char sign)
{
    size_t total = 1;
    for (int i = 1; i < argc; i++) total += strlen(argv[i]) + 1;

    char *list = malloc(total);
    if (!list) return NULL;
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        char        first = argv[i][0];
        bool        sign_word = first == '+' || first == '-';
        const char *word = argv[i] + (sign ? 1 : 0);
        if (sign ? first != sign : sign_word) continue;
        size_t len = strlen(word);
        if (pos > 0) list[pos++] = ',';
        memcpy(list + pos, word, len);
        pos += len;
    }
    list[pos] = '\0';
    return list;
}

/* What the line asks: 0 the whole context, 1 a change; -1 when it cannot be
 * read, and why was said. */
static int UseReadLine(int argc, char *argv[])
{
    bool plain = false, signed_words = false;
    for (int i = 1; i < argc; i++) {
        char first = argv[i][0];
        if (first != '+' && first != '-') {
            plain = true;
            continue;
        }
        if (argv[i][1] == '\0') {
            println("use: a sign wants a tag right after it: use +draft, use -cpp");
            return -1;
        }
        signed_words = true;
    }
    if (plain && signed_words) {
        println("use: say the whole context, or change it with + and -, not both at once: "
                "use code cpp, or use +draft -cpp");
        return -1;
    }
    return signed_words ? 1 : 0;
}

static bool UseSame(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int cmd_use(int argc, char *argv[])
{
    int  rc;
    bool remembered = false;
    bool unchanged  = false;

    int asked = argc == 1 ? 0 : UseReadLine(argc, argv);
    if (asked < 0) return 1;

    if (argc == 1) {
        rc = use_clear(&remembered);
    } else if (asked == 0) {
        char *list = UseWords(argc, argv, 0);
        if (!list) {
            println("use: no memory for the tag list");
            return 1;
        }
        rc = use_set(list, &remembered);
        free(list);
    } else {
        char *add  = UseWords(argc, argv, '+');
        char *drop = UseWords(argc, argv, '-');
        if (!add || !drop) {
            free(add);
            free(drop);
            println("use: no memory for the tag lists");
            return 1;
        }
        /* The context before and after, as the kernel spells it: a change
         * that changed nothing is said so. */
        char *before = NULL, *after = NULL;
        bool  seen   = ShellContextWhole(&before) >= 0;
        rc = use_change(add, drop, &remembered);
        if (rc == 0 && seen && ShellContextWhole(&after) >= 0)
            unchanged = UseSame(before, after);
        free(before);
        free(after);
        free(add);
        free(drop);
    }

    if (rc < 0) {
        error_t why = box_errno_of(rc);
        if (why == ERR_ACCESS_DENIED)
            println("use: only the shell and system programs may set the Use Context");
        else if (why == ERR_INVALID_ARGUMENT || why == ERR_TAGFS_TAG_TOO_LONG)
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
    else if (unchanged)
        println("Context unchanged");
    else
        println(remembered ? "Context set"
                           : "Context set; this volume will not remember it");
    return 0;
}
