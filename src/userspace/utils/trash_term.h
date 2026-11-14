/* trash_term.h — how long the programs that delete keep what they delete.
 *
 * The term is a person's choice, held in the hub's settings as `trash-days`:
 * a deleted file is kept that many days and then goes by itself, and 0 gives
 * it up at once. A program that deletes with a term (file_keep) asks for it
 * here. When the hub's settings cannot be read, the term is the default the
 * hub declares: thirty days. */

#ifndef TRASH_TERM_H
#define TRASH_TERM_H

#include "box/file.h"
#include "box/memory.h"
#include "box/settings.h"
#include "box/string.h"

#define TRASH_TERM_DEFAULT_DAYS 30u
#define TRASH_TERM_SECONDS_A_DAY 86400u

/* Whether the file is the hub's program: its name before the dot is hub. */
static inline bool TrashTermIsHub(file_id_t Id)
{
    FileMetadata Metadata;
    if (FileMetadataRead(Id, &Metadata) != 0) return false;
    const char *Dot  = Metadata.Filename ? strrchr(Metadata.Filename, '.') : NULL;
    size_t      Stem = Dot ? (size_t)(Dot - Metadata.Filename) : (Metadata.Filename ? strlen(Metadata.Filename) : 0);
    bool        Is   = Stem == 3 && memcmp(Metadata.Filename, "hub", 3) == 0;
    FileMetadataRelease(&Metadata);
    return Is;
}

/* The seconds a deleted file is kept: the hub's trash-days, as the person set
 * it, times a day. */
static inline uint64_t TrashTermSeconds(void)
{
    uint64_t   Days  = TRASH_TERM_DEFAULT_DAYS;
    file_id_t *Ids   = NULL;
    int        Count = query_all_everywhere("hub", NULL, &Ids);

    for (int I = 0; I < Count; I++) {
        if (!TrashTermIsHub(Ids[I])) continue;
        BoxSettings Settings;
        if (settings_read(Ids[I], &Settings) != 0) continue;
        const BoxSetting *Setting = settings_find(&Settings, "trash-days");
        bool              Found   = Setting != NULL;
        if (Found && Setting->Value.Number >= 0) Days = (uint64_t)Setting->Value.Number;
        settings_release(&Settings);
        if (Found) break;
    }
    free(Ids);
    return Days * TRASH_TERM_SECONDS_A_DAY;
}

#endif /* TRASH_TERM_H */
