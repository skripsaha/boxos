#ifndef VIEW_H
#define VIEW_H

#include "box/defs.h"
#include "box/types.h"
#include "box/file.h"

/* What a line being typed can see: the commands, the files, and the tags the
 * files wear, read from the volume once for the line, the first time each is
 * wanted, and let go with the line. Each kind of name stands on a shelf of
 * its own — sorted, each name once — so a name is found by halving and the
 * names that begin with a word stand side by side. */

typedef enum {
    VIEW_COMMANDS = 0,          /* the shell's own and every program's, as typed */
    VIEW_PROGRAM_FILES,         /* every program's and Itinerary's file */
    VIEW_FILES_HERE,            /* the files in the Use Context */
    VIEW_FILES_EVERYWHERE,      /* the files on the whole volume */
    VIEW_TAGS_HERE,             /* the tags of the files in the Use Context, but its own */
    VIEW_TAGS_EVERYWHERE,       /* the tags of the whole volume */
    VIEW_SHELVES
} ViewShelfKind;

typedef struct {
    const char **Names;
    size_t       Count;
} ViewShelf;

typedef struct ViewCommand ViewCommand;
typedef struct ViewFile    ViewFile;

typedef struct {
    const char  *Context;           /* the Use Context's tags, comma-joined; NULL for none */
    ViewCommand *Commands;
    size_t       CommandCount;
    bool         CommandsRead;
    ViewFile    *Files[2];          /* in the Use Context, on the whole volume */
    size_t       FileCount[2];
    bool         FilesRead[2];
    ViewShelf    Shelves[VIEW_SHELVES];
    bool         ShelvesBuilt[VIEW_SHELVES];
    TagListed   *Listed[2];         /* the volume's list of tags: in the Use Context, everywhere */
    size_t       ListedCount[2];
    bool         ListedRead[2];
} ShellView;

/* A view of the volume from the Use Context `context` (comma-joined tags,
 * NULL for none), which outlives it. */
void ViewBegin(ShellView *view, const char *context);
void ViewEnd(ShellView *view);

const ViewShelf *ViewShelfOf(ShellView *view, ViewShelfKind kind);

/* The first of the shelf's names that does not come before the `length`
 * bytes of `name`: where the names that begin with them start. */
size_t ViewFrom(const ViewShelf *shelf, const char *name, size_t length);

/* The shelf holds the `length` bytes of `name` as one of its names. */
bool ViewHolds(const ViewShelf *shelf, const char *name, size_t length);

/* The program file a command's name means, the first of them when several
 * do; 0 for a name no program on the volume has. */
file_id_t ViewProgram(ShellView *view, const char *name);

/* As BoxFormLookup's Find and All answer, from what the view holds: the files
 * called `name`, and every file — in the Use Context, or on the whole volume
 * when everywhere. A heap array the caller frees. */
int ViewFind(ShellView *view, const char *name, bool everywhere, file_id_t **ids);
int ViewAll(ShellView *view, bool everywhere, file_id_t **ids);

/* `items` sorted in place by `order` (negative: a before b), equals kept in
 * the order they came. False, and nothing moved, when there is no memory for
 * the sorting. */
typedef int (*ViewOrder)(const void *a, const void *b);
bool ViewSort(const void **items, size_t count, ViewOrder order);

#endif /* VIEW_H */
