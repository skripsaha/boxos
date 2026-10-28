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

#define ASSEMBLE_ERROR_BYTES 512u

extern const uint8_t _binary____build_boxlib_start_o_start[];
extern const uint8_t _binary____build_boxlib_start_o_end[];
extern const uint8_t _binary____boxlib_libbox_a_start[];
extern const uint8_t _binary____boxlib_libbox_a_end[];

const char *AssembleDescribeError(int Rc)
{
    switch (box_errno_of(Rc)) {
    case OK: return "ok";
#define ASSEMBLE_ERROR_TEXT(SUFFIX, name, value, msg, cat) case ERR_##SUFFIX: return msg;
    BOX_ERROR_LIST(ASSEMBLE_ERROR_TEXT)
#undef ASSEMBLE_ERROR_TEXT
    default: return "unknown error";
    }
}

void AssembleSayError(const char *Stage, int Rc, const char *Detail)
{
    if (Detail && Detail[0] != '\0')
        printf("assemble: %s: %s\n", Stage, Detail);
    else
        printf("assemble: %s: %s\n", Stage, AssembleDescribeError(Rc));
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

static int ReadFileBytes(file_id_t FileId, uint64_t Size, uint8_t **Out)
{
    uint8_t *Bytes;
    uint64_t Offset = 0;

    if (!Out || Size == 0 || Size > SIZE_MAX - 1u)
        return -ERR_INVALID_ARGUMENT;
    Bytes = (uint8_t *)malloc((size_t)Size + 1u);
    if (!Bytes)
        return -ERR_NO_MEMORY;
    while (Offset < Size) {
        uint64_t Remaining = Size - Offset;
        size_t Ask = Remaining > 65536u ? 65536u : (size_t)Remaining;
        int64_t Got = fread(FileId, Offset, Bytes + Offset, Ask);
        if (Got < 0) {
            free(Bytes);
            return (int)Got;
        }
        if (Got == 0 || (uint64_t)Got > Remaining) {
            free(Bytes);
            return -ERR_IO;
        }
        Offset += (uint64_t)Got;
    }
    Bytes[Size] = '\0';
    *Out = Bytes;
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

static int FindSource(const char *Name, file_id_t *OutFile)
{
    file_id_t Id;
    int Count = find_file_by_name(Name, &Id, NULL, 1u);

    if (Count < 0)
        return Count;
    if (Count == 0)
        return -ERR_FILE_NOT_FOUND;
    if (Count != 1)
        return -ERR_INVALID_STATE;
    *OutFile = Id;
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

/* The one source file the name means here, its bytes, and the tags the
 * program will wear: the source's own, then the command's. */
static int LoadSource(AssembleRequest *Request, int WordCount)
{
    FileMetadata Metadata;
    int Rc = FindSource(Request->SourceName, &Request->SourceId);

    if (Rc == -ERR_FILE_NOT_FOUND) {
        printf("assemble: there is no %s here\n", Request->SourceName);
        return Rc;
    }
    if (Rc == -ERR_INVALID_STATE) {
        printf("assemble: more than one file is named %s; narrow the context with use\n",
               Request->SourceName);
        return Rc;
    }
    if (Rc != 0) {
        AssembleSayError("source lookup", Rc, NULL);
        return Rc;
    }
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
        Rc = ReadFileBytes(Request->SourceId, Metadata.Size, &Request->SourceBytes);
        if (Rc != 0)
            AssembleSayError("source read", Rc, NULL);
    }
    if (Rc == 0) {
        Request->SourceSize = Metadata.Size;
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
 * does not assemble or link leaves no file behind. */
static int BuildImage(AssembleRequest *Request, BoxAsmBlob *Image)
{
    BoxAsmBlob Program = { 0 };
    char Error[ASSEMBLE_ERROR_BYTES] = { 0 };
    int Rc = BoxAsmSourceAssemble((const char *)Request->SourceBytes,
                                  (size_t)Request->SourceSize, Request->SourceName,
                                  &Program, Error, sizeof(Error));

    free(Request->SourceBytes);
    Request->SourceBytes = NULL;
    if (Rc != 0) {
        if (Error[0] != '\0')
            printf("assemble: %s\n", Error);
        else
            AssembleSayError(Request->SourceName, Rc, NULL);
        return Rc;
    }
    Rc = LinkEmbedded(Request->SourceName, &Program, Image, Error, sizeof(Error));
    if (Rc != 0)
        AssembleSayError("link", Rc, Error);
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
        exit(1);
    }
    Rc = AssemblePublish(&Request, &Image);
    free((void *)Image.Data);
    exit(Rc == 0 ? 0 : 1);
}
