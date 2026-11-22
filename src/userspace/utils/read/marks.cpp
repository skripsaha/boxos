/* The bookmarks read keeps from one reading to the next. */

#include "marks.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "box/cxx/whose.h"
#include "box/memory.h"

#include "kin/look.h"
#include "kin/volume.h"

namespace read {

namespace {

constexpr std::string_view FileName = "read.marks";
/* The words a bookmark keeps: enough to find its paragraph again among the
 * others, few enough that read.marks stays a file a person can read. */
constexpr std::size_t      WordsBytes = 40;

/* read.marks, wherever on the volume it is; 0 when there is none. */
file_id_t Find()
{
    file_id_t *Ids   = nullptr;
    const int  Count = query_name_all_everywhere(box::tagfs::my_tags, std::string(FileName).c_str(), nullptr, nullptr, &Ids);
    file_id_t  Found = 0;
    for (int Index = 0; Index < Count && Found == 0; Index++) {
        std::expected<kin::FileFacts, std::string> Facts = kin::ReadFacts(Ids[Index]);
        if (Facts && Facts->Name == FileName && !Facts->Trashed) Found = Ids[Index];
    }
    free(Ids);
    return Found;
}

template <class Number>
bool NumberOf(std::string_view Word, Number &Out)
{
    const auto Read = std::from_chars(Word.data(), Word.data() + Word.size(), Out);
    return Read.ec == std::errc{} && Read.ptr == Word.data() + Word.size();
}

/* The words at Source, to the end of their line: whole characters only. */
std::string WordsAt(std::string_view Bytes, std::size_t Source)
{
    if (Source >= Bytes.size()) return {};
    const std::string_view Line = Bytes.substr(Source, Bytes.find_first_of("\r\n", Source) - Source);
    std::size_t            End  = 0;
    while (End < Line.size()) {
        const std::size_t Next = End + painter::tui::GlyphAt(Line, End).Length;
        if (Next > WordsBytes) break;
        End = Next;
    }
    return std::string(Line.substr(0, End));
}

/* Where Words stand in Bytes nearest Near; nothing when they stand nowhere. */
std::optional<std::size_t> Nearest(std::string_view Bytes, std::string_view Words, std::size_t Near)
{
    std::optional<std::size_t> Best;
    for (std::size_t At = Bytes.find(Words); At != std::string_view::npos; At = Bytes.find(Words, At + 1)) {
        const std::size_t Far = At > Near ? At - Near : Near - At;
        if (!Best || Far < (*Best > Near ? *Best - Near : Near - *Best)) Best = At;
    }
    return Best;
}

}  // namespace

std::optional<std::string> Memory::Load()
{
    Kept = Find();
    if (Kept == 0) return std::nullopt;

    std::expected<kin::FileFacts, std::string> Facts = kin::ReadFacts(Kept);
    if (!Facts) return std::format("{}: {}", FileName, Facts.error());
    std::expected<std::string, std::string> Text = kin::ReadBytes(Kept, Facts->Size);
    if (!Text) return std::format("{}: {}", FileName, Text.error());
    Read(*Text);
    LetGoOfTheGone();
    return std::nullopt;
}

/* A line that says nothing this reader knows is passed over: a later read
 * may know more, and an older one keeps what it understands. */
void Memory::Read(std::string_view Text)
{
    std::size_t At = 0;
    while (At < Text.size()) {
        const std::size_t End = std::min(Text.find('\n', At), Text.size());
        std::string_view  Line = Text.substr(At, End - At);
        At                     = End + 1;

        std::string_view Fields[3];
        bool             Whole = true;
        for (std::string_view &Field : Fields) {
            const std::size_t Blank = std::min(Line.find(' '), Line.size());
            Field                   = Line.substr(0, Blank);
            Line.remove_prefix(std::min(Blank + 1, Line.size()));
            Whole = Whole && !Field.empty();
        }
        if (!Whole || (Fields[0] != "place" && Fields[0] != "mark")) continue;

        Fact One;
        One.Left = Fields[0] == "place";
        if (!NumberOf(Fields[1], One.File) || !NumberOf(Fields[2], One.Source)) continue;
        One.Words = std::string(Line);
        Facts.push_back(std::move(One));
    }
}

/* The volume is asked once a file, whatever the number of its bookmarks. */
void Memory::LetGoOfTheGone()
{
    std::vector<file_id_t> Asked, Gone;
    for (const Fact &One : Facts) {
        if (std::find(Asked.begin(), Asked.end(), One.File) != Asked.end()) continue;
        Asked.push_back(One.File);
        if (kin::FileGone(One.File)) Gone.push_back(One.File);
    }
    if (Gone.empty()) return;
    std::erase_if(Facts, [&](const Fact &One) { return std::find(Gone.begin(), Gone.end(), One.File) != Gone.end(); });
    Changed = true;
}

std::optional<std::string> Memory::Save()
{
    if (!Changed) return std::nullopt;

    std::string Text;
    for (const Fact &One : Facts)
        Text += std::format("{} {} {} {}\n", One.Left ? "place" : "mark", One.File, One.Source, One.Words);

    if (Kept == 0) {
        std::expected<file_id_t, std::string> Made = kin::MakeFile(FileName, kin::Scope::Everywhere);
        if (!Made) return Made.error();
        Kept = *Made;
    }
    if (std::optional<std::string> Refused = kin::WriteBytes(Kept, Text))
        return std::format("{}: {}", FileName, *Refused);
    Changed = false;
    return std::nullopt;
}

/* A bookmark kept before it kept its words learns them from where it stands. */
void Memory::Settle(file_id_t File, std::string_view Bytes)
{
    for (Fact &One : Facts) {
        if (One.File != File) continue;
        One.Lost = false;
        if (One.Words.empty()) {
            One.Words = WordsAt(Bytes, One.Source);
            One.Lost  = One.Source > Bytes.size();
            Changed   = Changed || !One.Words.empty();
            continue;
        }
        if (Bytes.substr(std::min(One.Source, Bytes.size())).starts_with(One.Words)) continue;
        if (const std::optional<std::size_t> Moved = Nearest(Bytes, One.Words, One.Source)) {
            One.Source = *Moved;
            Changed    = true;
        } else {
            One.Lost = true;
        }
    }
    /* Two bookmarks whose words went to one place are one bookmark. */
    for (auto Each = Facts.begin(); Each != Facts.end(); ++Each)
        Facts.erase(std::remove_if(std::next(Each), Facts.end(),
                                   [&](const Fact &Other) {
                                       return Other.File == Each->File && Other.Left == Each->Left &&
                                              Other.Source == Each->Source;
                                   }),
                    Facts.end());
}

std::optional<Mark> Memory::Place(file_id_t File) const
{
    for (const Fact &One : Facts)
        if (One.Left && One.File == File) return Mark{One.Source, One.Lost};
    return std::nullopt;
}

void Memory::SetPlace(file_id_t File, std::size_t Source, std::string_view Bytes)
{
    for (Fact &One : Facts) {
        if (!One.Left || One.File != File) continue;
        if (One.Source == Source && !One.Lost) return;
        One.Source = Source;
        One.Words  = WordsAt(Bytes, Source);
        One.Lost   = false;
        Changed    = true;
        return;
    }
    Facts.push_back({true, File, Source, WordsAt(Bytes, Source), false});
    Changed = true;
}

std::vector<Mark> Memory::Marks(file_id_t File) const
{
    std::vector<Mark> Found;
    for (const Fact &One : Facts)
        if (!One.Left && One.File == File) Found.push_back({One.Source, One.Lost});
    std::sort(Found.begin(), Found.end(), [](const Mark &A, const Mark &B) { return A.Source < B.Source; });
    return Found;
}

void Memory::Put(file_id_t File, std::size_t Source, std::string_view Bytes)
{
    for (const Fact &One : Facts)
        if (!One.Left && One.File == File && One.Source == Source) return;
    Facts.push_back({false, File, Source, WordsAt(Bytes, Source), false});
    Changed = true;
}

void Memory::Unmark(file_id_t File, std::size_t Source)
{
    const std::size_t Before = Facts.size();
    std::erase_if(Facts, [&](const Fact &One) { return !One.Left && One.File == File && One.Source == Source; });
    if (Facts.size() != Before) Changed = true;
}

}  // namespace read
