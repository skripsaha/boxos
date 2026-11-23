/*
 * carry — files taken to another volume of the house, whole.
 *
 *   carry note.txt, photo.png 1       moved to volume 1: there now, and only there
 *   carry note.txt 1 copy             copied to volume 1: there too
 *
 * A file goes as it was: its bytes, its name, its tags, the programs it shows
 * them to, its term in the trash, who made it and when. `volumes` says what
 * the house holds; the copies are given back as `them`.
 */

#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/error.h"
#include "box/system.h"

BOX_FORM("takes things \"the files to carry\"\n"
         "takes text \"the volume's number to carry them to, and copy to leave them where they are too\"\n"
         "gives things\n");

/* What a refused carry is told as. */
static const char *carry_refusal(int rc)
{
    switch (-rc) {
    case ERR_ACCESS_DENIED:      return "only the person's hands carry files";
    case ERR_OBJECT_NOT_FOUND:   return "there is no volume of that number";
    case ERR_INVALID_ARGUMENT:   return "a file lies on that volume already";
    case ERR_FILE_NOT_FOUND:     return "a file is not there any more";
    case ERR_PERMISSION_DENIED:  return "a file the system keeps is laid by a volume's maker, not carried";
    case ERR_DEVICE_NOT_READY:   return "a medium left on the way";
    case ERR_BUSY:               return "a file changed while it was carried: it stays where it was, beside its copy";
    case ERR_TAGFS_NO_SPACE:     return "the volume has no room for it";
    case ERR_TAGFS_READ_ONLY:    return "the volume takes no change";
    default:                     return "the volume refused it";
    }
}

static void carry_usage(void)
{
    println("A volume is named by its number: carry <files> <number>, or carry <files> <number> copy");
}

int main(void)
{
    const file_id_t *files = NULL;
    size_t           count = 0;
    const char      *text  = form_text(1);
    if (form_things(0, &files, &count) != 0 || !text) {
        exit(1);
        return 1;
    }

    uint32_t    volume = 0;
    const char *at     = text;
    if (*at < '0' || *at > '9') {
        carry_usage();
        exit(1);
        return 1;
    }
    while (*at >= '0' && *at <= '9' && volume <= 0x7FFFu)
        volume = volume * 10u + (uint32_t)(*at++ - '0');
    while (*at == ' ')
        at++;
    bool copy = strcmp(at, "copy") == 0;
    if (*at && !copy) {
        carry_usage();
        exit(1);
        return 1;
    }

    file_id_t *copies  = malloc(sizeof(file_id_t) * count);
    size_t     carried = 0;
    uint32_t   dropped = 0;
    if (!copies) {
        println("Error: no memory for the copies' ids");
        exit(1);
        return 1;
    }
    int rc = copy ? files_copy_to(volume, files, count, copies, &carried, &dropped)
                  : files_move_to(volume, files, count, copies, &carried, &dropped);

    char          shown[STORAGE_VOLUME_NAME_MOST + 64];
    VolumeListed *v = NULL;
    if (volume_info(volume, &v) == 0) {
        volume_shown(v, shown, sizeof(shown));
        free(v);
    } else {
        snprintf(shown, sizeof(shown), "volume %u", volume);
    }
    if (carried)
        printf("%s %u file(s) to %s\n", copy ? "Copied" : "Moved", (unsigned)carried, shown);
    if (dropped)
        printf("%u word(s) of programs %s does not hold were left behind\n", dropped, shown);
    if (rc != 0)
        printf("%s file(s) not %s: %s\n", carried ? "The rest of the" : "The", copy ? "copied" : "moved",
               carry_refusal(rc));
    souvenir_things(copies, carried);
    free(copies);
    exit(rc == 0 ? 0 : 1);
    return rc == 0 ? 0 : 1;
}
