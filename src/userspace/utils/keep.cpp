/* keep.cpp — a program's own small file on the volume (keep.h). */

#include <cstring>
#include <format>

#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/use.h"

#include "keep.h"

namespace {

std::string Refused(std::string_view What, std::int64_t Code)
{
    return std::format("{} (error {})", What, static_cast<int>(box_errno_of(static_cast<int>(Code))));
}

/* 1 with Id filled, 0 when there is none, a negative error otherwise: the one
 * called by its name among the files wearing its tags, not in the trash. */
int Find(const Keep &File, file_id_t &Id)
{
    file_id_t *Ids   = nullptr;
    int        Count = query_all_everywhere(File.Tags, nullptr, &Ids);
    int        Found = Count < 0 ? Count : 0;
    for (int At = 0; At < Count && Found == 0; At++) {
        FileMetadata Metadata;
        if (FileMetadataRead(Ids[At], &Metadata) != 0) continue;
        if (Metadata.Filename && std::strcmp(Metadata.Filename, File.Name) == 0) {
            Id    = Ids[At];
            Found = 1;
        }
        FileMetadataRelease(&Metadata);
    }
    free(Ids);
    return Found;
}

}

std::optional<std::string> KeepRead(const Keep &File, std::string &Text)
{
    Text.clear();
    file_id_t Id    = 0;
    int       Found = Find(File, Id);
    if (Found < 0) return Refused(std::format("the volume would not list its files, so {} was not read", File.Name), Found);
    if (Found == 0) return std::nullopt;

    FileMetadata Metadata;
    int          Code = FileMetadataRead(Id, &Metadata);
    if (Code != 0) return Refused(std::format("{} could not be looked at", File.Name), Code);
    std::uint64_t Size = Metadata.Size;
    FileMetadataRelease(&Metadata);

    Text.resize(static_cast<std::size_t>(Size));
    std::size_t Have = 0;
    while (Have < Text.size()) {
        std::int64_t Got = fread(Id, Have, Text.data() + Have, Text.size() - Have);
        if (Got < 0) {
            Text.clear();
            return Refused(std::format("{} would not be read", File.Name), Got);
        }
        if (Got == 0) break;
        Have += static_cast<std::size_t>(Got);
    }
    Text.resize(Have);
    return std::nullopt;
}

std::optional<std::string> KeepWrite(const Keep &File, std::string_view Text)
{
    file_id_t Id    = 0;
    int       Found = Find(File, Id);
    if (Found < 0) return Refused(std::format("the volume would not list its files, so {} was not kept", File.Name), Found);
    if (Found == 0) {
        std::int64_t Made = create_everywhere(File.Name, File.Tags);
        if (Made <= 0) return Refused(std::format("the volume would not make {}", File.Name), Made);
        Id = static_cast<file_id_t>(Made);
    }

    /* Grown first when the text is longer — a write lands no further than the
     * file's end — and cut to the text after. */
    int Code = file_span(Id, Text.size(), STORAGE_SPAN_GROW);
    if (Code != 0) return Refused(std::format("{} would not grow", File.Name), Code);
    std::size_t Done = 0;
    while (Done < Text.size()) {
        std::int64_t Wrote = fwrite(Id, Done, Text.data() + Done, Text.size() - Done);
        if (Wrote <= 0) return Refused(std::format("{} would not take what was written", File.Name), Wrote);
        Done += static_cast<std::size_t>(Wrote);
    }
    Code = file_span(Id, Text.size(), 0);
    if (Code != 0) return Refused(std::format("{} would not be cut to its text", File.Name), Code);

    Code = anchor(Id);
    if (Code != 0) return Refused(std::format("{} was written but not settled on the medium", File.Name), Code);
    return std::nullopt;
}

std::string KeepContext()
{
    std::size_t Needed = 0;
    if (use_get(nullptr, 0, &Needed) < 0 || Needed <= 1) return {};
    std::string Tags(Needed, '\0');
    if (use_get(Tags.data(), Tags.size(), &Needed) < 0) return {};
    Tags.resize(Tags.find('\0'));
    return Tags;
}
