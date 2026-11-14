/*
 * autostart — the programs the person wants running whenever the machine
 * comes up.
 *
 * The system starts three programs of its own — the display, the hub and
 * this one — and this one starts the rest: the list is its setting
 * `programs`, program names apart by blanks (a program's file name, or its
 * name before the dot), read from the Settings tree like any program's
 * settings (box/settings.h):
 *
 *     settings autostart                       the list as it stands
 *     settings autostart programs "clock pulse" change it
 *     settings autostart programs default      the list the machine came with
 *
 * Each program is started on its own and none waits for another: a program
 * that will not start — no such file, two files of that name, not a program,
 * the machine full — is said and the rest still start. One line for each,
 * and the program ends. A program whose box says rise is raised again by the
 * system if it falls, as one the system started itself (rise.h).
 *
 * Its exit: 0 when every program on the list started, 1 when one did not.
 */

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("");
BOX_SETTINGS("programs text default \"clock pulse gardener\" "
             "\"the programs started when the machine comes up, their names apart by blanks\"\n");

/* The file a name means: `name` as the volume files it — "clock.box" — or,
 * with no dot, the name before the dot of one; on the whole volume. 0 and
 * *why, a sentence's end, when there is none or more than one. */
static file_id_t FindProgram(const char *name, const char **why)
{
    const char *dot    = strrchr(name, '.');
    size_t      length = dot ? (size_t)(dot - name) : strlen(name);
    char        stem[64];
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
        const char *its_dot = strrchr(metadata.Filename, '.');
        size_t      its_length = its_dot ? (size_t)(its_dot - metadata.Filename) : strlen(metadata.Filename);
        bool        named = dot ? strcmp(metadata.Filename, name) == 0
                                : its_length == length && memcmp(metadata.Filename, stem, length) == 0;
        if (named) {
            found = ids[i];
            matches++;
        }
        FileMetadataRelease(&metadata);
    }
    free(ids);

    if (matches == 0) *why = "the volume holds no such file";
    if (matches > 1) *why = "more than one file has that name — say its whole name";
    return matches == 1 ? found : 0;
}

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

/* One program of the list started, and a line said for it. True when it ran. */
static bool Start(const char *name)
{
    const char *why = "";
    file_id_t   program = FindProgram(name, &why);
    if (program == 0) {
        printf("[AUTOSTART] Skip '%s': %s\n", name, why);
        return false;
    }

    int pid = proc_exec_file(program, name, strlen(name), NULL, NULL);
    if (pid <= 0) {
        printf("[AUTOSTART] Skip '%s': %s\n", name, Reason(pid));
        return false;
    }
    printf("[AUTOSTART] Started '%s' (PID %d)\n", name, pid);
    return true;
}

int main(void)
{
    const char *list = settings_text("programs");
    size_t      size = strlen(list) + 1;
    char       *line = malloc(size);
    char      **names = malloc((size / 2 + 1) * sizeof(*names));
    if (!line || !names) {
        printf("[AUTOSTART] there was no memory to read the list\n");
        return 1;
    }
    memcpy(line, list, size);

    uint32_t count = luggage_cut(line, names, (uint32_t)(size / 2 + 1));
    if (count == 0) printf("[AUTOSTART] the list is empty — nothing to start\n");

    int refused = 0;
    for (uint32_t i = 0; i < count; i++)
        if (!Start(names[i])) refused++;

    free(names);
    free(line);
    return refused ? 1 : 0;
}
