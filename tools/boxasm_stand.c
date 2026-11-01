/*
 * boxasm_stand — the in-BoxOS assembler and linker, built for the host.
 *
 * The very sources `assemble` is made of, compiled here so tools/boxasm_stand.sh
 * can hold their output to GNU as, GNU ld and tools/nameplate on the inputs a
 * BoxOS image links. Nothing here re-implements them.
 *
 *     boxasm_stand assemble <source.asm> <out.o>
 *     boxasm_stand volume <dir> <context> <source.asm> <out.o>
 *     boxasm_stand link <entry.o> <program.o> <archive.a> <out.elf> [symbols]
 *
 * `volume` stands a set of tagged files in for a BoxOS volume: <dir>/volume
 * lists them a line each, "<id> <name> <tags>", and <dir>/<id> holds each
 * one's bytes. The source is found among the tags of <context> as `assemble`
 * finds it in the Use Context, and its %includes are found on that volume.
 *
 * A refusal prints the toolchain's own sentence on stderr and exits 1.
 */
#include "boxasm_link.h"
#include "boxasm_source.h"

#include "box/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *ReadAll(const char *Path, uint64_t *Size)
{
    FILE *File = fopen(Path, "rb");
    unsigned char *Bytes;
    long Length;

    if (!File)
        return NULL;
    if (fseek(File, 0, SEEK_END) != 0 || (Length = ftell(File)) < 0 ||
        fseek(File, 0, SEEK_SET) != 0) {
        fclose(File);
        return NULL;
    }
    Bytes = malloc((size_t)Length + 1u);
    if (!Bytes || fread(Bytes, 1, (size_t)Length, File) != (size_t)Length) {
        free(Bytes);
        fclose(File);
        return NULL;
    }
    fclose(File);
    Bytes[Length] = '\0';
    *Size = (uint64_t)Length;
    return Bytes;
}

static int WriteAll(const char *Path, const uint8_t *Data, uint64_t Size)
{
    FILE *File = fopen(Path, "wb");
    int Ok;

    if (!File)
        return 0;
    Ok = fwrite(Data, 1, (size_t)Size, File) == (size_t)Size;
    return fclose(File) == 0 && Ok;
}

static const char *BaseName(const char *Path)
{
    const char *Slash = strrchr(Path, '/');
    return Slash ? Slash + 1 : Path;
}

static int Load(const char *Path, BoxAsmBlob *Blob)
{
    uint64_t Size = 0;
    unsigned char *Bytes = ReadAll(Path, &Size);

    if (!Bytes) {
        fprintf(stderr, "boxasm_stand: cannot read %s\n", Path);
        return 0;
    }
    *Blob = (BoxAsmBlob){ Bytes, Size, BaseName(Path) };
    return 1;
}

/* ---- A volume of tagged files ------------------------------------------- */

typedef struct {
    uint64_t Id;
    char    *Name;
    char    *Tags;     /* as the volume says them: the name's stem first */
} StandFile;

typedef struct {
    BoxAsmFiles Files;
    const char *Dir;
    StandFile  *List;
    size_t      Count;
} StandVolume;

static char *StandCopy(const char *Text, size_t Length)
{
    char *Out = malloc(Length + 1u);

    if (Out) {
        memcpy(Out, Text, Length);
        Out[Length] = '\0';
    }
    return Out;
}

static int StandFind(BoxAsmFiles *Files, const char *Name, const char *Tags, uint64_t **Ids)
{
    StandVolume *Volume = (StandVolume *)Files;
    size_t Index;
    int Count = 0;

    *Ids = malloc((Volume->Count + 1u) * sizeof(uint64_t));
    if (!*Ids)
        return -ERR_NO_MEMORY;
    for (Index = 0; Index < Volume->Count; Index++) {
        const StandFile *File = &Volume->List[Index];
        BoxAsmTag Tag;
        size_t At = 0;
        int Wears = strcmp(File->Name, Name) == 0;

        while (Wears && BoxAsmNextTag(Tags, &At, &Tag))
            Wears = BoxAsmTagsHold(File->Tags, &Tag, 0);
        if (Wears)
            (*Ids)[Count++] = File->Id;
    }
    return Count;
}

static const StandFile *StandFileOf(StandVolume *Volume, uint64_t Id)
{
    size_t Index;

    for (Index = 0; Index < Volume->Count; Index++)
        if (Volume->List[Index].Id == Id)
            return &Volume->List[Index];
    return NULL;
}

static int StandTags(BoxAsmFiles *Files, uint64_t Id, char **Out)
{
    const StandFile *File = StandFileOf((StandVolume *)Files, Id);

    if (!File)
        return -ERR_FILE_NOT_FOUND;
    return (*Out = StandCopy(File->Tags, strlen(File->Tags))) ? 0 : -ERR_NO_MEMORY;
}

static int StandRead(BoxAsmFiles *Files, uint64_t Id, size_t Limit, char **Out, size_t *Size)
{
    StandVolume *Volume = (StandVolume *)Files;
    char Path[4096];
    uint64_t Length = 0;

    if (!StandFileOf(Volume, Id))
        return -ERR_FILE_NOT_FOUND;
    snprintf(Path, sizeof(Path), "%s/%llu", Volume->Dir, (unsigned long long)Id);
    if (!(*Out = (char *)ReadAll(Path, &Length)))
        return -ERR_IO;
    if (Length > Limit) {
        free(*Out);
        *Out = NULL;
        return -ERR_OUT_OF_RANGE;
    }
    *Size = (size_t)Length;
    return 0;
}

/* <dir>/volume read: "<id> <name> <tags>" a line, the stem put first. */
static int StandMount(StandVolume *Volume, const char *Dir)
{
    char Path[4096];
    char Line[4096];
    FILE *List;

    memset(Volume, 0, sizeof(*Volume));
    Volume->Files.Find = StandFind;
    Volume->Files.Tags = StandTags;
    Volume->Files.Read = StandRead;
    Volume->Dir = Dir;
    snprintf(Path, sizeof(Path), "%s/volume", Dir);
    if (!(List = fopen(Path, "r")))
        return 0;
    while (fgets(Line, sizeof(Line), List)) {
        char *Name = strchr(Line, ' ');
        char *Tags;
        char *Dot;
        size_t Stem;
        StandFile *File;

        Line[strcspn(Line, "\n")] = '\0';
        if (!Name)
            continue;
        *Name++ = '\0';
        Tags = strchr(Name, ' ');
        if (Tags)
            *Tags++ = '\0';
        Volume->List = realloc(Volume->List, (Volume->Count + 1u) * sizeof(StandFile));
        File = &Volume->List[Volume->Count++];
        File->Id = strtoull(Line, NULL, 10);
        File->Name = StandCopy(Name, strlen(Name));
        Dot = strrchr(Name, '.');
        Stem = Dot ? (size_t)(Dot - Name) : strlen(Name);
        File->Tags = malloc(Stem + (Tags ? strlen(Tags) : 0) + 2u);
        memcpy(File->Tags, Name, Stem);
        File->Tags[Stem] = '\0';
        if (Tags && *Tags) {
            strcat(File->Tags, Stem ? "," : "");
            strcat(File->Tags, Tags);
        }
    }
    fclose(List);
    return 1;
}

/* What `assemble` says of a lookup that did not find its one file. */
static void StandSayRefusal(int Rc, const BoxAsmBuffer *Summary, const BoxAsmBuffer *Details)
{
    const char *Text = (const char *)Details->Data;
    size_t Length = (size_t)Details->Size;

    if (Summary->Size == 0) {
        fprintf(stderr, "assemble: source lookup: %s\n", BoxAsmErrorText(Rc));
        return;
    }
    fprintf(stderr, "assemble: %.*s\n", (int)Summary->Size, (const char *)Summary->Data);
    if (Length != 0 && *Text == '\n') {
        Text++;
        Length--;
    }
    if (Length != 0)
        fprintf(stderr, "%.*s\n", (int)Length, Text);
}

static int Finish(int Rc, const char *Error, const BoxAsmBlob *Out, const char *Path,
                  BoxAsmBuffer *Said)
{
    if (Said->Size != 0)
        fprintf(stderr, "%.*s", (int)Said->Size, (const char *)Said->Data);
    BoxAsmBufferRelease(Said);
    if (Rc != 0) {
        fprintf(stderr, "%s\n", Error[0] ? Error : "(refused without a sentence)");
        return 1;
    }
    if (!WriteAll(Path, Out->Data, Out->Size)) {
        fprintf(stderr, "boxasm_stand: cannot write %s\n", Path);
        return 2;
    }
    return 0;
}

int main(int Argc, char **Argv)
{
    static char Error[8192];
    BoxAsmBlob Out = { 0 };
    BoxAsmBuffer Said = { 0 };

    if (Argc == 4 && strcmp(Argv[1], "assemble") == 0) {
        BoxAsmBlob Loaded;
        BoxAsmSource Source = { 0 };

        if (!Load(Argv[2], &Loaded))
            return 2;
        Source.Text = (const char *)Loaded.Data;
        Source.Size = (size_t)Loaded.Size;
        Source.Name = Loaded.Name;
        Source.Said = &Said;
        return Finish(BoxAsmSourceAssemble(&Source, &Out, Error, sizeof(Error)), Error, &Out, Argv[3],
                      &Said);
    }
    if (Argc == 6 && strcmp(Argv[1], "volume") == 0) {
        BoxAsmBuffer Summary = { 0 };
        BoxAsmBuffer Details = { 0 };
        BoxAsmLookup Lookup = { Argv[4], "", Argv[3], 0, NULL };
        BoxAsmSource Source = { 0 };
        StandVolume Volume;
        uint64_t Id = 0;
        char *Text = NULL;
        size_t Size = 0;
        int Rc;

        if (!StandMount(&Volume, Argv[2])) {
            fprintf(stderr, "boxasm_stand: cannot read %s/volume\n", Argv[2]);
            return 2;
        }
        Rc = BoxAsmLocate(&Volume.Files, &Lookup, &Id, &Summary, &Details);
        if (Rc != 0)
            StandSayRefusal(Rc, &Summary, &Details);
        BoxAsmBufferRelease(&Summary);
        BoxAsmBufferRelease(&Details);
        if (Rc != 0)
            return 1;
        if (StandRead(&Volume.Files, Id, SIZE_MAX, &Text, &Size) != 0)
            return 2;
        Source.Text = Text;
        Source.Size = Size;
        Source.Name = Argv[4];
        Source.Id = Id;
        Source.Home = Argv[3];
        Source.Files = &Volume.Files;
        Source.Said = &Said;
        return Finish(BoxAsmSourceAssemble(&Source, &Out, Error, sizeof(Error)), Error, &Out, Argv[5],
                      &Said);
    }
    if (Argc >= 6 && strcmp(Argv[1], "link") == 0) {
        BoxAsmBlob Entry, Program, Archive, Recipe = { 0 };
        uint32_t Flags = 0;
        int Known = 1;
        int Index;

        for (Index = 6; Index < Argc && Known; Index++) {
            if (strcmp(Argv[Index], "symbols") == 0)
                Flags |= BOXASM_LINK_SYMBOLS;
            else if (strncmp(Argv[Index], "recipe=", 7) == 0)
                Known = Load(Argv[Index] + 7, &Recipe) ? 1 : 2;
            else
                Known = 0;
        }
        if (Known == 2)
            return 2;
        if (Known) {
            if (!Load(Argv[2], &Entry) || !Load(Argv[3], &Program) || !Load(Argv[4], &Archive))
                return 2;
            return Finish(BoxAsmLink(&Entry, &Program, &Archive, Recipe.Data ? &Recipe : NULL,
                                     Flags, &Out, Error, sizeof(Error)),
                          Error, &Out, Argv[5], &Said);
        }
    }
    fprintf(stderr,
            "usage: %s assemble <source.asm> <out.o>\n"
            "       %s volume <dir> <context> <source.asm> <out.o>\n"
            "       %s link <entry.o> <program.o> <archive.a> <out.elf> [symbols] [recipe=<file>]\n",
            Argv[0], Argv[0], Argv[0]);
    return 2;
}
