/*
 * autostart — the programs the person wants running whenever the machine
 * comes up, and the way to change which they are.
 *
 * The system starts three programs of its own — the display, the hub and this
 * one. The list is this one's setting `programs`, program names apart by
 * blanks (a program's file name, or its name before the dot), read from the
 * Settings tree like any program's settings (box/settings.h):
 *
 *     autostart                  the list as it stands; what it names is what
 *                                `them` is for the next command
 *     autostart add clock pulse  put programs on the list
 *     autostart remove them      take programs off it — `them` is what the
 *                                last command gave, `autostart` alone too
 *     autostart add              nothing said: it shows the programs the
 *                                volume has that are not on the list and asks
 *                                which (with a person at the screen to answer)
 *
 * The programs are things — files, held to what they hold: a file is a
 * program when it is a box or an ELF image (box/program_part.h program_kind),
 * whatever it is called or tagged. A name on the list that no program has any
 * more is shown, and `settings autostart programs` mends it.
 *
 * ‼ THE LIST IS STARTED ONLY WHEN THE SYSTEM STARTS THIS PROGRAM — at the
 * machine's start, from its start set (the kernel's autostart.c). That is
 * told by who started it, which the kernel writes in the program's cabin and
 * no program can put there: a program started by the system has no spawner
 * (started_by_system, box/system.h). Run from the hub, or by any program, it
 * never starts anything: it shows and changes the list. When it does start
 * the list, each program is started on its own and none waits for another: a
 * program that is already running is skipped and said — the list is the
 * machine's start, not a second copy of what runs — and one that will not
 * start (no such file, two files of that name, the machine full) is said, and
 * the rest still start. A program whose box says rise is raised again by the
 * system if it falls, as one the system started itself (rise.h).
 *
 * What starts as it should is the system's log's to keep, not the person's to
 * read: a program started, one that runs already, an empty list go into the
 * log (kdbg_print — the wire, logsave, the Emergency Screen), which the hub's
 * screen does not show. A program on the list that will not start is said to
 * the person: the list asks for something the machine could not do.
 *
 * Its exit: 0 when everything asked of it was done (at the start: every
 * program on the list runs), 1 when something was not.
 */

#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/program_part.h"
#include "box/settings.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("takes optional name \"what to do: add or remove; leave it out to see the list\"\n"
         "takes optional things \"the programs to add or remove: names, them; asked for when add or remove has none\"\n"
         "gives things\n");
BOX_SETTINGS("programs text default \"clock pulse gardener\" "
             "\"the programs started when the machine comes up, their names apart by blanks\"\n");

/* ---- names ------------------------------------------------------------ */

typedef struct {
    char  **Items;
    size_t  Count;
} Names;

static char *Copy(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static bool NamesAdd(Names *names, const char *name)
{
    char **grown = realloc(names->Items, (names->Count + 1) * sizeof(*grown));
    if (!grown) return false;
    names->Items = grown;
    char *copy = Copy(name, strlen(name));
    if (!copy) return false;
    names->Items[names->Count++] = copy;
    return true;
}

static void NamesRelease(Names *names)
{
    for (size_t i = 0; i < names->Count; i++) free(names->Items[i]);
    free(names->Items);
    names->Items = NULL;
    names->Count = 0;
}

/* The words of `text` — blanks apart, "double quotes" grouping — into names. */
static bool NamesCut(Names *names, const char *text)
{
    size_t size = strlen(text) + 1;
    char  *line = Copy(text, size - 1);
    char **words = malloc((size / 2 + 1) * sizeof(*words));
    bool   whole = line && words;
    if (whole) {
        uint32_t count = luggage_cut(line, words, (uint32_t)(size / 2 + 1));
        for (uint32_t i = 0; whole && i < count; i++) whole = NamesAdd(names, words[i]);
    }
    free(words);
    free(line);
    return whole;
}

/* ---- programs --------------------------------------------------------- */

static bool IsProgram(file_id_t file)
{
    ProgramKind kind;
    return program_kind(file, &kind) == 0 && kind != PROGRAM_NONE;
}

/* The length of a file name before its last dot — the whole name when it has none. */
static size_t StemLength(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? (size_t)(dot - name) : strlen(name);
}

/* The program a name means: `name` as the volume files it — "clock.box" — or,
 * with no dot, the name before the dot of one; on the whole volume, among the
 * files that are programs (clock.settings has the stem clock and is none).
 * 0 and *why, a sentence's end, when there is none or more than one. */
static file_id_t FindProgram(const char *name, const char **why)
{
    bool   whole  = strrchr(name, '.') != NULL;
    size_t length = StemLength(name);
    char   stem[64];
    if (length == 0 || length >= sizeof(stem)) {
        *why = "that is no program's name";
        return 0;
    }
    memcpy(stem, name, length);
    stem[length] = '\0';

    file_id_t *ids   = NULL;
    int        count = query_all_everywhere(stem, NULL, &ids);
    if (count < 0) {
        *why = "the volume did not answer";
        return 0;
    }

    file_id_t found   = 0;
    int       matches = 0;
    for (int i = 0; i < count; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(ids[i], &metadata) != 0) continue;
        bool named = whole ? strcmp(metadata.Filename, name) == 0
                           : StemLength(metadata.Filename) == length && memcmp(metadata.Filename, name, length) == 0;
        if (named && IsProgram(ids[i])) {
            found = ids[i];
            matches++;
        }
        FileMetadataRelease(&metadata);
    }
    free(ids);

    *why = matches == 0 ? "the volume holds no such program"
         : matches > 1  ? "more than one program has that name — say its whole name" : "";
    return matches == 1 ? found : 0;
}

/* How the list names a program: its name before the dot, as the shell names a
 * command, or its whole file name when another program has the same stem. A
 * heap string; NULL with no memory or no file. */
static char *NameOf(file_id_t program)
{
    FileMetadata metadata;
    if (FileMetadataRead(program, &metadata) != 0) return NULL;

    char       *name = Copy(metadata.Filename, StemLength(metadata.Filename));
    const char *why;
    if (name && FindProgram(name, &why) != program) {
        free(name);
        name = Copy(metadata.Filename, strlen(metadata.Filename));
    }
    FileMetadataRelease(&metadata);
    return name;
}

/* The pid of the program if it runs now. */
static bool Running(file_id_t program, uint32_t *pid)
{
    RosterProgram *roster = NULL;
    int            count  = proc_roster(&roster, NULL);
    bool           found  = false;
    for (int i = 0; i < count && !found; i++) {
        if (roster[i].program_file != program) continue;
        *pid  = roster[i].pid;
        found = true;
    }
    free(roster);
    return found;
}

/* ---- the list --------------------------------------------------------- */

/* The programs the list names, in step with its names: a name that means no
 * program is 0. */
static void Resolve(const Names *names, file_id_t *programs, const char **whys)
{
    for (size_t i = 0; i < names->Count; i++) {
        whys[i]     = "";
        programs[i] = FindProgram(names->Items[i], &whys[i]);
    }
}

/* The list changed in the person's settings file: names joined by blanks.
 * True when it is saved. */
static bool Save(const Names *names)
{
    size_t room = 1;
    for (size_t i = 0; i < names->Count; i++) room += strlen(names->Items[i]) + 1;
    char *value = malloc(room);
    if (!value) {
        printf("autostart: there was no memory to write the list\n");
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < names->Count; i++) {
        size_t length = strlen(names->Items[i]);
        if (i > 0) value[used++] = ' ';
        memcpy(value + used, names->Items[i], length);
        used += length;
    }
    value[used] = '\0';

    BoxSettings  settings;
    const char  *why = "";
    int          rc  = settings_read(cabin_info()->program_file, &settings);
    if (rc == 0) {
        rc = settings_set(&settings, NULL, "programs", value, &why);
        settings_release(&settings);
    }
    free(value);
    if (rc != 0) {
        printf("autostart: the list could not be written (%s)\n", rc == -ERR_INVALID_ARGUMENT ? why : "the settings file would not be saved");
        return false;
    }
    return true;
}

static void GiveBack(const file_id_t *programs, size_t count)
{
    if (count > 0) souvenir_things(programs, count);
}

/* ---- at the machine's start: the list is started ----------------------- */

static const char *Reason(int refusal)
{
    switch (-refusal) {
        case ERR_FILE_NOT_FOUND:           return "it is not a program";
        case ERR_PROCESS_LIMIT_EXCEEDED:   return "the machine runs as many programs as it can";
        case ERR_BINARY_TOO_LARGE:         return "its file is too large to start";
        case ERR_NO_MEMORY:                return "there was no memory for it";
        default:                           return "the machine refused it";
    }
}

/* One program of the list started, unless it runs already, and a line for it
 * — in the log when it runs, to the person when it will not. True when it runs
 * now. */
static bool Start(const char *name, file_id_t program, const char *why)
{
    if (program == 0) {
        printf("[AUTOSTART] Skip '%s': %s\n", name, why);
        return false;
    }
    uint32_t running;
    if (Running(program, &running)) {
        kdbg_print("[AUTOSTART] Skip '%s': it runs already (PID %u)", name, running);
        return true;
    }

    int pid = proc_exec_file(program, name, strlen(name), NULL, NULL);
    if (pid <= 0) {
        printf("[AUTOSTART] Skip '%s': %s\n", name, Reason(pid));
        return false;
    }
    kdbg_print("[AUTOSTART] Started '%s' (PID %d)", name, pid);
    return true;
}

static int StartList(void)
{
    Names names = { NULL, 0 };
    if (!NamesCut(&names, settings_text("programs"))) {
        printf("[AUTOSTART] there was no memory to read the list\n");
        NamesRelease(&names);
        return 1;
    }
    if (names.Count == 0) kdbg_print("[AUTOSTART] the list is empty — nothing to start");

    int refused = 0;
    for (size_t i = 0; i < names.Count; i++) {
        const char *why = "";
        file_id_t   program = FindProgram(names.Items[i], &why);
        if (!Start(names.Items[i], program, why)) refused++;
    }
    NamesRelease(&names);
    return refused ? 1 : 0;
}

/* ---- from the hub: show the list, add to it, take from it -------------- */

static int ShowList(void)
{
    Names names = { NULL, 0 };
    if (!NamesCut(&names, settings_text("programs"))) {
        printf("autostart: there was no memory to read the list\n");
        NamesRelease(&names);
        return 1;
    }
    if (names.Count == 0) {
        printf("the list is empty — nothing starts with the machine\n");
        NamesRelease(&names);
        return 0;
    }

    file_id_t  *programs = malloc(names.Count * sizeof(*programs));
    const char **whys    = malloc(names.Count * sizeof(*whys));
    if (!programs || !whys) {
        printf("autostart: there was no memory to read the list\n");
        free(programs);
        free(whys);
        NamesRelease(&names);
        return 1;
    }
    Resolve(&names, programs, whys);

    size_t given = 0;
    int    stale = 0;
    for (size_t i = 0; i < names.Count; i++) {
        if (programs[i] == 0) {
            printf("%s — %s\n", names.Items[i], whys[i]);
            stale++;
            continue;
        }
        uint32_t pid;
        if (Running(programs[i], &pid)) printf("%s — runs now (PID %u)\n", names.Items[i], pid);
        else                            printf("%s\n", names.Items[i]);
        programs[given++] = programs[i];
    }
    if (stale > 0) printf("settings autostart programs \"...\" mends the list\n");
    GiveBack(programs, given);

    free(programs);
    free(whys);
    NamesRelease(&names);
    return 0;
}

/* The list's own names without the programs the person took away and, for
 * add, with the new names after them: one change of the list, saved once. */
typedef enum { ChangeAdd, ChangeRemove } Change;

static void Say(Change change, const char *name, const char *what)
{
    printf("%s '%s': %s\n", change == ChangeAdd ? "add" : "remove", name, what);
}

static int Add(Names *list, const file_id_t *programs, size_t count, file_id_t *listed, size_t *done)
{
    int refused = 0;
    for (size_t i = 0; i < count; i++) {
        char *name = IsProgram(programs[i]) ? NameOf(programs[i]) : NULL;
        if (!name) {
            printf("add: file %llu is no program\n", (unsigned long long)programs[i]);
            refused++;
            continue;
        }
        bool on = false;
        for (size_t at = 0; at < list->Count && !on; at++) on = listed[at] == programs[i];
        if (on) {
            Say(ChangeAdd, name, "it is on the list already");
        } else if (NamesAdd(list, name)) {
            listed[list->Count - 1] = programs[i];
            Say(ChangeAdd, name, "on the list");
            (*done)++;
        } else {
            printf("add: there was no memory for '%s'\n", name);
            refused++;
        }
        free(name);
    }
    return refused;
}

static void Remove(Names *list, const file_id_t *programs, size_t count, file_id_t *listed, size_t *done)
{
    for (size_t i = 0; i < count; i++) {
        bool off = false;
        for (size_t at = 0; at < list->Count;) {
            if (listed[at] != programs[i]) {
                at++;
                continue;
            }
            Say(ChangeRemove, list->Items[at], "off the list");
            free(list->Items[at]);
            for (size_t move = at; move + 1 < list->Count; move++) {
                list->Items[move] = list->Items[move + 1];
                listed[move]      = listed[move + 1];
            }
            list->Count--;
            off = true;
            (*done)++;
        }
        if (!off) {
            char *name = NameOf(programs[i]);
            Say(ChangeRemove, name ? name : "that file", "it is not on the list");
            free(name);
        }
    }
}

/* `programs` put on the list or taken off it, the list saved when it changed
 * and the programs given back. 0, or 1 when something was not done. */
static int ChangeList(Change change, const file_id_t *programs, size_t count)
{
    Names names = { NULL, 0 };
    if (!NamesCut(&names, settings_text("programs"))) {
        printf("autostart: there was no memory to read the list\n");
        NamesRelease(&names);
        return 1;
    }
    file_id_t   *listed = malloc((names.Count + count + 1) * sizeof(*listed));
    const char **whys   = malloc((names.Count + 1) * sizeof(*whys));
    if (!listed || !whys) {
        printf("autostart: there was no memory to read the list\n");
        free(listed);
        free(whys);
        NamesRelease(&names);
        return 1;
    }
    Resolve(&names, listed, whys);
    free(whys);

    size_t done    = 0;
    int    refused = 0;
    if (change == ChangeAdd) refused = Add(&names, programs, count, listed, &done);
    else                     Remove(&names, programs, count, listed, &done);
    if (done > 0 && !Save(&names)) refused++;
    else if (done > 0) printf("the list is kept; it is started when the machine comes up\n");
    GiveBack(programs, count);

    free(listed);
    NamesRelease(&names);
    return refused ? 1 : 0;
}

/* ---- asking which ------------------------------------------------------ */

/* What the person may be offered: for add, the programs the volume has that are
 * not on the list; for remove, the list. Said as one wrapped line of names. */
static void Offer(Change change)
{
    Names list = { NULL, 0 };
    NamesCut(&list, settings_text("programs"));
    if (change == ChangeRemove) {
        printf("on the list:");
        for (size_t i = 0; i < list.Count; i++) printf(" %s", list.Items[i]);
        printf("\n");
        NamesRelease(&list);
        return;
    }

    file_id_t *ids   = NULL;
    int        count = query_all_everywhere(NULL, NULL, &ids);
    printf("programs not on the list:");
    for (int i = 0; i < count; i++) {
        if (!IsProgram(ids[i])) continue;
        char *name = NameOf(ids[i]);
        bool  on   = false;
        for (size_t at = 0; name && at < list.Count && !on; at++) on = strcmp(list.Items[at], name) == 0;
        if (name && !on) printf(" %s", name);
        free(name);
    }
    printf("\n");
    free(ids);
    NamesRelease(&list);
}

/* The programs a person names when asked, one line: the program ids of those
 * names, in a heap array; NULL with *count 0 when none were named. */
static file_id_t *Ask(Change change, size_t *count)
{
    *count = 0;
    Offer(change);
    printf("autostart %s: which programs?\n", change == ChangeAdd ? "add" : "remove");
    char *typed = NULL;
    if (readline_grow(&typed) < 0) return NULL;

    Names words = { NULL, 0 };
    if (!NamesCut(&words, typed)) {
        free(typed);
        NamesRelease(&words);
        return NULL;
    }
    free(typed);

    file_id_t *programs = malloc((words.Count + 1) * sizeof(*programs));
    for (size_t i = 0; programs && i < words.Count; i++) {
        const char *why = "";
        file_id_t   program = FindProgram(words.Items[i], &why);
        if (program == 0) printf("%s: %s\n", words.Items[i], why);
        else              programs[(*count)++] = program;
    }
    NamesRelease(&words);
    if (programs && *count == 0) {
        free(programs);
        programs = NULL;
    }
    return programs;
}

/* ---- the Form ---------------------------------------------------------- */

static int FromForm(void)
{
    const char *const *verbs;
    size_t             verb_count;
    const file_id_t   *programs;
    size_t             program_count;
    if (form_words(0, &verbs, &verb_count) != 0 || form_things(1, &programs, &program_count) != 0) return 1;

    const char *verb = verb_count > 0 ? verbs[0] : NULL;
    if (!verb) {
        if (program_count > 0) {
            printf("autostart: add them or remove them? (autostart add ..., autostart remove ...)\n");
            return 1;
        }
        return ShowList();
    }
    Change change;
    if (strcmp(verb, "add") == 0) {
        change = ChangeAdd;
    } else if (strcmp(verb, "remove") == 0) {
        change = ChangeRemove;
    } else {
        printf("autostart knows add and remove, not %s\n", verb);
        return 1;
    }
    if (program_count > 0) return ChangeList(change, programs, program_count);

    if (!form_person_here()) {
        printf("autostart %s: say which programs — nobody is here to ask\n", verb);
        return 1;
    }
    size_t     asked;
    file_id_t *named = Ask(change, &asked);
    if (!named) {
        printf("autostart %s: no program was named — the list is as it was\n", verb);
        return 1;
    }
    int rc = ChangeList(change, named, asked);
    free(named);
    return rc;
}

int main(void)
{
    return started_by_system() ? StartList() : FromForm();
}
