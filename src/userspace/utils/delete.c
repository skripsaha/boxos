/* delete — files gone for good, after a look at what goes and a yes.
 *
 *   delete notes.txt              that file
 *   delete them                   what the last command gave: after `trash list`,
 *                                 everything in the trash
 *   delete old.txt                a file in the trash, by its name
 *
 * Any file, in the trash or out of it. What goes is shown first — each file's
 * name, size and tags — with what is worth a warning: a file out of the trash
 * does not go there, it is gone for good, and a program among them will not
 * start again. Nothing is deleted until the person says yes; with nobody at
 * the screen to say it — started behind, or by another program — nothing is
 * (box/form.h form_person_here). A file the system keeps the volume refuses,
 * and it stays; the rest go. trash is the gentle way: what is there can be
 * taken back.
 *
 * A name the shell found no file for among those in use comes as a word, and
 * is looked for among the files kept deleted: the shell's look-up, like a
 * listing, does not meet them.
 *
 * Its exit: 0 when everything asked for went, or the person said no; 1 when
 * something could not go, or nobody was there to be asked.
 */

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/program_part.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("takes optional things \"the files to delete\"\n"
         "takes optional words \"the name of a file in the trash\"\n");

/* How many files are shown before the rest are counted. */
#define SHOWN_FILES 12u

typedef struct {
    file_id_t *Ids;
    size_t     Count;
    size_t     Room;
} Going;

/* Into what goes, once each. False with no room. */
static bool going_add(Going *going, file_id_t id)
{
    for (size_t i = 0; i < going->Count; i++)
        if (going->Ids[i] == id) return true;
    if (going->Count == going->Room) {
        size_t     room  = going->Room ? going->Room * 2 : 16;
        file_id_t *grown = realloc(going->Ids, room * sizeof(file_id_t));
        if (!grown) return false;
        going->Ids  = grown;
        going->Room = room;
    }
    going->Ids[going->Count++] = id;
    return true;
}

/* A name looked for in the trash: the one file there called so goes. False
 * when it cannot: none, or more than one — said. */
static bool from_trash(Going *going, const file_id_t *trashed, int total, const char *name)
{
    file_id_t found = 0;
    int       count = 0;
    for (int i = 0; i < total; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(trashed[i], &metadata) != 0) continue;
        if (metadata.Filename && strcmp(metadata.Filename, name) == 0) {
            found = trashed[i];
            count++;
        }
        FileMetadataRelease(&metadata);
    }
    if (count == 0) {
        printf("There is no file called %s\n", name);
        return false;
    }
    if (count > 1) {
        printf("%s is in the trash %d times; trash list shows them, and delete them takes them all\n", name, count);
        return false;
    }
    if (!going_add(going, found)) {
        println("delete: there is no memory to keep what goes");
        return false;
    }
    return true;
}

static void say_size(uint64_t bytes)
{
    if (bytes < 1024) printf("%llu bytes", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) printf("%llu KiB", (unsigned long long)(bytes / 1024));
    else printf("%llu MiB", (unsigned long long)(bytes / (1024ull * 1024)));
}

/* What goes, one line a file, and what is worth a warning. */
static void show_going(const Going *going)
{
    size_t out = 0, programs = 0;
    for (size_t i = 0; i < going->Count; i++) {
        FileMetadata metadata;
        bool         read = FileMetadataRead(going->Ids[i], &metadata) == 0;
        bool         kept    = read && metadata.KeptUntil != 0;
        ProgramKind  kind    = PROGRAM_NONE;
        bool         program = read && program_kind(going->Ids[i], &kind) == 0 && kind != PROGRAM_NONE;
        out += !kept;
        programs += program;
        if (i < SHOWN_FILES) {
            printf("  %s  ", read && metadata.Filename ? metadata.Filename : "a file with no name");
            if (read) {
                say_size(metadata.Size);
                printf("  %s%s\n", metadata.Tags ? metadata.Tags : "", program ? "  — a program" : "");
            } else {
                printf("(cannot be read)\n");
            }
        }
        if (read) FileMetadataRelease(&metadata);
    }
    if (going->Count > SHOWN_FILES) printf("  and %zu more\n", going->Count - SHOWN_FILES);
    if (out == going->Count)
        printf("%s not go to the trash: deleted, %s gone for good\n", going->Count == 1 ? "It does" : "They do",
               going->Count == 1 ? "it is" : "they are");
    else if (out > 0)
        printf("%zu of them %s not in the trash: deleted, %s gone for good\n", out, out == 1 ? "is" : "are",
               out == 1 ? "it is" : "they are");
    if (programs > 0)
        printf("%s: once deleted, %s will not start again\n", programs == 1 ? "One is a program" : "Some are programs",
               programs == 1 ? "it" : "they");
}

/* Yes or no from the person at the screen. */
static bool agreed(size_t count)
{
    for (;;) {
        printf("delete %s? (yes or no) ", count == 1 ? "it" : "them");
        char *typed = NULL;
        if (readline_grow(&typed) < 0) {
            println("");
            return false;
        }
        size_t from = 0, to = strlen(typed);
        while (from < to && typed[from] == ' ') from++;
        while (to > from && typed[to - 1] == ' ') to--;
        typed[to] = '\0';
        bool yes = strcmp(typed + from, "yes") == 0 || strcmp(typed + from, "y") == 0;
        bool no  = strcmp(typed + from, "no") == 0 || strcmp(typed + from, "n") == 0;
        free(typed);
        if (yes || no) return yes;
        printf("please answer yes or no. ");
    }
}

/* Each file deleted; one the system keeps stays, and is said. True when every
 * one went. */
static bool delete_all(const Going *going)
{
    size_t deleted = 0, stayed = 0;
    for (size_t i = 0; i < going->Count; i++) {
        FileMetadata metadata;
        bool         named = FileMetadataRead(going->Ids[i], &metadata) == 0 && metadata.Filename;
        const char  *name  = named ? metadata.Filename : "a file";
        int          rc    = delete(going->Ids[i]);
        if (rc == 0) {
            deleted++;
        } else {
            stayed++;
            if (box_errno_of(rc) == ERR_PERMISSION_DENIED) printf("%s is kept by the system; it stays\n", name);
            else printf("%s could not be deleted (error %d); it stays\n", name, (int)box_errno_of(rc));
        }
        if (named) FileMetadataRelease(&metadata);
    }
    printf("Deleted %zu file%s\n", deleted, deleted == 1 ? "" : "s");
    return stayed == 0;
}

int main(void)
{
    const file_id_t   *things = NULL;
    const char *const *words  = NULL;
    size_t             thing_count = 0, word_count = 0;
    if (form_things(0, &things, &thing_count) != 0 || form_words(1, &words, &word_count) != 0) {
        exit(1);
        return 1;
    }
    if (thing_count == 0 && word_count == 0) {
        println("delete: say which files — their names, or them (trash list gives what is in the trash)");
        exit(1);
        return 1;
    }

    Going going = { NULL, 0, 0 };
    bool  whole = true;
    for (size_t i = 0; i < thing_count && whole; i++) whole = going_add(&going, things[i]);

    file_id_t *trashed = NULL;
    int        total   = word_count ? query_all_deleted(NULL, NULL, &trashed) : 0;
    if (total < 0) {
        println("delete: the trash could not be looked in");
        whole = false;
        total = 0;
    }
    for (size_t i = 0; i < word_count; i++) {
        whole &= from_trash(&going, trashed, total, words[i]);
    }
    free(trashed);

    int status = whole ? 0 : 1;
    if (going.Count > 0) {
        show_going(&going);
        if (!form_person_here()) {
            println("delete: nobody is at the screen to say yes, so nothing was deleted");
            status = 1;
        } else if (!agreed(going.Count)) {
            println("Nothing was deleted");
        } else if (!delete_all(&going)) {
            status = 1;
        }
    }
    free(going.Ids);
    exit(status);
    return status;
}
