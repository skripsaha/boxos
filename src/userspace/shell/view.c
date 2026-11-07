/*
 * view.c — what a line being typed can see.
 *
 * The volume is asked once for the line, and only for what the line wants:
 * the commands when a command's name is typed, the files of the Use Context
 * when a command takes files, the whole volume only after `everywhere`. What
 * is read is put on shelves, so every key after that finds a name by halving
 * however many the volume holds.
 */

#include "view.h"
#include "program.h"
#include "shell.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"

extern const ShellCommand g_commands[];

struct ViewCommand {
    char      *Name;        /* as it is typed */
    char      *Filename;    /* NULL for the shell's own */
    file_id_t  Id;          /* 0 for the shell's own */
};

struct ViewFile {
    char      *Name;
    char      *Tags;        /* comma-joined, NULL for none */
    file_id_t  Id;
};

static char *Copy(const char *text, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

/* Room for one more item of `size` in *items, which holds `count` of *room:
 * doubled as it fills. False when memory is out. */
static bool Room(void **items, size_t *room, size_t count, size_t size)
{
    if (count < *room) return true;
    size_t grown = *room ? *room * 2 : 32;
    void  *more  = realloc(*items, grown * size);
    if (!more) return false;
    *items = more;
    *room  = grown;
    return true;
}

void ViewBegin(ShellView *view, const char *context)
{
    memset(view, 0, sizeof(*view));
    view->Context = context;
}

/* ---- the commands ------------------------------------------------------ */

typedef struct {
    ShellView *View;
    size_t     Room;
} Gathering;

static void AddCommand(Gathering *gathering, const char *name, size_t name_len,
                       const char *filename, file_id_t id)
{
    ShellView *view = gathering->View;
    if (!Room((void **)&view->Commands, &gathering->Room, view->CommandCount, sizeof(ViewCommand)))
        return;
    ViewCommand *command = &view->Commands[view->CommandCount];
    command->Name     = Copy(name, name_len);
    command->Filename = filename ? Copy(filename, strlen(filename)) : NULL;
    command->Id       = id;
    if (!command->Name || (filename && !command->Filename)) {
        free(command->Name);
        free(command->Filename);
        return;
    }
    view->CommandCount++;
}

static void GatherProgram(void *context, file_id_t id, const char *filename)
{
    AddCommand(context, filename, ProgramStemLength(filename), filename, id);
}

static void ReadCommands(ShellView *view)
{
    if (view->CommandsRead) return;
    view->CommandsRead = true;

    Gathering gathering = { view, 0 };
    for (int i = 0; g_commands[i].name != NULL; i++)
        AddCommand(&gathering, g_commands[i].name, strlen(g_commands[i].name), NULL, 0);
    ProgramEachCommand(GatherProgram, &gathering);
}

file_id_t ViewProgram(ShellView *view, const char *name)
{
    ReadCommands(view);
    for (size_t i = 0; i < view->CommandCount; i++) {
        if (view->Commands[i].Id != 0 && strcmp(view->Commands[i].Name, name) == 0)
            return view->Commands[i].Id;
    }
    return 0;
}

/* ---- the files --------------------------------------------------------- */

static void ReadFiles(ShellView *view, bool everywhere)
{
    int where = everywhere ? 1 : 0;
    if (view->FilesRead[where]) return;
    view->FilesRead[where] = true;

    file_id_t *ids   = NULL;
    int        found = everywhere ? query_all_everywhere(NULL, &ids) : query_all(NULL, &ids);
    ViewFile  *files = found > 0 ? calloc((size_t)found, sizeof(ViewFile)) : NULL;
    size_t     kept  = 0;
    for (int i = 0; files && i < found; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(ids[i], &metadata) != 0) continue;
        if (metadata.Filename && !(metadata.Flags & FILE_FLAG_TRASHED)) {
            ViewFile *file = &files[kept];
            file->Name = Copy(metadata.Filename, strlen(metadata.Filename));
            file->Tags = metadata.Tags ? Copy(metadata.Tags, strlen(metadata.Tags)) : NULL;
            file->Id   = ids[i];
            if (file->Name && (!metadata.Tags || file->Tags)) {
                kept++;
            } else {
                free(file->Name);
                free(file->Tags);
            }
        }
        FileMetadataRelease(&metadata);
    }
    free(ids);
    view->Files[where]     = files;
    view->FileCount[where] = kept;
}

/* The ids of the files called `name` — of all of them, with no name. */
static int Collect(const ViewFile *files, size_t count, const char *name, file_id_t **ids)
{
    *ids = NULL;
    size_t found = 0;
    for (size_t i = 0; i < count; i++) found += !name || strcmp(files[i].Name, name) == 0;
    if (found == 0) return 0;

    *ids = malloc(found * sizeof(file_id_t));
    if (!*ids) return 0;
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        if (!name || strcmp(files[i].Name, name) == 0) (*ids)[at++] = files[i].Id;
    }
    return (int)found;
}

int ViewFind(ShellView *view, const char *name, bool everywhere, file_id_t **ids)
{
    ReadFiles(view, everywhere);
    return Collect(view->Files[everywhere], view->FileCount[everywhere], name, ids);
}

int ViewAll(ShellView *view, file_id_t **ids)
{
    ReadFiles(view, false);
    return Collect(view->Files[0], view->FileCount[0], NULL, ids);
}

/* ---- sorting ----------------------------------------------------------- */

/* Bottom-up merge sort: runs of `width` merged in pairs through a spare
 * array, the left run first among equals. */
bool ViewSort(const void **items, size_t count, ViewOrder order)
{
    if (count < 2) return true;
    const void **spare = malloc(count * sizeof(void *));
    if (!spare) return false;
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t low = 0; low < count; low += 2 * width) {
            size_t middle = low + width < count ? low + width : count;
            size_t high   = low + 2 * width < count ? low + 2 * width : count;
            size_t left = low, right = middle, out = low;
            while (left < middle && right < high)
                spare[out++] = order(items[right], items[left]) < 0 ? items[right++] : items[left++];
            while (left < middle) spare[out++] = items[left++];
            while (right < high) spare[out++] = items[right++];
        }
        memcpy(items, spare, count * sizeof(void *));
    }
    free(spare);
    return true;
}

/* ---- shelves ----------------------------------------------------------- */

typedef struct {
    const char **Names;
    size_t       Count;
    size_t       Room;
} Pile;

static void Put(Pile *pile, const char *name)
{
    if (Room((void **)&pile->Names, &pile->Room, pile->Count, sizeof(char *)))
        pile->Names[pile->Count++] = name;
}

/* The comma-joined `tags` laid on the pile, each a string of the view's own
 * — but those in `worn`. */
static void PutTags(ShellView *view, Pile *pile, const char *tags, const char *worn)
{
    while (*tags) {
        const char *comma  = strchr(tags, ',');
        size_t      length = comma ? (size_t)(comma - tags) : strlen(tags);
        bool        own    = false;
        for (const char *w = worn; w && *w && !own;) {
            const char *next = strchr(w, ',');
            size_t      n    = next ? (size_t)(next - w) : strlen(w);
            own = n == length && memcmp(w, tags, length) == 0;
            w   = next ? next + 1 : w + n;
        }
        if (length > 0 && !own &&
            Room((void **)&view->Tags, &view->TagRoom, view->TagCount, sizeof(char *))) {
            char *tag = Copy(tags, length);
            if (tag) {
                view->Tags[view->TagCount++] = tag;
                Put(pile, tag);
            }
        }
        if (!comma) break;
        tags = comma + 1;
    }
}

static void Gather(ShellView *view, ViewShelfKind kind, Pile *pile)
{
    bool everywhere = kind == VIEW_FILES_EVERYWHERE || kind == VIEW_TAGS_EVERYWHERE;
    if (kind == VIEW_COMMANDS || kind == VIEW_PROGRAM_FILES) {
        ReadCommands(view);
        for (size_t i = 0; i < view->CommandCount; i++) {
            const char *name = kind == VIEW_COMMANDS ? view->Commands[i].Name : view->Commands[i].Filename;
            if (name) Put(pile, name);
        }
        return;
    }

    ReadFiles(view, everywhere);
    const char *worn = everywhere ? NULL : view->Context;
    for (size_t i = 0; i < view->FileCount[everywhere]; i++) {
        const ViewFile *file = &view->Files[everywhere][i];
        if (kind == VIEW_FILES_HERE || kind == VIEW_FILES_EVERYWHERE) Put(pile, file->Name);
        else if (file->Tags) PutTags(view, pile, file->Tags, worn);
    }
}

static int NameOrder(const void *a, const void *b)
{
    return strcmp(a, b);
}

const ViewShelf *ViewShelfOf(ShellView *view, ViewShelfKind kind)
{
    ViewShelf *shelf = &view->Shelves[kind];
    if (view->ShelvesBuilt[kind]) return shelf;
    view->ShelvesBuilt[kind] = true;

    Pile pile = { NULL, 0, 0 };
    Gather(view, kind, &pile);
    if (!ViewSort((const void **)pile.Names, pile.Count, NameOrder)) {
        free(pile.Names);
        return shelf;
    }
    size_t kept = 0;
    for (size_t i = 0; i < pile.Count; i++) {
        if (kept == 0 || strcmp(pile.Names[kept - 1], pile.Names[i]) != 0)
            pile.Names[kept++] = pile.Names[i];
    }
    shelf->Names = pile.Names;
    shelf->Count = kept;
    return shelf;
}

/* `entry` against the `length` bytes of `name`, as strcmp orders them. */
static int Order(const char *entry, const char *name, size_t length)
{
    int order = strncmp(entry, name, length);
    if (order != 0) return order;
    return entry[length] == '\0' ? 0 : 1;
}

size_t ViewFrom(const ViewShelf *shelf, const char *name, size_t length)
{
    size_t low = 0, high = shelf->Count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (Order(shelf->Names[middle], name, length) < 0) low = middle + 1;
        else                                               high = middle;
    }
    return low;
}

bool ViewHolds(const ViewShelf *shelf, const char *name, size_t length)
{
    size_t at = ViewFrom(shelf, name, length);
    return at < shelf->Count && Order(shelf->Names[at], name, length) == 0;
}

void ViewEnd(ShellView *view)
{
    for (size_t i = 0; i < view->CommandCount; i++) {
        free(view->Commands[i].Name);
        free(view->Commands[i].Filename);
    }
    free(view->Commands);
    for (int where = 0; where < 2; where++) {
        for (size_t i = 0; i < view->FileCount[where]; i++) {
            free(view->Files[where][i].Name);
            free(view->Files[where][i].Tags);
        }
        free(view->Files[where]);
    }
    for (int kind = 0; kind < VIEW_SHELVES; kind++) free(view->Shelves[kind].Names);
    for (size_t i = 0; i < view->TagCount; i++) free(view->Tags[i]);
    free(view->Tags);
    memset(view, 0, sizeof(*view));
}
