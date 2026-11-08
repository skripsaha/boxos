/* The volume as edit and read ask it: reading, making, writing, renaming,
 * retagging, and the tags files wear. */

#include "volume.h"

#include <algorithm>
#include <format>
#include <new>

#include "box/cxx/error.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/use.h"

namespace kin {

namespace {

std::string Why(std::int64_t Answer)
{
    return std::string(box::error(static_cast<::error_t>(-Answer)).message());
}

std::string_view KeyOf(std::string_view Tag)
{
    return Tag.substr(0, Tag.find(':'));
}

bool Holds(const std::vector<std::string> &List, std::string_view Tag)
{
    return std::find(List.begin(), List.end(), Tag) != List.end();
}

}  // namespace

std::expected<FileFacts, std::string> ReadFacts(file_id_t File)
{
    FileMetadata Metadata;
    const int    Answer = FileMetadataRead(File, &Metadata);
    if (Answer != 0) return std::unexpected(std::format("the volume would not say what it is: {}", Why(Answer)));

    FileFacts Facts;
    Facts.Name    = Metadata.Filename ? Metadata.Filename : "";
    Facts.Tags    = Metadata.Tags ? Metadata.Tags : "";
    Facts.Size    = Metadata.Size;
    Facts.Trashed = (Metadata.Flags & FILE_FLAG_TRASHED) != 0;
    FileMetadataRelease(&Metadata);
    return Facts;
}

bool FileGone(file_id_t File)
{
    FileMetadata Metadata;
    const int    Answer = FileMetadataRead(File, &Metadata);
    if (Answer == 0) FileMetadataRelease(&Metadata);
    return box_errno_of(Answer) == ERR_FILE_NOT_FOUND;
}

/* A short read is the volume answering with what it had in hand: the next
 * read asks for the rest. */
std::expected<std::string, std::string> ReadBytes(file_id_t File, std::uint64_t Size)
{
    std::string Bytes;
    try {
        Bytes.resize(static_cast<std::size_t>(Size));
    } catch (const std::bad_alloc &) {
        return std::unexpected(std::format("there is no memory for its {} bytes", Size));
    }

    std::uint64_t Got = 0;
    while (Got < Size) {
        const std::int64_t Read = fread(File, Got, Bytes.data() + Got, static_cast<std::size_t>(Size - Got));
        if (Read < 0) return std::unexpected(std::format("it would not be read past byte {}: {}", Got, Why(Read)));
        if (Read == 0) break;
        Got += static_cast<std::uint64_t>(Read);
    }
    Bytes.resize(static_cast<std::size_t>(Got));
    return Bytes;
}

std::expected<file_id_t, std::string> MakeFile(std::string_view Name, Scope Looking)
{
    const std::string  Text(Name);
    const std::int64_t Made = Looking == Scope::InContext ? create(Text.c_str(), "")
                                                          : create_everywhere(Text.c_str(), "");
    if (Made <= 0) return std::unexpected(std::format("the volume would not make {}: {}", Name, Why(Made)));
    return static_cast<file_id_t>(Made);
}

std::optional<std::string> EraseFile(file_id_t File)
{
    if (const int Answer = file_delete(File); Answer != 0)
        return std::format("the volume would not erase it: {}", Why(Answer));
    return std::nullopt;
}

std::optional<std::string> WriteBytes(file_id_t File, std::string_view Bytes)
{
    /* A short count is the bytes the file holds of the write; the next write,
     * from there, meets whatever stopped it. */
    std::uint64_t Done = 0;
    while (Done < Bytes.size()) {
        const std::int64_t Wrote = fwrite(File, Done, Bytes.data() + Done, Bytes.size() - Done);
        if (Wrote < 0) return std::format("the volume stopped taking bytes after {}: {}", Done, Why(Wrote));
        if (Wrote == 0) return std::format("the volume stopped taking bytes after {}", Done);
        Done += static_cast<std::uint64_t>(Wrote);
    }

    /* Exactly as long as the text: a shorter text's old tail is cut. */
    if (const int Cut = file_span(File, Bytes.size(), 0); Cut != 0)
        return std::format("it was written, but its old end could not be cut: {}", Why(Cut));
    if (const int Settled = anchor(File); Settled != 0)
        return std::format("it was written, but the volume did not say it is safe on the medium: {}", Why(Settled));
    return std::nullopt;
}

std::optional<std::string> Rename(file_id_t File, std::string_view Name)
{
    const std::string Text(Name);
    if (const int Answer = file_rename(File, Text.c_str()); Answer != 0)
        return std::format("the volume would not call it {}: {}", Name, Why(Answer));
    return std::nullopt;
}

/* What comes off goes first, so a key whose value changes is taken off and put
 * on again rather than worn twice. */
std::optional<std::string> Retag(file_id_t File, const std::vector<std::string> &Had,
                                 const std::vector<std::string> &Wanted)
{
    for (const std::string &Tag : Had) {
        if (Holds(Wanted, Tag)) continue;
        const std::string Key(KeyOf(Tag));
        if (const int Answer = tag_remove(File, Key.c_str()); Answer != 0)
            return std::format("{} would not come off: {}", Tag, Why(Answer));
    }
    for (const std::string &Tag : Wanted) {
        if (Holds(Had, Tag)) continue;
        if (const int Answer = tag_add(File, Tag.c_str()); Answer != 0)
            return std::format("{} would not go on: {}", Tag, Why(Answer));
    }
    return std::nullopt;
}

std::expected<std::vector<file_id_t>, std::string> FilesSeen(Scope Looking)
{
    file_id_t *Ids   = nullptr;
    const int  Count = Looking == Scope::InContext ? query_all(nullptr, &Ids) : query_all_everywhere(nullptr, &Ids);
    if (Count < 0) return std::unexpected(std::format("the volume would not say what files it has: {}", Why(Count)));
    std::vector<file_id_t> Seen(Ids, Ids + Count);
    free(Ids);
    return Seen;
}

std::vector<box::tagfs::tag_count> TagsWorn(Scope Looking)
{
    std::vector<box::tagfs::tag_count> Worn =
        Looking == Scope::InContext ? box::tagfs::tags() : box::tagfs::tags_everywhere();
    std::stable_sort(Worn.begin(), Worn.end(), [](const box::tagfs::tag_count &A, const box::tagfs::tag_count &B) {
        return A.files != B.files ? A.files > B.files : Spelled(A.what) < Spelled(B.what);
    });
    return Worn;
}

std::string Spelled(const box::tagfs::tag &One)
{
    return One.value.empty() ? One.key : One.key + ":" + One.value;
}

std::string ContextTags()
{
    /* Asked with no room, the answer is how many tags it wrote — none — and
     * Needed says how much room the context takes: one byte is its NUL alone. */
    std::size_t Needed = 0;
    if (use_get(nullptr, 0, &Needed) < 0 || Needed <= 1) return {};
    std::string Tags(Needed, '\0');
    if (use_get(Tags.data(), Tags.size(), &Needed) < 0) return {};
    Tags.resize(Tags.find('\0'));
    return Tags;
}

std::string ScopeNote(Scope Looking)
{
    const std::string Context = Looking == Scope::InContext ? ContextTags() : std::string();
    return Context.empty() ? std::string("the whole volume") : "in " + Joined(TagWords(Context), ", ");
}

std::vector<std::string> TagWords(std::string_view Text)
{
    std::vector<std::string> Words;
    std::size_t              At = 0;
    while (At < Text.size()) {
        while (At < Text.size() && (Text[At] == ' ' || Text[At] == ',')) At++;
        const std::size_t Start = At;
        while (At < Text.size() && Text[At] != ' ' && Text[At] != ',') At++;
        if (At > Start && !Holds(Words, Text.substr(Start, At - Start)))
            Words.emplace_back(Text.substr(Start, At - Start));
    }
    return Words;
}

std::string Joined(const std::vector<std::string> &Words, std::string_view Between)
{
    std::string Out;
    for (const std::string &Word : Words) {
        if (!Out.empty()) Out += Between;
        Out += Word;
    }
    return Out;
}

}  // namespace kin
