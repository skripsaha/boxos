/* copy — a file again under another name, with its tags.
 *
 *   copy note.txt -> note2.txt
 *
 * The arrow says which way the copy goes: the copy is the result, and `->`
 * points at where it is put, so the direction is said, not guessed from the
 * order of two words.
 *
 * The copy is a new file: the same bytes, the source's tags, the name given.
 * It is made with create(), so it belongs to the Use Context the person works
 * in as every file made now does, and the source is not touched. A name is
 * not given to two files: the shell would have to ask "which one" every time
 * it was said. A name that one file wears already is the person's to settle:
 * the old file goes to the trash, where it can be had back, and the copy
 * takes its name.
 *
 * Room is asked for first, the whole size at once, so a volume without room
 * says so before a byte is copied. A copy that fails part way is erased and
 * the person is told nothing was left behind.
 */

#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "box/cxx/error.h"
#include "box/display.h"
#include "box/file.h"
#include "box/form.h"
#include "box/print.h"
#include "box/time.h"

BOX_FORM("takes thing \"the file to copy\"\n"
         "takes into \"the copy's name\"\n"
         "gives things\n");

namespace {

constexpr std::size_t   ChunkBytes     = 256 * 1024;
constexpr std::size_t   LongestName    = 31;
constexpr std::uint64_t SlowAfterMs    = 1000;
constexpr std::uint64_t RepaintEveryMs = 250;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

void Tell(std::string_view Text)
{
    std::string Line(Text);
    println(Line.c_str());
}

std::string Why(std::int64_t Answer)
{
    return std::string(box::error(static_cast<::error_t>(-Answer)).message());
}

std::string Grouped(std::uint64_t Number)
{
    std::string Digits = std::to_string(Number);
    std::string Result;
    for (std::size_t I = 0; I < Digits.size(); I++) {
        if (I > 0 && (Digits.size() - I) % 3 == 0) Result += ' ';
        Result += Digits[I];
    }
    return Result;
}

/* The name up to its last dot: the tag every file wears for its name. */
std::string_view StemOf(std::string_view Name)
{
    std::size_t Dot = Name.rfind('.');
    return Dot == std::string_view::npos ? Name : Name.substr(0, Dot);
}

/* A file's tags as the person is told them: all but the one it wears for its
 * own name. */
std::string ShownTags(const char *Tags, std::string_view Name)
{
    std::string Shown;
    if (!Tags) return "no tags";
    std::string_view Rest = Tags;
    while (!Rest.empty()) {
        std::size_t      Comma = Rest.find(',');
        std::string_view Tag   = Rest.substr(0, Comma);
        Rest = Comma == std::string_view::npos ? std::string_view() : Rest.substr(Comma + 1);
        if (Tag.empty() || Tag == StemOf(Name)) continue;
        if (!Shown.empty()) Shown += ", ";
        Shown += Tag;
    }
    return Shown.empty() ? "no tags" : "tags " + Shown;
}

struct CarriedTags {
    std::string              Joined;
    std::vector<std::string> Kept;
};

/* The source's tags but its name's own, which the new name makes afresh. */
CarriedTags TagsToCarry(const char *Tags, std::string_view SourceName)
{
    CarriedTags Carried;
    if (!Tags) return Carried;
    std::string_view Rest = Tags;
    std::string_view Stem = StemOf(SourceName);
    while (!Rest.empty()) {
        std::size_t     Comma = Rest.find(',');
        std::string_view Tag  = Rest.substr(0, Comma);
        Rest = Comma == std::string_view::npos ? std::string_view() : Rest.substr(Comma + 1);
        if (Tag.empty() || Tag == Stem) continue;
        if (!Carried.Joined.empty()) Carried.Joined += ',';
        Carried.Joined += Tag;
        Carried.Kept.emplace_back(Tag);
    }
    return Carried;
}

std::string JoinedWith(const std::vector<std::string> &Words, std::string_view Between)
{
    std::string Result;
    for (const std::string &Word : Words) {
        if (!Result.empty()) Result += Between;
        Result += Word;
    }
    return Result;
}

/* The reason the new name cannot be had at all, or empty. */
std::string NameRefusal(std::string_view NewName, std::string_view SourceName)
{
    if (NewName.empty()) return "copy needs a name for the copy";
    if (NewName.size() > LongestName)
        return std::format("'{}' is too long: a name is {} characters or fewer", NewName, LongestName);
    if (NewName == SourceName)
        return std::format("{} is the file itself: give the copy another name", NewName);
    return {};
}

enum class Answer { Yes, No, NoOne };

bool Blank(char Ch)
{
    return Ch == ' ' || Ch == '\t';
}

/* yes or no, asked again on anything else; NoOne when nobody is there to hear. */
Answer AskReplace(std::string_view Name, const std::string &Tags)
{
    Say(std::format("there is another {}, {}", Name, Tags));
    for (;;) {
        char Reply[64];
        if (input("replace it? (yes or no) ", Reply, sizeof(Reply)) < 0) return Answer::NoOne;
        std::string_view Word = Reply;
        while (!Word.empty() && Blank(Word.front())) Word.remove_prefix(1);
        while (!Word.empty() && Blank(Word.back())) Word.remove_suffix(1);
        if (Word == "yes" || Word == "y") return Answer::Yes;
        if (Word == "no" || Word == "n") return Answer::No;
    }
}

/* Whether the name is taken and the person agrees to replace it: no Refusal
 * and Replaced 0 when it is free, Replaced the file for the trash when the
 * person says yes. */
struct NameSettled {
    std::string Refusal;
    file_id_t   Replaced = 0;
};

NameSettled SettleTakenName(std::string_view NewName)
{
    NameSettled Settled;
    std::string Name(NewName);
    file_id_t   Found[2];
    int         Count = find_file_by_name(Name.c_str(), "trashed", Found, nullptr, 2);
    if (Count < 0) {
        Settled.Refusal = std::format("could not look for a file called {}: {}", NewName, Why(Count));
        return Settled;
    }
    if (Count == 0) return Settled;
    if (Count > 1) {
        Settled.Refusal = std::format("{} files are called {} here already: give the copy another name",
                                      Count, NewName);
        return Settled;
    }

    FileMetadata Old;
    if (FileMetadataRead(Found[0], &Old) != 0) {
        Settled.Refusal = std::format("{} is here, but that file would not be looked at; it may be being erased",
                                      NewName);
        return Settled;
    }
    std::string Tags = ShownTags(Old.Tags, NewName);
    FileMetadataRelease(&Old);

    switch (AskReplace(NewName, Tags)) {
    case Answer::Yes:
        Settled.Replaced = Found[0];
        break;
    case Answer::No:
        Settled.Refusal = std::format("nothing was copied: {} stays as it is", NewName);
        break;
    case Answer::NoOne:
        Settled.Refusal = std::format("nothing was copied: no one is here to say whether {} is replaced; "
                                      "give the copy another name, or trash that file first", NewName);
        break;
    }
    return Settled;
}

/* A slow copy shows how far it is, on the screen only, one line rewritten
 * where it stands: '\r' takes the caret back to the line's start, and the
 * line is always as wide as the screen's last column so no old text shows
 * through. */
class Progress {
public:
    explicit Progress(std::uint64_t Total) : Total(Total), Began(Now())
    {
        std::uint32_t Cols = 0, Rows = 0;
        if (display_size(&Cols, &Rows) == 0 && Cols > 8) Width = Cols - 1u;
    }

    void Update(std::uint64_t Done)
    {
        std::uint64_t Moment = Now();
        if (Moment - Began < SlowAfterMs) return;
        if (Shown && Moment - Last < RepaintEveryMs) return;
        Last  = Moment;
        Shown = true;
        std::uint64_t Percent = Total == 0 ? 100 : Done * 100 / Total;
        Paint(std::format("copying {}%  {} of {} bytes", Percent, Grouped(Done), Grouped(Total)));
    }

    void Clear()
    {
        if (!Shown) return;
        Paint("");
        std::string Home = "\r";
        print_to_screen(Home.c_str());
        Shown = false;
    }

private:
    static std::uint64_t Now()
    {
        std::uint64_t Milliseconds = 0;
        time_uptime_ms(&Milliseconds);
        return Milliseconds;
    }

    void Paint(std::string Text)
    {
        Text.resize(Width, ' ');
        Text.insert(Text.begin(), '\r');
        print_to_screen(Text.c_str());
    }

    std::uint64_t Total;
    std::uint64_t Began;
    std::uint64_t Last  = 0;
    std::size_t   Width = 79;
    bool          Shown = false;
};

/* One piece of the source into the copy: the bytes read at Offset, written
 * whole at Offset. A write the volume refused part way answers a short count,
 * and goes on from there until it is whole or refused outright. */
std::string CopyPiece(file_id_t Source, file_id_t Copy, std::uint64_t Offset, std::uint64_t Want,
                      std::vector<std::uint8_t> &Buffer, std::uint64_t &Moved)
{
    std::int64_t Got = fread(Source, Offset, Buffer.data(), static_cast<std::size_t>(Want));
    if (Got < 0) return std::format("the original would not be read at byte {}: {}", Grouped(Offset), Why(Got));
    if (Got == 0) return std::format("the original ends at byte {}, short of what it was", Grouped(Offset));

    std::int64_t Done = 0;
    while (Done < Got) {
        std::int64_t Wrote = fwrite(Copy, Offset + static_cast<std::uint64_t>(Done), Buffer.data() + Done,
                                    static_cast<std::size_t>(Got - Done));
        if (Wrote < 0) return std::format("the copy would not be written at byte {}: {}",
                                          Grouped(Offset + static_cast<std::uint64_t>(Done)), Why(Wrote));
        if (Wrote == 0) return std::format("the volume took no more of the copy at byte {}",
                                           Grouped(Offset + static_cast<std::uint64_t>(Done)));
        Done += Wrote;
    }
    Moved = static_cast<std::uint64_t>(Got);
    return {};
}

/* The bytes of the source into the copy, room made for all of them first.
 * Empty when the copy is whole, else why it is not. */
std::string FillCopy(file_id_t Source, file_id_t Copy, std::uint64_t Size, std::vector<std::uint8_t> &Buffer)
{
    try {
        if (Size == 0) return {};
        int Roomed = file_span(Copy, Size, 0);
        if (Roomed == -static_cast<int>(ERR_TAGFS_NO_SPACE))
            return std::format("the volume has no room for {} bytes", Grouped(Size));
        if (Roomed != 0) return std::format("room for {} bytes could not be made: {}", Grouped(Size), Why(Roomed));

        Progress Shown(Size);
        std::uint64_t At = 0;
        while (At < Size) {
            std::uint64_t Moved = 0;
            std::uint64_t Want  = Size - At > ChunkBytes ? ChunkBytes : Size - At;
            std::string   Failure = CopyPiece(Source, Copy, At, Want, Buffer, Moved);
            if (!Failure.empty()) {
                Shown.Clear();
                return Failure;
            }
            At += Moved;
            Shown.Update(At);
        }
        Shown.Clear();
        return {};
    } catch (const std::bad_alloc &) {
        return "there was no memory left";
    }
}

/* The old file back out of the trash after a copy that was not made: what is
 * said of it, or empty when no file was replaced. */
std::string TakeBack(file_id_t Old, std::string_view Name)
{
    if (Old == 0) return {};
    int Back = tag_remove(Old, "trashed");
    if (Back == 0) return std::format(" The old {} is back, as it was.", Name);
    return std::format(" The old {} could not be taken out of the trash ({}): it is there under that name.",
                       Name, Why(Back));
}

/* A copy that failed is erased, so none is left half made. */
void Abandon(file_id_t Copy, std::string_view Name, const std::string &Failure, file_id_t Old)
{
    int Erased = file_delete(Copy);
    if (Erased == 0)
        Say(std::format("copy: {} was not made: {}. Nothing was left behind.{}", Name, Failure,
                        TakeBack(Old, Name)));
    else
        Say(std::format("copy: {} was not finished: {}. It could not be erased either ({}): "
                        "the half copy stands under that name.{}", Name, Failure, Why(Erased),
                        TakeBack(Old, Name)));
}

void ReportCopied(std::string_view From, std::string_view To, std::uint64_t Size, const CarriedTags &Carried,
                  bool Replaced)
{
    std::string Tags = Carried.Kept.empty() ? "no tags" : "tags " + JoinedWith(Carried.Kept, ", ");
    Tell(std::format("copied {} to {}{}, {} bytes, {}", From, To,
                     Replaced ? std::format(" (the old {} is in the trash)", To) : std::string(),
                     Grouped(Size), Tags));
}

int CopyFile(file_id_t Source, std::string_view NewName)
{
    FileMetadata Metadata;
    if (FileMetadataRead(Source, &Metadata) != 0) {
        Say("copy: could not look at that file; it may have been erased");
        return 1;
    }
    std::string   SourceName = Metadata.Filename ? Metadata.Filename : "a file with no name";
    std::uint64_t Size       = Metadata.Size;
    CarriedTags   Carried    = TagsToCarry(Metadata.Tags, SourceName);
    FileMetadataRelease(&Metadata);

    std::string Refusal = NameRefusal(NewName, SourceName);
    if (!Refusal.empty()) {
        Say("copy: " + Refusal);
        return 1;
    }

    NameSettled Settled = SettleTakenName(NewName);
    if (!Settled.Refusal.empty()) {
        Say("copy: " + Settled.Refusal);
        return 1;
    }
    file_id_t Old = Settled.Replaced;

    /* The buffer is had before anything is changed: no memory is found out
     * then, with nothing yet to take back. */
    std::vector<std::uint8_t> Buffer(Size > ChunkBytes ? ChunkBytes : static_cast<std::size_t>(Size));

    if (Old != 0) {
        int Trashed = tag_add(Old, "trashed");
        if (Trashed != 0) {
            Say(std::format("copy: nothing was copied: the old {} could not be put in the trash: {}",
                            NewName, Why(Trashed)));
            return 1;
        }
    }

    std::string Name(NewName);
    std::int64_t Made = create(Name.c_str(), Carried.Joined.c_str());
    if (Made < 0) {
        Say(std::format("copy: {} was not made: {}.{}", NewName, Why(Made), TakeBack(Old, NewName)));
        return 1;
    }
    file_id_t Copy = static_cast<file_id_t>(Made);

    std::string Failure = FillCopy(Source, Copy, Size, Buffer);
    if (!Failure.empty()) {
        Abandon(Copy, NewName, Failure, Old);
        return 1;
    }

    souvenir_things(&Copy, 1);
    ReportCopied(SourceName, NewName, Size, Carried, Old != 0);
    return 0;
}

}

int main()
{
    try {
        const file_id_t   *Files;
        std::size_t        FileCount;
        const char *const *Words;
        std::size_t        WordCount;
        if (form_things(0, &Files, &FileCount) != 0 || form_words(1, &Words, &WordCount) != 0 ||
            FileCount == 0 || WordCount == 0)
            return 1;
        return CopyFile(Files[0], Words[0]);
    } catch (const std::bad_alloc &) {
        Say("copy: there is no memory for this copy");
        return 1;
    }
}
