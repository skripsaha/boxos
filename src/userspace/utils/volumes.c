/*
 * volumes — the volumes of the house, and the names a person gives them.
 *
 *   volumes                 every volume: its number, what it is called,
 *                           how full it is, and where it stands
 *   volumes 1               the one numbered 1
 *   volumes 1 Фото 2026     the one numbered 1 is called "Фото 2026" from now
 *                           on — kept in its book, the name goes with it
 */

#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/error.h"
#include "box/system.h"

BOX_FORM("takes optional text \"a volume's number, and the name to give it\"\n");

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
