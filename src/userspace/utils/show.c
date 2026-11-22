#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes thing \"the file to show\"\n"
         "gives things\n");

#define SHOW_LIMIT 4096u

/* Printable bytes as they are, line breaks and tabs kept, the rest as \xNN. */
static void show_bytes(const char *bytes, int count)
{
    for (int i = 0; i < count; i++)
    {
        char c = bytes[i];
        if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t')
        {
            char s[2] = {c, 0};
            print(s);
        }
        else
        {
            printf("\\x%02x", (unsigned char)c);
        }
    }
}

int main(void)
{
    const file_id_t *file;
    size_t file_count;

    if (form_things(0, &file, &file_count) != 0)
    {
        exit(1);
        return 1;
    }

    file_id_t file_id = file[0];
    FileMetadata metadata;
    if (FileMetadataRead(MY_TAGS, file_id, &metadata) != 0)
    {
        println("Could not look at that file; it may have been erased");
        exit(1);
        return 1;
    }
    uint64_t size = metadata.Size;
    FileMetadataRelease(&metadata);

    /* boxlib's fread has no Pocket payload cap (Phase 12 Manifest + Crate
     * makes it unbounded), so 4 KiB per fread is one storage syscall
     * instead of ~24. */
    char buffer[SHOW_LIMIT];
    size_t total_read = 0;
    size_t bytes_to_read = size > SHOW_LIMIT ? SHOW_LIMIT : (size_t)size;

    while (total_read < bytes_to_read)
    {
        size_t chunk = bytes_to_read - total_read;
        int result = fread(file_id, total_read, buffer, chunk);
        if (result < 0)
        {
            char *name = name_of_file(file_id);
            printf("Could not read %s\n", name ? name : "the file");
            free(name);
            exit(1);
            return 1;
        }
        if (result == 0)
            break;

        show_bytes(buffer, result);
        total_read += result;
        if ((size_t)result < chunk)
            break;
    }

    souvenir_things(&file_id, 1);
    println("");
    exit(0);
    return 0;
}
