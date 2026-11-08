/* What read remembers from one reading to the next. */

#include "marks.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "box/cxx/error.h"
#include "box/memory.h"

namespace read {

namespace {

constexpr std::string_view FileName = "read.marks";
/* The tag a file is found by its name with: the name before its last dot. */
constexpr std::string_view NameTag  = "read";

std::string Why(std::int64_t Answer)
{
    return std::string(box::error(box_errno_of(static_cast<int>(Answer))).message());
}

/* read.marks, wherever on the volume it is; 0 when there is none. */
file_id_t Find()
{
    file_id_t *Ids   = nullptr;
    const int  Count = query_all_everywhere(std::string(NameTag).c_str(), &Ids);
    file_id_t  Found = 0;
    for (int Index = 0; Index < Count && Found == 0; Index++) {
        FileMetadata Facts;
        if (FileMetadataRead(Ids[Index], &Facts) != 0) continue;
        if (Facts.Filename && FileName == Facts.Filename && !(Facts.Flags & FILE_FLAG_TRASHED)) Found = Ids[Index];
        FileMetadataRelease(&Facts);
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

}  // namespace

std::optional<std::string> Memory::Load()
{
    Kept = Find();
    if (Kept == 0) return std::nullopt;

    FileMetadata Facts;
    if (const int Answer = FileMetadataRead(Kept, &Facts); Answer != 0)
        return std::format("{} would not say what it holds: {}", FileName, Why(Answer));
    const std::uint64_t Size = Facts.Size;
    FileMetadataRelease(&Facts);

    std::string   Text(static_cast<std::size_t>(Size), '\0');
    std::uint64_t Got = 0;
    while (Got < Size) {
        const std::int64_t Read = fread(Kept, Got, Text.data() + Got, static_cast<std::size_t>(Size - Got));
        if (Read < 0) return std::format("{} would not be read: {}", FileName, Why(Read));
        if (Read == 0) break;
        Got += static_cast<std::uint64_t>(Read);
    }
    Text.resize(static_cast<std::size_t>(Got));
    Read(Text);
    return std::nullopt;
}

/* A line that says nothing this reader knows is passed over: a later read
 * may know more, and an older one keeps what it understands. */
void Memory::Read(std::string_view Text)
{
    std::size_t At = 0;
    while (At < Text.size()) {
        const std::size_t      End  = std::min(Text.find('\n', At), Text.size());
        const std::string_view Line = Text.substr(At, End - At);
        At                          = End + 1;

        std::vector<std::string_view> Words;
        for (std::size_t From = 0; From < Line.size();) {
            const std::size_t Blank = std::min(Line.find(' ', From), Line.size());
            if (Blank > From) Words.push_back(Line.substr(From, Blank - From));
            From = Blank + 1;
        }
        if (Words.size() == 2 && Words[0] == "theme") {
            Theme = std::string(Words[1]);
        } else if (Words.size() == 2 && Words[0] == "width") {
            (void)NumberOf(Words[1], Cols);
        } else if (Words.size() == 3 && (Words[0] == "place" || Words[0] == "mark")) {
            Fact One;
            if (!NumberOf(Words[1], One.File) || !NumberOf(Words[2], One.Source)) continue;
            (Words[0] == "place" ? Places : Marked).push_back(One);
        }
    }
}

std::optional<std::string> Memory::Save()
{
    if (!Changed) return std::nullopt;

    std::string Text;
    if (!Theme.empty()) Text += std::format("theme {}\n", Theme);
    if (Cols != 0) Text += std::format("width {}\n", Cols);
    for (const Fact &One : Places) Text += std::format("place {} {}\n", One.File, One.Source);
    for (const Fact &One : Marked) Text += std::format("mark {} {}\n", One.File, One.Source);

    if (Kept == 0) {
        const std::int64_t Made = create_everywhere(std::string(FileName).c_str(), "");
        if (Made <= 0) return std::format("{} could not be made: {}", FileName, Why(Made));
        Kept = static_cast<file_id_t>(Made);
    }
    std::uint64_t Done = 0;
    while (Done < Text.size()) {
        const std::int64_t Wrote = fwrite(Kept, Done, Text.data() + Done, Text.size() - Done);
        if (Wrote <= 0)
            return std::format("{} stopped taking bytes after {}{}", FileName, Done,
                               Wrote < 0 ? ": " + Why(Wrote) : std::string());
        Done += static_cast<std::uint64_t>(Wrote);
    }
    if (const int Cut = file_span(Kept, Text.size(), 0); Cut != 0)
        return std::format("{} was written, but its old end could not be cut: {}", FileName, Why(Cut));
    if (const int Settled = anchor(Kept); Settled != 0)
        return std::format("{} was written, but is not yet safe on the medium: {}", FileName, Why(Settled));
    Changed = false;
    return std::nullopt;
}

std::optional<std::size_t> Memory::Place(file_id_t File) const
{
    for (const Fact &One : Places)
        if (One.File == File) return One.Source;
    return std::nullopt;
}

void Memory::SetPlace(file_id_t File, std::size_t Source)
{
    for (Fact &One : Places)
        if (One.File == File) {
            if (One.Source != Source) Changed = true;
            One.Source = Source;
            return;
        }
    Places.push_back({File, Source});
    Changed = true;
}

std::vector<std::size_t> Memory::Marks(file_id_t File) const
{
    std::vector<std::size_t> Found;
    for (const Fact &One : Marked)
        if (One.File == File) Found.push_back(One.Source);
    std::sort(Found.begin(), Found.end());
    return Found;
}

bool Memory::Toggle(file_id_t File, std::size_t Source)
{
    const Fact One{File, Source};
    Changed = true;
    if (const auto At = std::find(Marked.begin(), Marked.end(), One); At != Marked.end()) {
        Marked.erase(At);
        return false;
    }
    Marked.push_back(One);
    return true;
}

void Memory::Unmark(file_id_t File, std::size_t Source)
{
    const auto Before = Marked.size();
    std::erase(Marked, Fact{File, Source});
    if (Marked.size() != Before) Changed = true;
}

void Memory::SetTheme(std::string_view Name)
{
    if (Theme == Name) return;
    Theme   = std::string(Name);
    Changed = true;
}

void Memory::SetWidth(std::int32_t Width)
{
    if (Cols == Width) return;
    Cols    = Width;
    Changed = true;
}

}  // namespace read
