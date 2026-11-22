/* notes.cpp — a note and its file (notes.h). */

#include "notes.h"

#include <charconv>
#include <cstring>
#include <format>
#include <string_view>

#include "box/cxx/whose.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/time.h"

#include "../trash_term.h"

namespace scratchpad {

namespace {

constexpr const char *NoteTag = "scratchpad";

// The longest name a note is given from its first line, in bytes.
constexpr std::size_t LongestName = 24;

std::string Refused(std::string_view What, std::int64_t Code)
{
    return std::format("{} (error {})", What, static_cast<int>(box_errno_of(static_cast<int>(Code))));
}

std::string PaperTag(Color Shade, const char *Key)
{
    return std::format("{}:{:06x}", Key, Shade.Raw());
}

std::string SheetTags(const Sheet &Colors)
{
    return PaperTag(Colors.Paper, "paper") + "," + PaperTag(Colors.Ink, "ink");
}

// The color a tag of the file holds: "paper:ffe680" is ffe680 for "paper".
std::optional<Color> ColorOf(std::string_view Tags, std::string_view Key)
{
    std::size_t At = 0;
    while (At < Tags.size()) {
        std::size_t End = Tags.find(',', At);
        if (End == std::string_view::npos) End = Tags.size();
        const std::string_view Tag = Tags.substr(At, End - At);
        At = End + 1;
        if (Tag.size() != Key.size() + 7 || Tag.substr(0, Key.size()) != Key || Tag[Key.size()] != ':') continue;
        std::uint32_t Raw = 0;
        const char   *From = Tag.data() + Key.size() + 1;
        const auto    Done = std::from_chars(From, Tag.data() + Tag.size(), Raw, 16);
        if (Done.ec == std::errc{} && Done.ptr == Tag.data() + Tag.size()) return Color::FromRaw(Raw);
    }
    return std::nullopt;
}

// The words its first line begins with, as many whole ones as fit, joined by
// hyphens; a first word too long is cut. Without any word, the time it was written.
std::string NameFrom(const NoteText &Text)
{
    const std::string Bytes = Text.Bytes();
    const std::string_view First = std::string_view(Bytes).substr(0, Bytes.find('\n'));
    const auto Letter = [](char Byte) {
        return (Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z') || (Byte >= '0' && Byte <= '9');
    };
    std::string Name;
    for (std::size_t At = 0; At < First.size();) {
        if (!Letter(First[At])) {
            At++;
            continue;
        }
        std::size_t End = At;
        while (End < First.size() && Letter(First[End])) End++;
        const std::string_view Word = First.substr(At, End - At);
        At = End;
        if (Name.empty()) {
            Name = Word.substr(0, LongestName);
        } else if (Name.size() + 1 + Word.size() <= LongestName) {
            Name += '-';
            Name += Word;
        } else {
            break;
        }
    }
    if (!Name.empty()) return Name;
    BoxTime Now{};
    if (clock_boxtime_local(&Now) != 0) return "note";
    return std::format("note-{:02}{:02}-{:02}{:02}{:02}", Now.month, Now.day, Now.hour, Now.minute, Now.second);
}

// The name, or the name with the first number after it that no file in use has.
std::string Unused(const std::string &Name)
{
    std::string Try = Name;
    for (int Number = 2; find_file_by_name(box::tagfs::my_tags, Try.c_str(), nullptr, nullptr, nullptr, 0) > 0; Number++)
        Try = std::format("{}-{}", Name, Number);
    return Try;
}

}  // namespace

std::expected<Note, std::string> Note::Read(file_id_t File, const Look &Shown)
{
    FileMetadata Info;
    if (const int Code = FileMetadataRead(box::tagfs::my_tags, File, &Info); Code != 0)
        return std::unexpected(Refused("a file could not be looked at", Code));
    Note        Made;
    Made.Held   = File;
    Made.Called = Info.Filename ? Info.Filename : "";
    Made.Worn   = FileMetadataWears(&Info, NoteTag);
    const std::string_view Tags = Info.Tags ? Info.Tags : "";
    const std::optional<Color> Paper = ColorOf(Tags, "paper");
    const std::optional<Color> Ink   = ColorOf(Tags, "ink");
    const std::uint64_t Size = Info.Size;
    FileMetadataRelease(&Info);
    if (Size > MostBytes)
        return std::unexpected(std::format("{} is {} bytes: more than a note holds ({})", Made.Called, Size, MostBytes));

    std::string Bytes(static_cast<std::size_t>(Size), '\0');
    std::size_t Have = 0;
    while (Have < Bytes.size()) {
        const std::int64_t Got = fread(File, Have, Bytes.data() + Have, Bytes.size() - Have);
        if (Got < 0) return std::unexpected(Refused(std::format("{} would not be read", Made.Called), Got));
        if (Got == 0) break;
        Have += static_cast<std::size_t>(Got);
    }
    Bytes.resize(Have);
    Made.Text = NoteText::FromBytes(Bytes);

    Made.Stored = Paper && Ink;
    if (Made.Stored) {
        Made.Colors = Sheet{*Paper, *Ink};
    } else {
        Made.Colors  = NewSheet(Shown);
        Made.Follows = !Shown.Random;
    }
    return Made;
}

Note Note::Blank(std::string Name, const Look &Shown)
{
    Note Made;
    Made.Called  = std::move(Name);
    Made.Colors  = NewSheet(Shown);
    Made.Follows = !Shown.Random;
    return Made;
}

void Note::Repaint(Sheet Now) noexcept
{
    Colors  = Now;
    Changed = true;
    Follows = false;
}

void Note::Follow(const Look &Shown) noexcept
{
    if (Follows) Colors = Shown.Fixed;
}

bool Note::Unsaved() const noexcept
{
    return Text.Changed() || Changed || (Held == 0 && !Text.Blank());
}

std::optional<std::string> Note::Write()
{
    const std::string Bytes = Text.Bytes();
    if (const int Code = file_span(Held, Bytes.size(), STORAGE_SPAN_GROW); Code != 0)
        return Refused(std::format("{} would not grow", Called), Code);
    std::size_t Done = 0;
    while (Done < Bytes.size()) {
        const std::int64_t Wrote = fwrite(Held, Done, Bytes.data() + Done, Bytes.size() - Done);
        if (Wrote <= 0) return Refused(std::format("{} would not take what was written", Called), Wrote);
        Done += static_cast<std::size_t>(Wrote);
    }
    if (const int Code = file_span(Held, Bytes.size(), 0); Code != 0)
        return Refused(std::format("{} would not be cut to its text", Called), Code);
    return std::nullopt;
}

std::optional<std::string> Note::Retag()
{
    if (!Worn) {
        if (const int Code = tag_add(box::tagfs::my_tags, Held, NoteTag); Code != 0) return Refused(std::format("{} could not be tagged", Called), Code);
        Worn = true;
    }
    if (Stored) {
        (void)tag_remove(box::tagfs::my_tags, Held, "paper");
        (void)tag_remove(box::tagfs::my_tags, Held, "ink");
    }
    for (const std::string &Tag : {PaperTag(Colors.Paper, "paper"), PaperTag(Colors.Ink, "ink")})
        if (const int Code = tag_add(box::tagfs::my_tags, Held, Tag.c_str()); Code != 0)
            return Refused(std::format("{} could not be given its colors", Called), Code);
    Stored  = true;
    Changed = false;
    return std::nullopt;
}

std::optional<std::string> Note::Save()
{
    if (!Unsaved()) return std::nullopt;
    const bool Made = Held == 0;
    if (Made) {
        if (Called.empty()) Called = NameFrom(Text);
        Called = Unused(Called);
        const std::string Tags = std::string(NoteTag) + "," + SheetTags(Colors);
        const std::int64_t Id = create(box::tagfs::my_tags, Called.c_str(), Tags.c_str());
        if (Id <= 0) return Refused(std::format("the volume would not make {}", Called), Id);
        Held   = static_cast<file_id_t>(Id);
        Stored = Worn = true;
    } else if (!Stored || Changed || !Worn) {
        if (const std::optional<std::string> Trouble = Retag()) return Trouble;
    }
    if (Made || Text.Changed())
        if (const std::optional<std::string> Trouble = Write()) return Trouble;
    if (const int Code = anchor(Held); Code != 0)
        return Refused(std::format("{} was written but not settled on the medium", Called), Code);
    Text.Kept();
    Changed = false;
    Follows = false;
    return std::nullopt;
}

std::optional<std::string> Note::Trash()
{
    if (Held == 0) return std::nullopt;
    if (const int Code = file_keep(Held, TrashTermSeconds()); Code != 0)
        return Refused(std::format("{} could not be moved to the trash", Called), Code);
    return std::nullopt;
}

std::expected<std::vector<file_id_t>, std::string> AllNotes()
{
    file_id_t *Ids   = nullptr;
    const int  Count = query_all_everywhere(box::tagfs::my_tags, NoteTag, nullptr, &Ids);
    if (Count < 0) return std::unexpected(Refused("the volume would not list the notes", Count));
    std::vector<file_id_t> Found(Ids, Ids + Count);
    free(Ids);
    return Found;
}

}  // namespace scratchpad
