#include "assemble.h"
#include "boxasm_link.h"
#include "boxasm_source.h"

#include "box/error.h"
#include "box/file.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/recipe.h"
#include "box/string.h"
#include "box/system.h"

#define ASSEMBLE_ERROR_BYTES 8192u

BOX_FORM("takes thing \"the source\"\n"
         "takes optional into \"the program: a box, or an ELF image named .elf\"\n"
         "takes optional words \"the program's tags\"\n"
         "gives things\n");

extern const uint8_t _binary____build_boxlib_start_o_start[];
extern const uint8_t _binary____build_boxlib_start_o_end[];
extern const uint8_t _binary____boxlib_libbox_a_start[];
extern const uint8_t _binary____boxlib_libbox_a_end[];

void AssembleSayError(const char *Stage, int Rc, const char *Detail)
{
    if (Detail && Detail[0] != '\0')
        printf("assemble: %s: %s\n", Stage, Detail);
    else
        printf("assemble: %s: %s\n", Stage, BoxAsmErrorText(Rc));
}

static int EndsWith(const char *Name, const char *Ending)
{
    size_t Length = strlen(Name);
    size_t EndingLength = strlen(Ending);

    return Length >= EndingLength && strcmp(Name + Length - EndingLength, Ending) == 0;
}

/* The program's name: the one -> points at, or the source's stem made a
 * box — hello.asm makes hello.box. */
static int BuildOutputName(const char *Source, const char *Requested,
                           char *Out, size_t Capacity)
{
    size_t Length;

    if (!Source || !Out || Capacity == 0)
        return -ERR_INVALID_ARGUMENT;
    if (Requested) {
        Length = strlen(Requested);
        if (Length == 0 || Length >= Capacity)
            return -ERR_INVALID_ARGUMENT;
        memcpy(Out, Requested, Length + 1u);
        return 0;
    }
    Length = strlen(Source);
    if (EndsWith(Source, ".asm"))
        Length -= 4u;
    if (Length > SIZE_MAX - 4u || Length + 4u >= Capacity)
        return -ERR_INVALID_ARGUMENT;
    memcpy(Out, Source, Length);
    memcpy(Out + Length, ".box", 5u);
    return 0;
}

static int AppendTags(char *Buffer, size_t Capacity, size_t *Used,
                      const char *Tags)
{
    size_t Length;

    if (!Tags || Tags[0] == '\0')
        return 0;
    Length = strlen(Tags);
    if (*Used != 0) {
        if (*Used + 1u >= Capacity)
            return -ERR_INVALID_ARGUMENT;
        Buffer[(*Used)++] = ',';
    }
    if (*Used > Capacity - 1u || Length > Capacity - 1u - *Used)
        return -ERR_INVALID_ARGUMENT;
    memcpy(Buffer + *Used, Tags, Length);
    *Used += Length;
    Buffer[*Used] = '\0';
    return 0;
}

/* The source's tags, then the ones the command names. The shell starts only
 * a file that wears app or utility, so a program that would wear neither is
 * given app and the command says so; *AddedApp reports it. */
static int AppendExplicitTags(AssembleRequest *Request)
{
    size_t Index;
    int HasApp = AssembleTagsHaveKey(Request->Tags, "app");
    int HasUtility = AssembleTagsHaveKey(Request->Tags, "utility");

    Request->AddedApp = 0;
    for (Index = 0; Index < Request->TagWordCount; Index++) {
        const char *Tag = Request->TagWords[Index];

        if (AppendTags(Request->Tags, sizeof(Request->Tags), &Request->TagLength, Tag) != 0)
            return -ERR_INVALID_ARGUMENT;
        if (AssembleTagsHaveKey(Tag, "app")) HasApp = 1;
        if (AssembleTagsHaveKey(Tag, "utility")) HasUtility = 1;
    }
    if (!HasApp && !HasUtility) {
        if (AppendTags(Request->Tags, sizeof(Request->Tags), &Request->TagLength, "app") != 0)
            return -ERR_INVALID_ARGUMENT;
        Request->AddedApp = 1;
    }
    return 0;
}

/* The program linked as a box, which the kernel places where it chooses — or,
 * for a program named name.elf, as an ELF image at the code start. */
static int LinkEmbedded(const char *SourceName, const char *OutputName, BoxAsmBlob *Program,
                        const BoxAsmBlob *Recipe, BoxAsmBlob *Image, char *Error,
                        size_t ErrorCapacity)
{
    BoxAsmBlob Entry = { _binary____build_boxlib_start_o_start,
                         (uint64_t)(_binary____build_boxlib_start_o_end -
                                    _binary____build_boxlib_start_o_start),
                         "boxlib_start.o" };
    BoxAsmBlob RuntimeArchive = { _binary____boxlib_libbox_a_start,
                                  (uint64_t)(_binary____boxlib_libbox_a_end -
                                             _binary____boxlib_libbox_a_start),
                                  "libbox.a" };
    int Rc;

    Program->Name = SourceName;
    Rc = BoxAsmLink(&Entry, Program, &RuntimeArchive, Recipe,
                    EndsWith(OutputName, ".elf") ? BOXASM_LINK_ELF : 0, Image, Error,
                    ErrorCapacity);
    free((void *)Program->Data);
    Program->Data = NULL;
    Program->Size = 0;
    return Rc;
}

/* The Form filled: the source file, the name the program is to have (the
 * one `->` points at) and the tags it wears. */
static int ReadForm(AssembleRequest *Request)
{
    const file_id_t *Source;
    const char *const *Into;
    size_t Count;

    if (form_things(0, &Source, &Count) != 0 ||
        form_words(1, &Into, &Count) != 0)
        return -ERR_INVALID_ARGUMENT;
    Request->SourceId = Source[0];
    Request->RequestedOutput = Count != 0 ? Into[0] : NULL;
    return form_words(2, &Request->TagWords, &Request->TagWordCount);
}

/* The file a Recipe begins with, still there to be read again. */
static int RecipeSourceThere(const BoxRecipe *Recipe, const char *Program)
{
    FileMetadata Metadata;
    int Rc;
    int Trashed;

    memset(&Metadata, 0, sizeof(Metadata));
    Rc = FileMetadataRead(Recipe->Files[0].Id, &Metadata);
    if (Rc != 0 && Rc != -ERR_FILE_NOT_FOUND) {
        AssembleSayError(Recipe->Files[0].Name, Rc, NULL);
        return Rc;
    }
    Trashed = Rc == 0 && Metadata.KeptUntil != 0;
    if (Rc == 0)
        FileMetadataRelease(&Metadata);
    if (Rc == 0 && !Trashed)
        return 0;
    printf("assemble: %s, which %s was made from, is gone\n", Recipe->Files[0].Name, Program);
    return -ERR_FILE_NOT_FOUND;
}

/* What a Recipe asks of the build: its first file as the source, its Use
 * Context as the home, and the program itself as what is made — unless ->
 * names another. */
static int TakeRecipe(AssembleRequest *Request, const BoxRecipe *Recipe)
{
    int Rc;

    if (strcmp(Recipe->By, "assemble") != 0) {
        printf("assemble: %s was made by %s, not by assemble\n", Request->RemakeName, Recipe->By);
        return -ERR_INVALID_ARGUMENT;
    }
    Rc = RecipeSourceThere(Recipe, Request->RemakeName);
    if (Rc != 0)
        return Rc;
    if (!(Request->Home = BoxAsmCopyText(Recipe->Context, strlen(Recipe->Context)))) {
        AssembleSayError(Request->RemakeName, -ERR_NO_MEMORY, NULL);
        return -ERR_NO_MEMORY;
    }
    Request->Remake = Request->RequestedOutput ? 0 : Request->SourceId;
    if (!Request->RequestedOutput)
        Request->RequestedOutput = Request->RemakeName;
    Request->SourceId = Recipe->Files[0].Id;
    return 0;
}

/* A program handed over in place of a source is made again by its Recipe.
 * One with no Recipe was made elsewhere: there is nothing to make it from. */
static int FollowRecipe(AssembleRequest *Request)
{
    BoxRecipe Recipe;
    FileMetadata Metadata;
    size_t Length;
    int Rc = recipe_of_file(Request->SourceId, &Recipe);

    if (Rc != 0) {
        AssembleSayError("the program's Recipe", Rc, NULL);
        return Rc;
    }
    if (!Recipe.Image)
        return 0;
    memset(&Metadata, 0, sizeof(Metadata));
    if ((Rc = FileMetadataRead(Request->SourceId, &Metadata)) != 0) {
        recipe_release(&Recipe);
        AssembleSayError("program metadata", Rc, NULL);
        return Rc;
    }
    Length = Metadata.Filename ? strlen(Metadata.Filename) : 0;
    if (Length == 0 || Length >= sizeof(Request->RemakeName)) {
        AssembleSayError("program name", -ERR_INVALID_ARGUMENT,
                         "the name is empty or longer than 63 characters");
        Rc = -ERR_INVALID_ARGUMENT;
    } else {
        memcpy(Request->RemakeName, Metadata.Filename, Length + 1u);
    }
    FileMetadataRelease(&Metadata);
    if (Rc == 0 && !Recipe.Declared) {
        printf("assemble: %s is a program made elsewhere: it carries no Recipe to make it again from\n",
               Request->RemakeName);
        Rc = -ERR_INVALID_ARGUMENT;
    }
    if (Rc == 0)
        Rc = TakeRecipe(Request, &Recipe);
    recipe_release(&Recipe);
    return Rc;
}

/* The source file's name and, from it or from the command, the program's. */
static int NameFiles(AssembleRequest *Request, const FileMetadata *Metadata)
{
    size_t Length = Metadata->Filename ? strlen(Metadata->Filename) : 0;

    if (Length == 0 || Length >= sizeof(Request->SourceName)) {
        AssembleSayError("source name", -ERR_INVALID_ARGUMENT,
                 "the name is empty or longer than 63 characters");
        return -ERR_INVALID_ARGUMENT;
    }
    memcpy(Request->SourceName, Metadata->Filename, Length + 1u);
    if (BuildOutputName(Request->SourceName, Request->RequestedOutput,
                        Request->OutputName, sizeof(Request->OutputName)) != 0) {
        AssembleSayError("output name", -ERR_INVALID_ARGUMENT,
                 "the name is empty or longer than 63 characters");
        return -ERR_INVALID_ARGUMENT;
    }
    return 0;
}

/* The Use Context the build begins in - taken once, the home of the whole
 * build, unless a Recipe gave it - the source's bytes, and the tags the
 * program will wear: the source's own, then the command's. */
static int LoadSource(AssembleRequest *Request)
{
    FileMetadata Metadata;
    int Rc;

    if (!Request->Home && (Rc = AssembleTakeContext(&Request->Home)) != 0) {
        AssembleSayError("use context", Rc, NULL);
        return Rc;
    }
    memset(&Metadata, 0, sizeof(Metadata));
    Rc = FileMetadataRead(Request->SourceId, &Metadata);
    if (Rc != 0) {
        AssembleSayError("source metadata", Rc, NULL);
        return Rc;
    }
    Rc = NameFiles(Request, &Metadata);
    if (Rc == 0 && Metadata.Size == 0) {
        printf("assemble: %s is empty\n", Request->SourceName);
        Rc = -ERR_INVALID_ARGUMENT;
    }
    if (Rc == 0) {
        Rc = AssembleVolume()->Read(AssembleVolume(), Request->SourceId, SIZE_MAX - 1u,
                                    &Request->SourceBytes, &Request->SourceSize);
        if (Rc != 0)
            AssembleSayError("source read", Rc, NULL);
    }
    if (Rc == 0) {
        if (AppendTags(Request->Tags, sizeof(Request->Tags), &Request->TagLength,
                       Metadata.Tags) != 0 ||
            AppendExplicitTags(Request) != 0) {
            AssembleSayError("output tags", -ERR_INVALID_ARGUMENT,
                     "the tags do not fit");
            Rc = -ERR_INVALID_ARGUMENT;
        }
    }
    FileMetadataRelease(&Metadata);
    return Rc;
}

/* Source to ET_REL to the linked image, entirely in memory: a program that
 * does not assemble or link leaves no file behind. Its %includes are found on
 * the volume, in the home the source was found in, and the image carries the
 * Recipe of every file the build read. */
static int BuildImage(AssembleRequest *Request, BoxAsmBlob *Image)
{
    BoxAsmBlob Program = { 0 };
    BoxAsmBuffer Said = { 0 };
    BoxAsmSource Source;
    char *Error = (char *)malloc(ASSEMBLE_ERROR_BYTES);
    int Rc;

    if (!Error) {
        AssembleSayError(Request->SourceName, -ERR_NO_MEMORY, NULL);
        return -ERR_NO_MEMORY;
    }
    Error[0] = '\0';
    Source.Text = Request->SourceBytes;
    Source.Size = Request->SourceSize;
    Source.Name = Request->SourceName;
    Source.Id = Request->SourceId;
    Source.Home = Request->Home;
    Source.Files = AssembleVolume();
    Source.Said = &Said;
    Rc = BoxAsmSourceAssemble(&Source, &Program, Error, ASSEMBLE_ERROR_BYTES);
    if (Said.Size != 0)                        /* what the source said, before how it ended */
        printf("assemble: %.*s", (int)Said.Size, (const char *)Said.Data);
    BoxAsmBufferRelease(&Said);
    free(Request->SourceBytes);
    Request->SourceBytes = NULL;
    if (Rc != 0) {
        if (Error[0] != '\0')
            printf("assemble: %s\n", Error);
        else
            AssembleSayError(Request->SourceName, Rc, NULL);
    } else {
        BoxAsmBlob Recipe = { NULL, 0, "recipe" };
        char *Text = NULL;
        size_t Length = 0;

        Rc = AssembleRecipe(Request->Home, &Text, &Length);
        if (Rc != 0) {
            AssembleSayError("recipe", Rc, NULL);
            free((void *)Program.Data);
        } else {
            Recipe.Data = (const uint8_t *)Text;
            Recipe.Size = Length;
            Rc = LinkEmbedded(Request->SourceName, Request->OutputName, &Program, &Recipe, Image,
                              Error, ASSEMBLE_ERROR_BYTES);
            free(Text);
            if (Rc != 0)
                AssembleSayError("link", Rc, Error);
        }
    }
    free(Error);
    return Rc;
}

int main(void)
{
    AssembleRequest Request;
    BoxAsmBlob Image = { 0 };
    file_id_t Published = 0;
    int Rc;

    memset(&Request, 0, sizeof(Request));
    if (ReadForm(&Request) != 0 ||
        FollowRecipe(&Request) != 0 ||
        LoadSource(&Request) != 0 ||
        BuildImage(&Request, &Image) != 0) {
        free(Request.SourceBytes);
        free(Request.Home);
        exit(1);
    }
    Rc = AssemblePublish(&Request, &Image, &Published);
    free((void *)Image.Data);
    free(Request.Home);
    if (Rc == 0)
        souvenir_things(&Published, 1);
    exit(Rc == 0 ? 0 : 1);
}
