/*
 * volumes — the volumes of the house, and the names a person gives them.
 *
 *   volumes                 every volume: its number, what it is called,
 *                           how full it is, and where it stands
 *   volumes 1               the one numbered 1; `volumes Photos` the one
 *                           called so
 *   volumes 1 Фото 2026     the one numbered 1 is called "Фото 2026" from now
 *                           on — kept in its book, the name goes with it;
 *                           `volumes Photos Фото 2026` renames the one called
 *                           Photos
 *   volumes Flash away      the volume called Flash put away: what waits to
 *                           be written to it written, then let go of — its
 *                           stick may be pulled out, and seated anew it is
 *                           taken up again
 *   volumes shipped 0       what came with the system on volume 0 — its
 *                           programs, fonts and help, none of the person's
 *                           files — given back as `them`: `carry them 1 copy`
 *                           gives volume 1 a fresh system
 *   volumes new 2 1 Photos  a new volume laid on BoxOS's partition in entry
 *                           1 of disk 2 (`disks` shows them), called
 *                           "Photos", and given the system: the kernel and
 *                           the programs the system keeps — once the person
 *                           at the screen says yes
 *
 * A volume is named by the name a person gave it or by its number, a disk by
 * what its medium calls itself, a volume's name on it or its number
 * (volume_named, disk_named); a name several share is asked again by number.
 * A volume whose medium is out of the machine is said to be: its files are
 * shown by nothing till it is back, and the changes waiting for it here.
 */

#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/disk.h"
#include "box/string.h"
#include "box/error.h"
#include "box/system.h"

BOX_FORM("takes optional text \"a volume's name or number, and the name to give it or away; or new, a disk, a partition's entry and a name; or shipped and a volume\"\n"
         "gives things\n");

/* One volume, one line: its number, what it is called, and what stands. */
static void volumes_say(const VolumeListed *v)
{
    char shown[STORAGE_VOLUME_NAME_MOST + 64];
    char free_text[32];
    volume_shown(v, shown, sizeof(shown));
    size_shown(v->FreeBytes, free_text, sizeof(free_text));
    printf("%3u  %s%s", v->Number, shown, v->Home ? " (home)" : "");
    if (v->Away)
        printf(" — not plugged in: its %llu file(s) show again when it is", (unsigned long long)v->Files);
    else if (v->PutAway)
        printf(" — put away: its medium may be pulled out, and seated anew it is taken up again");
    else if (v->Mounted)
        printf(" — %llu file(s), %s free", (unsigned long long)v->Files, free_text);
    else
        printf(" — not mounted");
    if (v->Away && v->Unwritten)
        printf("; %llu change(s) wait to be written to it — plug it back in here before anywhere else",
               (unsigned long long)v->Unwritten);
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
    case ERR_DEVICE_NOT_READY:  return "it is not plugged in";
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
    DiskListed *disks = NULL;
    size_t      count = 0;
    if (disk_list(&disks, &count) != 0) {
        println("The disks could not be listed");
        return 1;
    }
    const char       *named  = at;
    const DiskListed *d      = NULL;
    size_t            called = disk_named(&at, disks, count, &d);
    uint32_t          entry  = 0;
    if (called == 0 && *named >= '0' && *named <= '9') {
        printf("There is no disk numbered %.*s\n", (int)strcspn(named, " "), named);
        free(disks);
        return 1;
    }
    if (called == 1 && !volumes_number(&at, &entry)) called = 0;
    if (called != 1) {
        println(called ? "More than one disk is called so — name it by its number (disks shows them)"
                       : "A volume is laid on a partition: volumes new <disk> <entry> [name] — disks shows them");
        free(disks);
        return 1;
    }
    if (strlen(at) > STORAGE_VOLUME_NAME_MOST) {
        printf("A volume's name is %u bytes at the most\n", STORAGE_VOLUME_NAME_MOST);
        free(disks);
        return 1;
    }
    uint32_t              seat = d->Seat;
    const DiskPartListed *p    = NULL;
    for (size_t i = 0; i < d->PartCount; i++)
        if (d->Parts[i].Entry == entry) p = &d->Parts[i];
    int status = 1;
    if (!p) {
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

/* `volumes <volume> away`: the volume put away — written, then let go of. */
static int volumes_put_away(uint32_t number)
{
    char          shown[STORAGE_VOLUME_NAME_MOST + 64];
    VolumeListed *v = NULL;
    if (volume_info(number, &v) != 0) {
        printf("There is no volume numbered %u\n", number);
        return 1;
    }
    volume_shown(v, shown, sizeof(shown));
    bool removable = v->Removable;
    free(v);
    printf("Putting %s away: what waits to be written to it is written, and then it is let go of\n", shown);
    int rc = volume_put_away(number);
    if (rc != 0) {
        printf("%s was not put away: %s\n", shown,
               rc == -ERR_BUSY             ? "the system runs from it"
               : rc == -ERR_DEVICE_NOT_READY ? "it is not plugged in — plug it in, and what waits for it is written"
               : rc == -ERR_ACCESS_DENIED  ? "only the person's hands put a volume away"
                                           : "the volume refused it");
        return 1;
    }
    printf("%s is put away%s\n", shown, removable ? ": it may be pulled out now" : "");
    return 0;
}

/* `volumes shipped <number>`: what came with the system on that volume, given
 * back as `them`. */
static int volumes_shipped(const char *at)
{
    uint32_t number = 0;
    int      called = volume_named(&at, &number);
    if (called != 1 || *at) {
        println(called > 1 ? "More than one volume is called so — name it by its number (volumes shows them)"
                           : "What came with the system is asked of a volume by its name or number: volumes shipped "
                             "<volume>");
        return 1;
    }
    file_id_t *ids   = NULL;
    int        count = files_shipped(number, &ids);
    if (count < 0) {
        printf("Volume %u could not be asked what came with the system: %s\n", number,
               count == -ERR_OBJECT_NOT_FOUND ? "there is no volume of that number"
               : count == -ERR_DEVICE_NOT_READY ? "it is not plugged in"
                                                : "the volume refused it");
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
    int         called = volume_named(&at, &number);
    if (called != 1) {
        if (called > 1)
            println("More than one volume is called so — name it by its number (volumes shows them)");
        else
            printf("There is no volume called or numbered %s — volumes shows them\n", text);
        exit(1);
        return 1;
    }

    if (strcmp(at, "away") == 0) {
        int rc = volumes_put_away(number);
        exit(rc);
        return rc;
    }
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
