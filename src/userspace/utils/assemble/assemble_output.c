#include "assemble.h"

#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/print.h"
#include "box/string.h"

#define ASSEMBLE_ANSWER_BYTES 32u

/* A file already called by the program's name, and whether it wears every
 * tag the program will wear. */
typedef struct {
    file_id_t    Id;
    FileMetadata Metadata;
    int          Exact;
} AssembleCandidate;

static int TagBlank(char C)
{
    return C == ' ' || C == '\t';
}

int AssembleTagsHaveKey(const char *Tags, const char *Key)
{
    BoxAsmTag Wanted = { Key, strlen(Key), NULL, 0 };

    return BoxAsmTagsHold(Tags, &Wanted, 1);
}

static int WearsEvery(const char *Worn, const char *Wanted)
{
    BoxAsmTag Tag;
    size_t At = 0;

    while (BoxAsmNextTag(Wanted, &At, &Tag)) {
        if (!BoxAsmTagsHold(Worn, &Tag, 0))
            return 0;
    }
    return 1;
}

/* The tags a program wears in place of the one before it: every tag the
 * build gives it, then each of the old one's whose key the build does not
 * speak of — a rebuild changes what the program is made of, never what it
 * was told. NULL when there is no memory; the caller frees the list. */
static char *CarriedTags(const char *Old, const char *New)
{
    size_t OldLength = Old ? strlen(Old) : 0;
    size_t Used = strlen(New);
    char *Out = (char *)malloc(Used + OldLength + 2u);
    BoxAsmTag Tag;
    size_t At = 0;

    if (!Out)
        return NULL;
    memcpy(Out, New, Used);
    while (BoxAsmNextTag(Old, &At, &Tag)) {
        const char *End = Tag.Value ? Tag.Value + Tag.ValueLength : Tag.Key + Tag.KeyLength;

        if (BoxAsmTagsHold(New, &Tag, 1))
            continue;
        if (Used != 0)
            Out[Used++] = ',';
        memcpy(Out + Used, Tag.Key, (size_t)(End - Tag.Key));
        Used += (size_t)(End - Tag.Key);
    }
    Out[Used] = '\0';
    return Out;
}

static void ReleaseCandidates(AssembleCandidate *List, int Count)
{
    int Index;

    for (Index = 0; Index < Count; Index++)
        FileMetadataRelease(&List[Index].Metadata);
    free(List);
}

/* Every file in the Use Context called by the program's name, each with its
 * whole record. A file let go between the count and the read is passed over. */
static int GatherCandidates(const AssembleRequest *Request,
                            AssembleCandidate **OutList, int *OutCount)
{
    int Count = find_file_by_name(Request->OutputName, NULL, NULL, 0);
    file_id_t *Ids;
    AssembleCandidate *List;
    int Found;
    int Kept = 0;
    int Index;

    *OutList = NULL;
    *OutCount = 0;
    if (Count <= 0)
        return Count;
    Ids = (file_id_t *)malloc((size_t)Count * sizeof(file_id_t));
    if (!Ids)
        return -ERR_NO_MEMORY;
    Found = find_file_by_name(Request->OutputName, Ids, NULL, (size_t)Count);
    if (Found < 0) {
        free(Ids);
        return Found;
    }
    if (Found > Count)
        Found = Count;
    List = (AssembleCandidate *)malloc((size_t)(Found ? Found : 1) * sizeof(AssembleCandidate));
    if (!List) {
        free(Ids);
        return -ERR_NO_MEMORY;
    }
    for (Index = 0; Index < Found; Index++) {
        AssembleCandidate *Candidate = &List[Kept];
        int Rc;

        memset(Candidate, 0, sizeof(*Candidate));
        Rc = FileMetadataRead(Ids[Index], &Candidate->Metadata);
        if (Rc == -ERR_FILE_NOT_FOUND)
            continue;
        if (Rc != 0) {
            free(Ids);
            ReleaseCandidates(List, Kept);
            return Rc;
        }
        Candidate->Id = Ids[Index];
        Candidate->Exact = WearsEvery(Candidate->Metadata.Tags, Request->Tags);
        Kept++;
    }
    free(Ids);
    *OutList = List;
    *OutCount = Kept;
    return 0;
}

static int AnswerIs(const char *Answer, const char *Word)
{
    size_t Index;

    for (Index = 0; Word[Index] != '\0'; Index++) {
        char C = Answer[Index];

        if (C >= 'A' && C <= 'Z')
            C = (char)(C - 'A' + 'a');
        if (C != Word[Index])
            return 0;
    }
    return Answer[Index] == '\0';
}

/* One answer from whoever is at the console, the blanks around it dropped.
 * -1 when there is no one to hear it. */
static int Ask(const char *Prompt, char *Answer, size_t Capacity)
{
    size_t Begin = 0;
    size_t End;

    if (input(Prompt, Answer, Capacity) < 0)
        return -1;
    End = strlen(Answer);
    while (End > 0 && TagBlank(Answer[End - 1u]))
        End--;
    Answer[End] = '\0';
    while (TagBlank(Answer[Begin]))
        Begin++;
    memmove(Answer, Answer + Begin, End - Begin + 1u);
    return 0;
}

static void SayNoOneToAsk(const AssembleRequest *Request)
{
    printf("assemble: nothing written: no one is here to say which %s the program replaces;\n"
           "name it by a tag it wears: assemble %s tag ...\n",
           Request->OutputName, Request->SourceName);
}

/* "there is another NAME — replace?": *Chosen 0 to replace it, -1 to make
 * a new program beside it. */
static int AskReplace(const AssembleRequest *Request, const AssembleCandidate *Other,
                      int *Chosen)
{
    char Answer[ASSEMBLE_ANSWER_BYTES];

    printf("there is another %s, tags %s\n", Request->OutputName,
           Other->Metadata.Tags ? Other->Metadata.Tags : "(none)");
    for (;;) {
        if (Ask("replace it? (yes, or no to make a new one beside it) ",
                Answer, sizeof(Answer)) != 0) {
            SayNoOneToAsk(Request);
            return -ERR_INVALID_STATE;
        }
        if (AnswerIs(Answer, "yes") || AnswerIs(Answer, "y")) {
            *Chosen = 0;
            return 0;
        }
        if (AnswerIs(Answer, "no") || AnswerIs(Answer, "n")) {
            *Chosen = -1;
            return 0;
        }
    }
}

/* The files that could be meant, numbered with their tags: *Chosen the
 * index in List of the one to replace, or -1 to make a new program. Only the
 * ones wearing every tag are offered when there are several of them. */
static int AskWhich(const AssembleRequest *Request, const AssembleCandidate *List,
                    int Count, int ExactOnly, int *Chosen)
{
    char Answer[ASSEMBLE_ANSWER_BYTES];
    int Shown = 0;
    int Index;

    printf("%s could mean any of these:\n", Request->OutputName);
    for (Index = 0; Index < Count; Index++) {
        if (ExactOnly && !List[Index].Exact)
            continue;
        printf("  %d: tags %s\n", ++Shown,
               List[Index].Metadata.Tags ? List[Index].Metadata.Tags : "(none)");
    }
    for (;;) {
        int Number = 0;
        size_t At = 0;

        if (Ask("which one does the program replace? (a number, or new) ",
                Answer, sizeof(Answer)) != 0) {
            SayNoOneToAsk(Request);
            return -ERR_INVALID_STATE;
        }
        if (AnswerIs(Answer, "new")) {
            *Chosen = -1;
            return 0;
        }
        while (Answer[At] >= '0' && Answer[At] <= '9' && Number <= Shown)
            Number = Number * 10 + (Answer[At++] - '0');
        if (At == 0 || Answer[At] != '\0' || Number < 1 || Number > Shown)
            continue;
        for (Index = 0; Index < Count; Index++) {
            if (ExactOnly && !List[Index].Exact)
                continue;
            if (--Number == 0) {
                *Chosen = Index;
                return 0;
            }
        }
    }
}

/* Which file the program replaces: the one wearing every tag it will wear,
 * asked about when that is not one file. *Chosen is its index, or -1 for a
 * new program. */
static int ChooseReplaced(const AssembleRequest *Request, const AssembleCandidate *List,
                          int Count, int *Chosen)
{
    int Exact = 0;
    int Index;

    *Chosen = -1;
    for (Index = 0; Index < Count; Index++) {
        if (List[Index].Exact) {
            Exact++;
            *Chosen = Index;
        }
    }
    if (Count == 0 || Exact == 1)
        return 0;
    if (Count == 1)
        return AskReplace(Request, &List[0], Chosen);
    return AskWhich(Request, List, Count, Exact > 1, Chosen);
}

/* create, span, every byte written, anchor — and on any refusal after the
 * create, the half-made file deleted so no artifact pretends to be whole. */
static int WriteImage(const char *OutputName, const char *Tags,
                      const BoxAsmBlob *Image, file_id_t *OutFile)
{
    int64_t FileId = create(OutputName, Tags);
    uint64_t Offset = 0;
    int Rc;

    if (FileId < 0) {
        AssembleSayError("writing the program", (int)FileId, NULL);
        return (int)FileId;
    }
    Rc = file_span((file_id_t)FileId, Image->Size, 0);
    while (Rc == 0 && Offset < Image->Size) {
        uint64_t Remaining = Image->Size - Offset;
        size_t Ask = Remaining > 65536u ? 65536u : (size_t)Remaining;
        int64_t Written = fwrite((file_id_t)FileId, Offset, Image->Data + Offset, Ask);

        if (Written <= 0 || (uint64_t)Written > Remaining) {
            Rc = Written < 0 ? (int)Written : -ERR_IO;
            break;
        }
        Offset += (uint64_t)Written;
    }
    if (Rc == 0)
        Rc = anchor((file_id_t)FileId);
    if (Rc != 0) {
        AssembleSayError("writing the program", Rc, NULL);
        if (delete((file_id_t)FileId) != 0)
            printf("assemble: the half-written %s, file %llu, could not be deleted\n",
                   OutputName, (unsigned long long)FileId);
        return Rc;
    }
    *OutFile = (file_id_t)FileId;
    return 0;
}

/* The new program written whole and anchored before the old one is let go,
 * so a refusal anywhere leaves the old one as it was. */
static int ReplaceProgram(const AssembleRequest *Request, const BoxAsmBlob *Image,
                          const AssembleCandidate *Old, file_id_t *OutFile)
{
    char *Tags = CarriedTags(Old->Metadata.Tags, Request->Tags);
    int Rc;

    if (!Tags) {
        AssembleSayError("writing the program", -ERR_NO_MEMORY, NULL);
        return -ERR_NO_MEMORY;
    }
    Rc = WriteImage(Request->OutputName, Tags, Image, OutFile);
    free(Tags);
    if (Rc != 0)
        return Rc;
    Rc = delete(Old->Id);
    if (Rc != 0) {
        printf("assemble: %s could not be replaced: %s; it is as it was\n",
               Request->OutputName, BoxAsmErrorText(Rc));
        if (delete(*OutFile) != 0)
            printf("assemble: the new %s, file %llu, could not be deleted\n",
                   Request->OutputName, (unsigned long long)*OutFile);
    }
    return Rc;
}

/* What was made, the tags it wears, and app when it is new to the program:
 * a program put in place of one that wore app was given nothing. */
static void SayPublished(const AssembleRequest *Request, file_id_t FileId,
                         const AssembleCandidate *Replaced)
{
    FileMetadata Metadata;

    printf("assembled %s -> %s%s\n", Request->SourceName, Request->OutputName,
           Replaced ? ", in place of the one before" : "");
    memset(&Metadata, 0, sizeof(Metadata));
    if (FileMetadataRead(FileId, &Metadata) == 0) {
        printf("tags: %s\n", Metadata.Tags ? Metadata.Tags : "(none)");
        FileMetadataRelease(&Metadata);
    }
    if (Request->AddedApp &&
        !(Replaced && AssembleTagsHaveKey(Replaced->Metadata.Tags, "app")))
        println("added app: without app or utility the shell cannot start it");
}

/* A program made again by its Recipe takes its own place: no name is looked
 * up and nothing is asked — it is the one handed over. */
static int RemakeInPlace(const AssembleRequest *Request, const BoxAsmBlob *Image,
                         file_id_t *Published)
{
    AssembleCandidate Old;
    int Rc;

    memset(&Old, 0, sizeof(Old));
    Old.Id = Request->Remake;
    Rc = FileMetadataRead(Old.Id, &Old.Metadata);
    if (Rc != 0) {
        AssembleSayError(Request->RemakeName, Rc, NULL);
        return Rc;
    }
    Rc = ReplaceProgram(Request, Image, &Old, Published);
    if (Rc == 0)
        SayPublished(Request, *Published, &Old);
    FileMetadataRelease(&Old.Metadata);
    return Rc;
}

int AssemblePublish(const AssembleRequest *Request, const BoxAsmBlob *Image,
                    file_id_t *Published)
{
    AssembleCandidate *List;
    file_id_t OutputId = 0;
    int Count;
    int Chosen;
    int Index;
    int Rc;

    if (Request->Remake != 0)
        return RemakeInPlace(Request, Image, Published);
    Rc = GatherCandidates(Request, &List, &Count);

    if (Rc != 0) {
        AssembleSayError("output lookup", Rc, NULL);
        return Rc;
    }
    for (Index = 0; Index < Count; Index++) {
        if (List[Index].Id == Request->SourceId) {
            printf("assemble: %s is the source itself; name the program with ->\n",
                   Request->OutputName);
            ReleaseCandidates(List, Count);
            return -ERR_INVALID_ARGUMENT;
        }
    }
    Rc = ChooseReplaced(Request, List, Count, &Chosen);
    if (Rc == 0 && Chosen < 0)
        Rc = WriteImage(Request->OutputName, Request->Tags, Image, &OutputId);
    else if (Rc == 0)
        Rc = ReplaceProgram(Request, Image, &List[Chosen], &OutputId);
    if (Rc == 0) {
        SayPublished(Request, OutputId, Chosen >= 0 ? &List[Chosen] : NULL);
        *Published = OutputId;
    }
    ReleaseCandidates(List, Count);
    return Rc;
}
