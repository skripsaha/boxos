/* The volume as `assemble` reads it: the files a source names, found by name
 * and tags on the whole volume — the toolchain says where to look, and the
 * Use Context it looks in is taken once, when the build begins — and their
 * bytes. */

#include "assemble.h"

#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/use.h"

/* The name is a tag: the volume stamps every file with the stem of its name,
 * up to the last dot, so the index answers who is called so in one ask,
 * every answer then held to the whole name. A stem the tag grammar cannot
 * spell (a ':' or a ',' in it) is asked by the other tags alone. */
static int VolumeFind(BoxAsmFiles *Files, const char *Name, const char *Tags, uint64_t **Ids)
{
    const char *Dot = strrchr(Name, '.');
    size_t Stem = Dot ? (size_t)(Dot - Name) : strlen(Name);
    size_t TagLength = strlen(Tags);
    size_t Index;
    char *Ask;
    file_id_t *Found = NULL;
    int Spelled = Stem != 0;
    int Count;
    int Kept = 0;

    (void)Files;
    for (Index = 0; Index < Stem; Index++)
        if (Name[Index] == ':' || Name[Index] == ',')
            Spelled = 0;
    if (!(Ask = (char *)malloc(Stem + TagLength + 2u)))
        return -ERR_NO_MEMORY;
    Ask[0] = '\0';
    if (Spelled) {
        memcpy(Ask, Name, Stem);
        Ask[Stem] = '\0';
    }
    if (TagLength != 0) {
        if (Ask[0] != '\0')
            strcat(Ask, ",");
        strcat(Ask, Tags);
    }
    Count = query_all_everywhere(Ask, &Found);
    free(Ask);
    if (Count < 0)
        return Count;
    for (Index = 0; Index < (size_t)Count; Index++) {
        FileMetadata Metadata;

        memset(&Metadata, 0, sizeof(Metadata));
        if (FileMetadataRead(Found[Index], &Metadata) != 0)
            continue;
        if (Metadata.Filename && strcmp(Metadata.Filename, Name) == 0)
            Found[Kept++] = Found[Index];
        FileMetadataRelease(&Metadata);
    }
    *Ids = Found;
    return Kept;
}

static int VolumeTags(BoxAsmFiles *Files, uint64_t Id, char **Out)
{
    FileMetadata Metadata;
    const char *Tags;
    int Rc;

    (void)Files;
    memset(&Metadata, 0, sizeof(Metadata));
    if ((Rc = FileMetadataRead(Id, &Metadata)) != 0)
        return Rc;
    Tags = Metadata.Tags ? Metadata.Tags : "";
    *Out = BoxAsmCopyText(Tags, strlen(Tags));
    FileMetadataRelease(&Metadata);
    return *Out ? 0 : -ERR_NO_MEMORY;
}

/* A short read is the volume answering with what it had in hand: asked
 * again until the whole size is here. */
static int VolumeRead(BoxAsmFiles *Files, uint64_t Id, size_t Limit, char **Out, size_t *Size)
{
    FileMetadata Metadata;
    uint64_t Offset = 0;
    uint64_t Length;
    char *Bytes;
    int Rc;

    (void)Files;
    memset(&Metadata, 0, sizeof(Metadata));
    if ((Rc = FileMetadataRead(Id, &Metadata)) != 0)
        return Rc;
    Length = Metadata.Size;
    FileMetadataRelease(&Metadata);
    if (Length > Limit || Length > SIZE_MAX - 1u)
        return -ERR_OUT_OF_RANGE;
    if (!(Bytes = (char *)malloc((size_t)Length + 1u)))
        return -ERR_NO_MEMORY;
    while (Offset < Length) {
        uint64_t Remaining = Length - Offset;
        int64_t Got = fread(Id, Offset, Bytes + Offset, Remaining > 65536u ? 65536u : (size_t)Remaining);

        if (Got <= 0 || (uint64_t)Got > Remaining) {
            free(Bytes);
            return Got < 0 ? (int)Got : -ERR_IO;
        }
        Offset += (uint64_t)Got;
    }
    Bytes[Length] = '\0';
    *Out = Bytes;
    *Size = (size_t)Length;
    return 0;
}

static BoxAsmFiles AssembleVolumeFiles = { VolumeFind, VolumeTags, VolumeRead };

BoxAsmFiles *AssembleVolume(void)
{
    return &AssembleVolumeFiles;
}

int AssembleTakeContext(char **Out)
{
    size_t Needed = 0;
    int Rc;

    *Out = NULL;
    if ((Rc = use_get(NULL, 0, &Needed)) < 0)
        return Rc;
    for (;;) {
        size_t Room = Needed > 0 ? Needed : 1u;
        char *Text = (char *)malloc(Room);

        if (!Text)
            return -ERR_NO_MEMORY;
        Text[0] = '\0';
        if ((Rc = use_get(Text, Room, &Needed)) < 0) {
            free(Text);
            return Rc;
        }
        if (Needed <= Room) {
            *Out = Text;
            return 0;
        }
        free(Text);                             /* said longer meanwhile */
    }
}
