/*
 * volumes — the volumes of the house, and the names a person gives them.
 *
 *   volumes                 every volume: its number, what it is called,
 *                           how full it is, and where it stands
 *   volumes 1               the one numbered 1
 *   volumes 1 Фото 2026     the one numbered 1 is called "Фото 2026" from now
 *                           on — kept in its book, the name goes with it
 *   volumes shipped 0       what came with the system on volume 0 — its
 *                           programs, fonts and help, none of the person's
 *                           files — given back as `them`: `carry them 1 copy`
 *                           gives volume 1 a fresh system
 *   volumes new 2 1 Photos  a new volume laid on BoxOS's partition in entry
 *                           1 of disk 2 (`disks` shows them), called
 *                           "Photos", and given the system: the kernel and
 *                           the programs the system keeps — once the person
 *                           at the screen says yes
 */

#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/disk.h"
#include "box/string.h"
#include "box/error.h"
#include "box/system.h"

BOX_FORM("takes optional text \"a volume's number, and the name to give it; or new, a disk's number, a partition's entry and a name; or shipped and a volume's number\"\n"
         "gives things\n");

/* One volume, one line: its number, what it is called, and what stands. */
static void volumes_say(const VolumeListed *v)
{
    char shown[STORAGE_VOLUME_NAME_MOST + 64];
    char free_text[32];
    volume_shown(v, shown, sizeof(shown));
    size_shown(v->FreeBytes, free_text, sizeof(free_text));
    printf("%3u  %s%s", v->Number, shown, v->Home ? " (home)" : "");
    if (v->Mounted)
        printf(" — %llu file(s), %s free", (unsigned long long)v->Files, free_text);
    else
        printf(" — not mounted");
    if (v->Away)
        printf(", its medium left: waiting for it");
    if (v->ReadOnly)
        printf(", read only");
    if (v->Removable)
        printf(", on a removable medium");
    printf("\n");
}

static int volumes_list(void)
{
    VolumeListed *volumes = NULL;
    size_t        count   = 0;
    if (volume_list(&volumes, &count) != 0) {
        println("Could not list the volumes");
        return 1;
    }
    println("Volumes:");
    for (size_t i = 0; i < count; i++)
        volumes_say(&volumes[i]);
    free(volumes);
    return 0;
}

/* What a refused name is told as. */
static const char *volumes_refusal(int rc)
{
    switch (-rc) {
    case ERR_INVALID_ARGUMENT:  return "it is not a name: UTF-8 of any language, no control character";
    case ERR_ACCESS_DENIED:     return "only the person's hands name a volume";
    case ERR_OBJECT_NOT_FOUND:  return "there is no volume of that number";
    case ERR_DEVICE_NOT_READY:  return "its medium left";
    case ERR_TAGFS_READ_ONLY:   return "it takes no change";
    case ERR_TAGFS_NO_SPACE:    return "its book had no room for it; it keeps the name it had";
    default:                    return "the volume refused it";
    }
}

/* What a refused laying is told as. */
static const char *volumes_lay_refusal(int rc)
{
    switch (-rc) {
    case ERR_ACCESS_DENIED:     return "only the person's hands lay a volume";
    case ERR_OBJECT_NOT_FOUND:  return "the disk is not there any more, or another sits in its place";
    case ERR_OUT_OF_RANGE:      return "the disk has no partition in that entry";
    case ERR_INVALID_ARGUMENT:  return "the partition is not BoxOS's, or the name is no name";
    case ERR_BUSY:              return "a volume of the house stands on it";
    case ERR_TAGFS_NO_SPACE:    return "the partition is too small for a volume and the system";
    case ERR_WOULD_BLOCK:       return "another volume is being laid: ask again";
    case ERR_PERMISSION_DENIED: return "the disk refuses writes: it is protected";
    case ERR_DEVICE_NOT_READY:  return "the disk left on the way";
    default:                    return "the disk refused it";
    }
}

/* A number at *at, the spaces after it passed: false for no number. */
static bool volumes_number(const char **at, uint32_t *out)
{
    const char *p = *at;
    uint32_t    n = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9' && n <= 0xFFFFu) n = n * 10u + (uint32_t)(*p++ - '0');
    if (*p && *p != ' ') return false;
    while (*p == ' ') p++;
    *at  = p;
    *out = n;
    return true;
}

/* `volumes new <disk> <entry> [name]`: the partition shown, a yes asked, and
 * the volume laid and given the system. */
static int volumes_new(const char *at)
{
    uint32_t seat = 0, entry = 0;
    if (!volumes_number(&at, &seat) || !volumes_number(&at, &entry)) {
        println("A volume is laid on a partition: volumes new <disk> <entry> [name] — `disks` shows them");
        return 1;
    }
    if (strlen(at) > STORAGE_VOLUME_NAME_MOST) {
        printf("A volume's name is %u bytes at the most\n", STORAGE_VOLUME_NAME_MOST);
        return 1;
    }
    DiskListed *disks = NULL;
    size_t      count = 0;
    if (disk_list(&disks, &count) != 0) {
        println("The disks could not be listed");
        return 1;
    }
    const DiskListed     *d = NULL;
    const DiskPartListed *p = NULL;
    for (size_t i = 0; i < count; i++)
        if (disks[i].Seat == seat) d = &disks[i];
    for (size_t i = 0; d && i < d->PartCount; i++)
        if (d->Parts[i].Entry == entry) p = &d->Parts[i];
    int status = 1;
    if (!d) {
        printf("There is no disk numbered %u\n", seat);
    } else if (!p) {
        printf("Disk %u has no partition in entry %u\n", seat, entry);
    } else if (p->Kind != DISK_PART_BOXOS) {
        printf("Entry %u of disk %u is %s, not BoxOS's — `disks %u add boxos` lays one\n", entry, seat,
               DiskKindName((DiskPartKind)p->Kind), seat);
    } else if (p->Mounted) {
        printf("A volume of the house stands on entry %u of disk %u: no volume is laid over it\n", entry, seat);
    } else {
        char size[24];
        size_shown(p->Blocks * d->BlockBytes, size, sizeof(size));
        printf("A new volume on entry %u of disk %u, %s%s%s%s: whatever the partition holds is gone, and it is "
               "given the system — the kernel and the programs the system keeps\n", entry, seat, size,
               *at ? ", called \"" : "", at, *at ? "\"" : "");
        uint32_t number = 0, roles = 0;
        if (!form_person_here()) {
            println("volumes: nobody is at the screen to say yes, so nothing was laid");
        } else if (!form_yes("lay it?")) {
            println("Nothing was laid");
            status = 0;
        } else {
            int rc = volume_lay(d, (uint16_t)entry, at, &number, &roles);
            if (rc != 0) {
                printf("No volume was laid on disk %u: %s\n", seat, volumes_lay_refusal(rc));
            } else {
                printf("Volume %u is laid on disk %u, %u file(s) of the system on it\n", number, seat, roles);
                VolumeListed *v = NULL;
                if (volume_info(number, &v) == 0) {
                    volumes_say(v);
                    free(v);
                }
                status = 0;
            }
        }
    }
    free(disks);
    return status;
}

/* `volumes shipped <number>`: what came with the system on that volume, given
 * back as `them`. */
static int volumes_shipped(const char *at)
{
    uint32_t number = 0;
    if (!volumes_number(&at, &number) || *at) {
        println("What came with the system is asked of a volume by its number: volumes shipped <number>");
        return 1;
    }
    file_id_t *ids   = NULL;
    int        count = files_shipped(number, &ids);
    if (count < 0) {
        printf("Volume %u could not be asked what came with the system: %s\n", number,
               count == -ERR_OBJECT_NOT_FOUND ? "there is no volume of that number" : "the volume refused it");
        return 1;
    }
    printf("%d file(s) came with the system on volume %u — `them` for the next command\n", count, number);
    souvenir_things(ids, (size_t)count);
    free(ids);
    return 0;
}

int main(void)
{
    const char *text = form_text(0);
    if (!text) {
        exit(1);
        return 1;
    }
    if (!text[0]) {
        int rc = volumes_list();
        exit(rc);
        return rc;
    }
    if (strncmp(text, "new ", 4) == 0 || strcmp(text, "new") == 0) {
        int rc = volumes_new(text + 3 + (text[3] == ' '));
        exit(rc);
        return rc;
    }
    if (strncmp(text, "shipped ", 8) == 0 || strcmp(text, "shipped") == 0) {
        int rc = volumes_shipped(text + 7 + (text[7] == ' '));
        exit(rc);
        return rc;
    }

    uint32_t    number = 0;
    const char *at     = text;
    if (*at < '0' || *at > '9') {
        println("A volume is named by its number: volumes <number> <name>");
        exit(1);
        return 1;
    }
    while (*at >= '0' && *at <= '9' && number <= 0x7FFFu)
        number = number * 10u + (uint32_t)(*at++ - '0');
    if (*at && *at != ' ') {
        println("A volume is named by its number: volumes <number> <name>");
        exit(1);
        return 1;
    }
    while (*at == ' ')
        at++;

    if (*at) {
        size_t len = strlen(at);
        if (len > STORAGE_VOLUME_NAME_MOST) {
            printf("A volume's name is %u bytes at the most\n", STORAGE_VOLUME_NAME_MOST);
            exit(1);
            return 1;
        }
        int rc = volume_rename(number, at);
        if (rc != 0) {
            printf("Volume %u is not called \"%s\": %s\n", number, at, volumes_refusal(rc));
            exit(1);
            return 1;
        }
    }

    VolumeListed *v = NULL;
    if (volume_info(number, &v) != 0) {
        printf("There is no volume numbered %u\n", number);
        exit(1);
        return 1;
    }
    volumes_say(v);
    free(v);
    exit(0);
    return 0;
}
