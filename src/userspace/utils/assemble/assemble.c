#include "boxasm_elf.h"
#include "boxasm_link.h"
#include "boxasm_source.h"

#include "box/file.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"
#include "box/system.h"

extern const uint8_t _binary____build_boxlib_start_o_start[];
extern const uint8_t _binary____build_boxlib_start_o_end[];
extern const uint8_t _binary____boxlib_libbox_a_start[];
extern const uint8_t _binary____boxlib_libbox_a_end[];

static void SayError(const char *Stage, int Rc, const char *Detail)
{
    if (Detail && Detail[0] != '\0')
        printf("assemble: %s: %s (error %d)\n", Stage, Detail, Rc);
    else
        printf("assemble: %s (error %d)\n", Stage, Rc);
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

static int TokenHasKey(const char *Tags, const char *Wanted)
{
    size_t At = 0;
    size_t WantLength = strlen(Wanted);

    while (Tags && Tags[At] != '\0') {
        size_t Begin = At;
        size_t Length;
        while (Tags[At] != '\0' && Tags[At] != ',')
            At++;
        Length = At - Begin;
        if (Length > WantLength && memcmp(Tags + Begin, Wanted, WantLength) == 0 &&
            Tags[Begin + WantLength] == ':')
            return 1;
        if (Length == WantLength && memcmp(Tags + Begin, Wanted, WantLength) == 0)
            return 1;
        if (Tags[At] == ',')
            At++;
    }
    return 0;
}

static int AppendExplicitTags(char *Buffer, size_t Capacity, size_t *Used,
                              int WordCount, int TagWordStart)
{
    int Index;
    int HasApp = TokenHasKey(Buffer, "app");
    int HasUtility = TokenHasKey(Buffer, "utility");

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
        if (TokenHasKey(Clean, "app")) HasApp = 1;
        if (TokenHasKey(Clean, "utility")) HasUtility = 1;
    }
    if (!HasApp && !HasUtility && AppendTags(Buffer, Capacity, Used, "app") != 0)
        return -ERR_INVALID_ARGUMENT;
    return 0;
}

static int FindSource(const char *Name, file_id_t *OutFile)
{
    file_id_t *Ids = NULL;
    int Count = find_file_by_name(Name, NULL, NULL, 0);
    int Found;

    if (Count < 0)
        return Count;
    if (Count == 0)
        return -ERR_FILE_NOT_FOUND;
    if (Count != 1)
        return -ERR_INVALID_STATE;
    Ids = (file_id_t *)malloc(sizeof(*Ids));
    if (!Ids)
        return -ERR_NO_MEMORY;
    Found = find_file_by_name(Name, Ids, NULL, 1u);
    if (Found != 1) {
        free(Ids);
        return Found < 0 ? Found : -ERR_FILE_NOT_FOUND;
    }
    *OutFile = Ids[0];
    free(Ids);
    return 0;
}

static int LinkEmbedded(BoxAsmBlob *Program, BoxAsmBlob *Image,
                        char *Error, size_t ErrorCapacity)
{
    BoxAsmBlob Entry = { _binary____build_boxlib_start_o_start,
                         (uint64_t)(_binary____build_boxlib_start_o_end -
                                    _binary____build_boxlib_start_o_start),
                         "boxlib_start.o" };
    BoxAsmBlob RuntimeArchive = { _binary____boxlib_libbox_a_start,
                                  (uint64_t)(_binary____boxlib_libbox_a_end -
                                             _binary____boxlib_libbox_a_start),
                                  "libbox.a" };
    int Rc = BoxAsmLink(&Entry, Program, &RuntimeArchive, Image,
                        Error, ErrorCapacity);

    free((void *)Program->Data);
    Program->Data = NULL;
    Program->Size = 0;
    return Rc;
}

static int PublishImage(const char *OutputName, const char *Tags,
                        const BoxAsmBlob *Image, int *CleanupFailed)
{
    int64_t FileId;
    int Rc;
    uint64_t Offset = 0;

    if (!OutputName || !Tags || !Image || !CleanupFailed)
        return -ERR_INVALID_ARGUMENT;
    *CleanupFailed = 0;
    FileId = create(OutputName, Tags);
    if (FileId < 0)
        return (int)FileId;
    Rc = file_span((file_id_t)FileId, Image->Size, 0);
    while (Rc == 0 && Offset < Image->Size) {
        uint64_t Remaining = Image->Size - Offset;
        size_t Ask = Remaining > 65536u ? 65536u : (size_t)Remaining;
        int64_t Written = fwrite((file_id_t)FileId, Offset,
                                 Image->Data + Offset, Ask);

        if (Written <= 0 || (uint64_t)Written > Remaining) {
            Rc = Written < 0 ? (int)Written : -ERR_IO;
            break;
        }
        Offset += (uint64_t)Written;
    }
    if (Rc == 0)
        Rc = anchor((file_id_t)FileId);
    if (Rc != 0 && delete((file_id_t)FileId) != 0)
        *CleanupFailed = 1;
    return Rc;
}

int main(void)
{
    int WordCount = (int)luggage_word_count();
    const char *SourceName;
    const char *RequestedOutput = NULL;
    char OutputName[64];
    char Tags[2048];
    size_t TagLength = 0;
    file_id_t SourceId;
    FileMetadata Metadata;
    uint8_t *SourceBytes = NULL;
    BoxAsmBlob Program = { 0 };
    BoxAsmBlob Image = { 0 };
    char Error[256] = { 0 };
    int TagWordStart = 2;
    int Rc;

    if (WordCount < 2) {
        println("Usage: assemble program.asm [-> program.elf] [tags ...]");
        exit(1);
    }
    SourceName = luggage_word(1);
    if (WordCount >= 4 && strcmp(luggage_word(2), "->") == 0)
        RequestedOutput = luggage_word(3);
    if (RequestedOutput)
        TagWordStart = 4;
    Rc = BuildOutputName(SourceName, RequestedOutput, OutputName,
                         sizeof(OutputName));
    if (Rc != 0) {
        SayError("output name", Rc, NULL);
        exit(1);
    }
    Rc = FindSource(SourceName, &SourceId);
    if (Rc != 0) {
        SayError("source lookup", Rc, NULL);
        exit(1);
    }
    memset(&Metadata, 0, sizeof(Metadata));
    Rc = FileMetadataRead(SourceId, &Metadata);
    if (Rc != 0) {
        SayError("source metadata", Rc, NULL);
        exit(1);
    }
    Rc = ReadFileBytes(SourceId, Metadata.Size, &SourceBytes);
    if (Rc != 0) {
        FileMetadataRelease(&Metadata);
        SayError("source read", Rc, NULL);
        exit(1);
    }
    memset(Tags, 0, sizeof(Tags));
    if (AppendTags(Tags, sizeof(Tags), &TagLength, Metadata.Tags) != 0 ||
        AppendExplicitTags(Tags, sizeof(Tags), &TagLength, WordCount,
                           TagWordStart) != 0) {
        free(SourceBytes);
        FileMetadataRelease(&Metadata);
        SayError("output tags", -ERR_INVALID_ARGUMENT, NULL);
        exit(1);
    }
    Rc = BoxAsmSourceAssemble((const char *)SourceBytes, (size_t)Metadata.Size,
                              &Program, Error, sizeof(Error));
    free(SourceBytes);
    FileMetadataRelease(&Metadata);
    if (Rc != 0) {
        SayError("assembly", Rc, Error);
        exit(1);
    }
    Rc = LinkEmbedded(&Program, &Image, Error, sizeof(Error));
    if (Rc != 0) {
        SayError("link", Rc, Error);
        exit(1);
    }

    {
        int CleanupFailed;
        Rc = PublishImage(OutputName, Tags, &Image, &CleanupFailed);
        free((void *)Image.Data);
        Image.Data = NULL;
        if (Rc != 0) {
            SayError("publish output", Rc,
                     CleanupFailed ? "cleanup failed" : NULL);
            exit(1);
        }
    }
    printf("assembled %s -> %s\n", SourceName, OutputName);
    exit(0);
}
