#include "assemble.h"
#include "boxasm_link.h"
#include "boxasm_source.h"

#include "box/error.h"
#include "box/file.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"
#include "box/system.h"

#define ASSEMBLE_ERROR_BYTES 8192u

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

static int EndsWithAsm(const char *Name)
{
    size_t Length = strlen(Name);
    return Length >= 4u && strcmp(Name + Length - 4u, ".asm") == 0;
}

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
    if (EndsWithAsm(Source))
        Length -= 4u;
    if (Length > SIZE_MAX - 4u || Length + 4u >= Capacity)
        return -ERR_INVALID_ARGUMENT;
    memcpy(Out, Source, Length);
    memcpy(Out + Length, ".elf", 5u);
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
static int AppendExplicitTags(char *Buffer, size_t Capacity, size_t *Used,
                              int WordCount, int TagWordStart, int *AddedApp)
{
    int Index;
    int HasApp = AssembleTagsHaveKey(Buffer, "app");
    int HasUtility = AssembleTagsHaveKey(Buffer, "utility");

    *AddedApp = 0;
    for (Index = TagWordStart; Index < WordCount; Index++) {
        const char *Token = luggage_word(Index);
        char Clean[128];
        size_t Length = strlen(Token);
        size_t Begin = 0;
        size_t End = Length;

        while (Begin < End && (Token[Begin] == '(' || Token[Begin] == ','))
            Begin++;
        while (End > Begin && (Token[End - 1u] == ')' || Token[End - 1u] == ','))
            End--;
        if (End <= Begin ||
            (End - Begin == 4u && memcmp(Token + Begin, "tags", 4u) == 0) ||
            (End - Begin == 5u && memcmp(Token + Begin, "tags:", 5u) == 0))
            continue;
        if (End - Begin >= sizeof(Clean))
            return -ERR_INVALID_ARGUMENT;
        memcpy(Clean, Token + Begin, End - Begin);
        Clean[End - Begin] = '\0';
        if (AppendTags(Buffer, Capacity, Used, Clean) != 0)
            return -ERR_INVALID_ARGUMENT;
        if (AssembleTagsHaveKey(Clean, "app")) HasApp = 1;
        if (AssembleTagsHaveKey(Clean, "utility")) HasUtility = 1;
    }
    if (!HasApp && !HasUtility) {
        if (AppendTags(Buffer, Capacity, Used, "app") != 0)
            return -ERR_INVALID_ARGUMENT;
        *AddedApp = 1;
    }
    return 0;
}

static int LinkEmbedded(const char *SourceName, BoxAsmBlob *Program,
                        BoxAsmBlob *Image, char *Error, size_t ErrorCapacity)
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
    Rc = BoxAsmLink(&Entry, Program, &RuntimeArchive, 0, Image,
                    Error, ErrorCapacity);
    free((void *)Program->Data);
    Program->Data = NULL;
    Program->Size = 0;
    return Rc;
}

/* "assemble program.asm [-> program.elf] [tags ...]", read from Luggage. */
static int ReadCommand(AssembleRequest *Request, int WordCount)
{
    if (WordCount < 2) {
        println("Usage: assemble program.asm [-> program.elf] [tags ...]");
        return -ERR_INVALID_ARGUMENT;
    }
    Request->SourceName = luggage_word(1);
    Request->TagWordStart = 2;
    if (WordCount >= 4 && strcmp(luggage_word(2), "->") == 0) {
        Request->RequestedOutput = luggage_word(3);
        Request->TagWordStart = 4;
    }
    if (BuildOutputName(Request->SourceName, Request->RequestedOutput,
                        Request->OutputName, sizeof(Request->OutputName)) != 0) {
        AssembleSayError("output name", -ERR_INVALID_ARGUMENT,
                 "the name is empty or longer than 63 characters");
        return -ERR_INVALID_ARGUMENT;
    }
    return 0;
}

/* "assemble: <summary>", then its details: what a lookup said when it did
 * not find its one file. */
static void SayNotFound(int Rc, const BoxAsmBuffer *Summary, const BoxAsmBuffer *Details)
{
    const char *Text = (const char *)Details->Data;
    size_t Length = (size_t)Details->Size;

    if (Summary->Size == 0) {
        AssembleSayError("source lookup", Rc, NULL);
        return;
    }
    printf("assemble: %.*s\n", (int)Summary->Size, (const char *)Summary->Data);
    if (Length != 0 && *Text == '\n') {
        Text++;
        Length--;
    }
    if (Length != 0)
        printf("%.*s\n", (int)Length, Text);
}

/* The one source file the name means in the Use Context — taken once, the
 * home of the whole build — its bytes, and the tags the program will wear:
 * the source's own, then the command's. */
static int LoadSource(AssembleRequest *Request, int WordCount)
{
    BoxAsmBuffer Summary = { 0 };
    BoxAsmBuffer Details = { 0 };
    BoxAsmLookup Lookup;
    FileMetadata Metadata;
    int Rc;

    if ((Rc = AssembleTakeContext(&Request->Home)) != 0) {
        AssembleSayError("use context", Rc, NULL);
        return Rc;
    }
    Lookup.Name = Request->SourceName;
    Lookup.Tags = "";
    Lookup.Home = Request->Home;
    Lookup.Everywhere = 0;
    Lookup.Included = 0;
    Rc = BoxAsmLocate(AssembleVolume(), &Lookup, &Request->SourceId, &Summary, &Details);
    if (Rc != 0)
        SayNotFound(Rc, &Summary, &Details);
    BoxAsmBufferRelease(&Summary);
    BoxAsmBufferRelease(&Details);
    if (Rc != 0)
        return Rc;
    memset(&Metadata, 0, sizeof(Metadata));
    Rc = FileMetadataRead(Request->SourceId, &Metadata);
    if (Rc != 0) {
        AssembleSayError("source metadata", Rc, NULL);
        return Rc;
    }
    if (Metadata.Size == 0) {
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
            AppendExplicitTags(Request->Tags, sizeof(Request->Tags),
                               &Request->TagLength, WordCount,
                               Request->TagWordStart, &Request->AddedApp) != 0) {
            AssembleSayError("output tags", -ERR_INVALID_ARGUMENT,
                     "the tags do not fit, or one is longer than 127 characters");
            Rc = -ERR_INVALID_ARGUMENT;
        }
    }
    FileMetadataRelease(&Metadata);
    return Rc;
}

/* Source to ET_REL to the linked image, entirely in memory: a program that
 * does not assemble or link leaves no file behind. Its %includes are found on
 * the volume, in the home the source was found in. */
static int BuildImage(AssembleRequest *Request, BoxAsmBlob *Image)
{
    BoxAsmBlob Program = { 0 };
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
    Rc = BoxAsmSourceAssemble(&Source, &Program, Error, ASSEMBLE_ERROR_BYTES);
    free(Request->SourceBytes);
    Request->SourceBytes = NULL;
    if (Rc != 0) {
        if (Error[0] != '\0')
            printf("assemble: %s\n", Error);
        else
            AssembleSayError(Request->SourceName, Rc, NULL);
    } else {
        Rc = LinkEmbedded(Request->SourceName, &Program, Image, Error, ASSEMBLE_ERROR_BYTES);
        if (Rc != 0)
            AssembleSayError("link", Rc, Error);
    }
    free(Error);
    return Rc;
}

int main(void)
{
    int WordCount = (int)luggage_word_count();
    AssembleRequest Request;
    BoxAsmBlob Image = { 0 };
    int Rc;

    memset(&Request, 0, sizeof(Request));
    if (ReadCommand(&Request, WordCount) != 0 ||
        LoadSource(&Request, WordCount) != 0 ||
        BuildImage(&Request, &Image) != 0) {
        free(Request.SourceBytes);
        free(Request.Home);
        exit(1);
    }
    Rc = AssemblePublish(&Request, &Image);
    free((void *)Image.Data);
    free(Request.Home);
    exit(Rc == 0 ? 0 : 1);
}
