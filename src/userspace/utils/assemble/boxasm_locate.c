/* Which file a name means: the one called so that wears the meaning asked
 * for, looked for among the tags of its home — the context the work began
 * in, or the tags a file reached past it was found by — or, everywhere, past
 * the home. One, or a refusal that says what is there and what would mean
 * the one wanted: a tag only it wears, or where the ones outside the home
 * are. Nothing is ever taken by a guess. */

#include "boxasm_source.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_LOCATE_SHOWN 8u          /* files listed before "and N more" */

/* ---- Tags ------------------------------------------------------------------ */

static int BoxAsmTagBlank(char C)
{
    return C == ' ' || C == '\t';
}

int BoxAsmNextTag(const char *List, size_t *At, BoxAsmTag *Out)
{
    while (List && List[*At] != '\0') {
        size_t Begin = *At;
        size_t End;
        size_t Colon;

        while (List[*At] != '\0' && List[*At] != ',')
            (*At)++;
        End = *At;
        if (List[*At] == ',')
            (*At)++;
        while (Begin < End && BoxAsmTagBlank(List[Begin]))
            Begin++;
        while (End > Begin && BoxAsmTagBlank(List[End - 1u]))
            End--;
        if (Begin == End)
            continue;
        Colon = Begin;
        while (Colon < End && List[Colon] != ':')
            Colon++;
        Out->Key = List + Begin;
        Out->KeyLength = Colon - Begin;
        Out->Value = Colon + 1u < End ? List + Colon + 1u : NULL;
        Out->ValueLength = Colon + 1u < End ? End - Colon - 1u : 0;
        return 1;
    }
    return 0;
}

int BoxAsmTagsHold(const char *List, const BoxAsmTag *Tag, int KeyOnly)
{
    BoxAsmTag Worn;
    size_t At = 0;

    while (BoxAsmNextTag(List, &At, &Worn)) {
        if (Worn.KeyLength != Tag->KeyLength || memcmp(Worn.Key, Tag->Key, Tag->KeyLength) != 0)
            continue;
        if (KeyOnly || (Worn.ValueLength == Tag->ValueLength &&
                        (Tag->ValueLength == 0 ||
                         memcmp(Worn.Value, Tag->Value, Tag->ValueLength) == 0)))
            return 1;
    }
    return 0;
}

const char *BoxAsmErrorText(int Rc)
{
    switch (box_errno_of(Rc)) {
    case OK: return "ok";
#define BOXASM_ERROR_TEXT(SUFFIX, name, value, msg, cat) case ERR_##SUFFIX: return msg;
    BOX_ERROR_LIST(BOXASM_ERROR_TEXT)
#undef BOXASM_ERROR_TEXT
    default: return "unknown error";
    }
}

/* ---- Saying it ------------------------------------------------------------- */

static int BoxAsmSay(BoxAsmBuffer *Out, const char *Text)
{
    return BoxAsmBufferAppend(Out, Text, strlen(Text));
}

static int BoxAsmSayTag(BoxAsmBuffer *Out, const BoxAsmTag *Tag)
{
    const char *End = Tag->Value ? Tag->Value + Tag->ValueLength : Tag->Key + Tag->KeyLength;

    return BoxAsmBufferAppend(Out, Tag->Key, (uint64_t)(End - Tag->Key));
}

/* A tag list as "a, b, c". */
static int BoxAsmSayTags(BoxAsmBuffer *Out, const char *List)
{
    BoxAsmTag Tag;
    size_t At = 0;
    int First = 1;
    int Rc = 0;

    while (Rc == 0 && BoxAsmNextTag(List, &At, &Tag)) {
        if (!First)
            Rc = BoxAsmSay(Out, ", ");
        if (Rc == 0)
            Rc = BoxAsmSayTag(Out, &Tag);
        First = 0;
    }
    if (Rc == 0 && First)
        Rc = BoxAsmSay(Out, "no tags");
    return Rc;
}

static int BoxAsmHasTags(const char *List)
{
    BoxAsmTag Tag;
    size_t At = 0;

    return BoxAsmNextTag(List, &At, &Tag);
}

/* "in demo, asm", or "on the volume" for a home of no tags. */
static int BoxAsmSayWhere(BoxAsmBuffer *Out, const char *Home)
{
    int Rc;

    if (!BoxAsmHasTags(Home))
        return BoxAsmSay(Out, "on the volume");
    if ((Rc = BoxAsmSay(Out, "in ")) != 0)
        return Rc;
    return BoxAsmSayTags(Out, Home);
}

/* ---- The files a name may mean --------------------------------------------- */

typedef struct {
    uint64_t *Ids;
    char    **Tags;      /* each file's tags, read once they are needed */
    int       Count;
} BoxAsmFound;

static void BoxAsmFoundRelease(BoxAsmFound *Found)
{
    int Index;

    for (Index = 0; Found->Tags && Index < Found->Count; Index++)
        free(Found->Tags[Index]);
    free(Found->Tags);
    free(Found->Ids);
    memset(Found, 0, sizeof(*Found));
}

/* "A,B", either part possibly empty. NULL when there is no memory. */
static char *BoxAsmJoin(const char *A, const char *B)
{
    size_t First = strlen(A);
    size_t Second = strlen(B);
    char *Out = (char *)malloc(First + Second + 2u);

    if (!Out)
        return NULL;
    memcpy(Out, A, First);
    Out[First] = ',';
    memcpy(Out + First + 1u, B, Second + 1u);
    return Out;
}

static int BoxAsmFindAll(BoxAsmFiles *Files, const char *Name, const char *Home, const char *Tags,
                         BoxAsmFound *Out)
{
    char *Wanted = BoxAsmJoin(Home, Tags);
    int Count;

    memset(Out, 0, sizeof(*Out));
    if (!Wanted)
        return -ERR_NO_MEMORY;
    Count = Files->Find(Files, Name, Wanted, &Out->Ids);
    free(Wanted);
    if (Count < 0) {
        free(Out->Ids);
        Out->Ids = NULL;
        return Count;
    }
    Out->Count = Count;
    return 0;
}

static int BoxAsmReadTags(BoxAsmFiles *Files, BoxAsmFound *Found)
{
    int Index;
    int Rc;

    if (Found->Count == 0 || Found->Tags)
        return 0;
    if (!(Found->Tags = (char **)calloc((size_t)Found->Count, sizeof(char *))))
        return -ERR_NO_MEMORY;
    for (Index = 0; Index < Found->Count; Index++) {
        if ((Rc = Files->Tags(Files, Found->Ids[Index], &Found->Tags[Index])) < 0)
            return Rc;
    }
    return 0;
}

/* The first tag the file Self wears that no other found file wears and the
 * line does not already say — the name's own stem aside, which every file of
 * that name wears. 0 when it wears none of its own. */
static int BoxAsmOwnTag(const BoxAsmFound *Found, int Self, const BoxAsmLookup *Lookup,
                        BoxAsmTag *Out)
{
    const char *Dot = strrchr(Lookup->Name, '.');
    size_t Stem = Dot ? (size_t)(Dot - Lookup->Name) : strlen(Lookup->Name);
    BoxAsmTag Tag;
    size_t At = 0;

    while (BoxAsmNextTag(Found->Tags[Self], &At, &Tag)) {
        int Other;
        int Shared = 0;

        if ((!Tag.Value && Tag.KeyLength == Stem && memcmp(Tag.Key, Lookup->Name, Stem) == 0) ||
            BoxAsmTagsHold(Lookup->Tags, &Tag, 0))
            continue;
        for (Other = 0; Other < Found->Count && !Shared; Other++)
            Shared = Other != Self && BoxAsmTagsHold(Found->Tags[Other], &Tag, 0);
        if (!Shared) {
            *Out = Tag;
            return 1;
        }
    }
    return 0;
}

/* `%include "NAME" kept own [everywhere]`, or incbin's: the line that means file Self —
 * the meaning asked for that it wears, and a tag only it wears. 0 with
 * nothing said when, among several, it wears no tag of its own. */
static int BoxAsmSayInclude(BoxAsmBuffer *Out, const BoxAsmFound *Found, int Self,
                            const BoxAsmLookup *Lookup, int Everywhere, int *Said)
{
    BoxAsmTag Own;
    BoxAsmTag Tag;
    size_t At = 0;
    int HasOwn = BoxAsmOwnTag(Found, Self, Lookup, &Own);
    int Rc;

    *Said = HasOwn || Found->Count == 1;
    if (!*Said)
        return 0;
    if ((Rc = BoxAsmSay(Out, Lookup->Directive)) != 0 || (Rc = BoxAsmSay(Out, " \"")) != 0 ||
        (Rc = BoxAsmSay(Out, Lookup->Name)) != 0 ||
        (Rc = BoxAsmSay(Out, "\"")) != 0)
        return Rc;
    while (BoxAsmNextTag(Lookup->Tags, &At, &Tag)) {
        if (BoxAsmTagsHold(Found->Tags[Self], &Tag, 0) &&
            ((Rc = BoxAsmSay(Out, " ")) != 0 || (Rc = BoxAsmSayTag(Out, &Tag)) != 0))
            return Rc;
    }
    if (HasOwn && ((Rc = BoxAsmSay(Out, " ")) != 0 || (Rc = BoxAsmSayTag(Out, &Own)) != 0))
        return Rc;
    return Everywhere ? BoxAsmSay(Out, " everywhere") : 0;
}

/* "    %include ...    for the one wearing ...", the lines lined up; one
 * that wears nothing the others do not is told to take a tag of its own. */
static int BoxAsmSayIncludes(BoxAsmBuffer *Out, const BoxAsmFound *Found, int Shown,
                             const BoxAsmLookup *Lookup, int Everywhere)
{
    uint64_t Widest = 0;
    int Pass;
    int Index;
    int Rc = 0;

    for (Pass = 0; Pass < 2 && Rc == 0; Pass++) {
        for (Index = 0; Rc == 0 && Index < Shown; Index++) {
            BoxAsmBuffer Line = { 0 };
            int Said;

            if ((Rc = BoxAsmSayInclude(&Line, Found, Index, Lookup, Everywhere, &Said)) == 0) {
                if (Pass == 0 && Line.Size > Widest)
                    Widest = Line.Size;
                if (Pass == 1 && Said) {
                    if ((Rc = BoxAsmSay(Out, "\n    ")) == 0 &&
                        (Rc = BoxAsmBufferAppend(Out, Line.Data, Line.Size)) == 0 &&
                        (Rc = BoxAsmBufferAppend(Out, "                                        ",
                                                 Widest - Line.Size > 40u ? 40u : Widest - Line.Size)) == 0 &&
                        (Rc = BoxAsmSay(Out, "    for the one wearing ")) == 0)
                        Rc = BoxAsmSayTags(Out, Found->Tags[Index]);
                } else if (Pass == 1 && (Rc = BoxAsmSay(Out, "\n    the one wearing ")) == 0 &&
                           (Rc = BoxAsmSayTags(Out, Found->Tags[Index])) == 0) {
                    Rc = BoxAsmSay(Out, " wears nothing the others do not: give it a tag of its own");
                }
            }
            BoxAsmBufferRelease(&Line);
        }
    }
    return Rc;
}

/* Each found file — as the line that means it, or by its tags — at most
 * BOXASM_LOCATE_SHOWN of them. */
static int BoxAsmSayFound(BoxAsmBuffer *Out, const BoxAsmFound *Found, const BoxAsmLookup *Lookup,
                          int AsLines, int Everywhere)
{
    int Shown = Found->Count < (int)BOXASM_LOCATE_SHOWN ? Found->Count : (int)BOXASM_LOCATE_SHOWN;
    int Index;
    int Rc = 0;

    if (AsLines)
        Rc = BoxAsmSayIncludes(Out, Found, Shown, Lookup, Everywhere);
    for (Index = 0; Rc == 0 && !AsLines && Index < Shown; Index++) {
        if ((Rc = BoxAsmSay(Out, "\n    ")) == 0)
            Rc = BoxAsmSayTags(Out, Found->Tags[Index]);
    }
    if (Rc == 0 && Found->Count > Shown) {
        char Number[24];
        size_t Length = 0;
        uint32_t More = (uint32_t)(Found->Count - Shown);

        do Number[Length++] = (char)('0' + More % 10u); while ((More /= 10u) != 0);
        if ((Rc = BoxAsmSay(Out, "\n    and ")) != 0)
            return Rc;
        while (Rc == 0 && Length != 0)
            Rc = BoxAsmBufferAppend(Out, &Number[--Length], 1);
        if (Rc == 0)
            Rc = BoxAsmSay(Out, " more");
    }
    return Rc;
}

/* ---- The refusals ------------------------------------------------------------ */

static int BoxAsmSaySeveral(const BoxAsmFound *Found, const BoxAsmLookup *Lookup, const char *Home,
                            BoxAsmBuffer *Summary, BoxAsmBuffer *Details)
{
    char Count[12];
    size_t Length = 0;
    uint32_t Value = (uint32_t)Found->Count;
    int Rc;

    do Count[Length++] = (char)('0' + Value % 10u); while ((Value /= 10u) != 0);
    if ((Rc = BoxAsmSay(Summary, Lookup->Name)) != 0 ||
        (Rc = BoxAsmSay(Summary, " could be any of ")) != 0)
        return Rc;
    while (Length != 0)
        if ((Rc = BoxAsmBufferAppend(Summary, &Count[--Length], 1)) != 0)
            return Rc;
    if ((Rc = BoxAsmSay(Summary, " files ")) != 0 || (Rc = BoxAsmSayWhere(Summary, Home)) != 0)
        return Rc;
    if (Lookup->Directive) {
        if ((Rc = BoxAsmSay(Details, "  say which by a tag only it wears:")) != 0)
            return Rc;
        return BoxAsmSayFound(Details, Found, Lookup, 1, Lookup->Everywhere);
    }
    if ((Rc = BoxAsmSay(Details, "  they wear:")) != 0 ||
        (Rc = BoxAsmSayFound(Details, Found, Lookup, 0, 0)) != 0)
        return Rc;
    return BoxAsmSay(Details, "\n  narrow the context with use");
}

/* Files of the name are in the home, and none wears all the meaning asked. */
static int BoxAsmSayMeaning(const BoxAsmFound *Found, const BoxAsmLookup *Lookup, const char *Home,
                            BoxAsmBuffer *Summary, BoxAsmBuffer *Details)
{
    BoxAsmTag Tag;
    size_t At = 0;
    int Several = 0;
    int Rc;

    BoxAsmNextTag(Lookup->Tags, &At, &Tag);
    Several = BoxAsmNextTag(Lookup->Tags, &At, &Tag);
    if ((Rc = BoxAsmSay(Summary, Found->Count == 1 ? "" : "no ")) != 0 ||
        (Rc = BoxAsmSay(Summary, Lookup->Name)) != 0 || (Rc = BoxAsmSay(Summary, " ")) != 0 ||
        (Rc = BoxAsmSayWhere(Summary, Home)) != 0 ||
        (Rc = BoxAsmSay(Summary, Found->Count == 1 ? " does not wear " : " wears ")) != 0 ||
        (Rc = BoxAsmSay(Summary, Several ? "all of " : "")) != 0 ||
        (Rc = BoxAsmSayTags(Summary, Lookup->Tags)) != 0 ||
        (Rc = BoxAsmSay(Details, Found->Count == 1 ? "  it wears:" : "  they wear:")) != 0)
        return Rc;
    return BoxAsmSayFound(Details, Found, Lookup, 0, 0);
}

/* None in the home; Named, the files of the name, are all outside it. */
static int BoxAsmSayOutside(const BoxAsmFound *Named, const BoxAsmLookup *Lookup, const char *Home,
                            BoxAsmBuffer *Summary, BoxAsmBuffer *Details)
{
    int Rc;

    if ((Rc = BoxAsmSay(Summary, "there is no ")) != 0 || (Rc = BoxAsmSay(Summary, Lookup->Name)) != 0 ||
        (Rc = BoxAsmSay(Summary, " ")) != 0 || (Rc = BoxAsmSayWhere(Summary, Home)) != 0 ||
        (Rc = BoxAsmSay(Details, Lookup->Directive ? "  outside it, reached by:" : "  outside it, wearing:")) != 0 ||
        (Rc = BoxAsmSayFound(Details, Named, Lookup, Lookup->Directive != NULL, 1)) != 0)
        return Rc;
    if (!Lookup->Directive)
        return BoxAsmSay(Details, "\n  change the context with use");
    if ((Rc = BoxAsmSay(Details, Named->Count == 1 ? "\n  or let it wear " : "\n  or let the one meant wear ")) != 0 ||
        (Rc = BoxAsmSayTags(Details, Home)) != 0)
        return Rc;
    return BoxAsmSay(Details, " too");
}

int BoxAsmLocate(BoxAsmFiles *Files, const BoxAsmLookup *Lookup, uint64_t *Id,
                 BoxAsmBuffer *Summary, BoxAsmBuffer *Details)
{
    const char *Home = Lookup->Everywhere ? "" : Lookup->Home;
    BoxAsmFound Found;
    BoxAsmFound Named;
    BoxAsmFound Here;
    int Rc;

    if ((Rc = BoxAsmFindAll(Files, Lookup->Name, Home, Lookup->Tags, &Found)) != 0)
        return Rc;
    if (Found.Count == 1) {
        *Id = Found.Ids[0];
        BoxAsmFoundRelease(&Found);
        return 0;
    }
    if (Found.Count > 1) {
        if ((Rc = BoxAsmReadTags(Files, &Found)) == 0 &&
            (Rc = BoxAsmSaySeveral(&Found, Lookup, Home, Summary, Details)) == 0)
            Rc = -ERR_INVALID_STATE;
        BoxAsmFoundRelease(&Found);
        return Rc;
    }
    BoxAsmFoundRelease(&Found);
    /* None: whether any file is called so, and whether those are here. */
    if ((Rc = BoxAsmFindAll(Files, Lookup->Name, "", "", &Named)) != 0)
        return Rc;
    if (Named.Count == 0) {
        BoxAsmFoundRelease(&Named);
        if ((Rc = BoxAsmSay(Summary, "there is no file named ")) == 0 &&
            (Rc = BoxAsmSay(Summary, Lookup->Name)) == 0)
            Rc = BoxAsmSay(Summary, BoxAsmHasTags(Home) ? " here or anywhere else" : " on the volume");
        return Rc == 0 ? -ERR_FILE_NOT_FOUND : Rc;
    }
    if ((Rc = BoxAsmFindAll(Files, Lookup->Name, Home, "", &Here)) != 0) {
        BoxAsmFoundRelease(&Named);
        return Rc;
    }
    if (Here.Count > 0) {
        if ((Rc = BoxAsmReadTags(Files, &Here)) == 0)
            Rc = BoxAsmSayMeaning(&Here, Lookup, Home, Summary, Details);
    } else if ((Rc = BoxAsmReadTags(Files, &Named)) == 0) {
        Rc = BoxAsmSayOutside(&Named, Lookup, Home, Summary, Details);
    }
    BoxAsmFoundRelease(&Here);
    BoxAsmFoundRelease(&Named);
    return Rc == 0 ? -ERR_FILE_NOT_FOUND : Rc;
}
