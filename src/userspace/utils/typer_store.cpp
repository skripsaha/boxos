/* typer_store.cpp — the record file on the volume: typer.record, wearing
 * typer's own words typer,record, one line to a run. A program's own words are
 * asked of the whole volume, so the Use Context, whatever the person has said
 * to use, neither hides it nor stamps it. */

#include <format>

#include "box/cxx/whose.h"
#include "box/file.h"
#include "box/memory.h"

#include "typer_screen.h"

namespace {

constexpr char          RecordName[] = "typer.record";
constexpr char          RecordTags[] = "typer,record";
constexpr std::uint64_t TailMost     = 262144;

/* 1 with Id filled, 0 when there is no record yet, a negative error otherwise. */
int FindRecord(file_id_t &Id)
{
    file_id_t *Ids   = nullptr;
    int        Count = query_all_everywhere(box::tagfs::my_tags, RecordTags, nullptr, &Ids);
    if (Count > 0) Id = Ids[0];
    free(Ids);
    return Count > 0 ? 1 : Count;
}

std::string Refused(std::string_view What, std::int64_t Code)
{
    return std::format("{} (error {})", What, -Code);
}

std::optional<std::string> SizeOf(file_id_t Id, std::uint64_t &Size)
{
    FileMetadata Metadata;
    int          Code = FileMetadataRead(box::tagfs::my_tags, Id, &Metadata);
    if (Code != 0) return Refused("the record file could not be looked at", Code);
    Size = Metadata.Size;
    FileMetadataRelease(&Metadata);
    return std::nullopt;
}

}

std::optional<std::string> ReadRecordText(std::string &Text)
{
    Text.clear();
    file_id_t Id = 0;
    int       Found = FindRecord(Id);
    if (Found < 0) return Refused("the volume would not list its files, so the record was not read", Found);
    if (Found == 0) return std::nullopt;

    std::uint64_t Size = 0;
    if (std::optional<std::string> Trouble = SizeOf(Id, Size)) return Trouble;

    std::uint64_t Start = Size > TailMost ? Size - TailMost : 0;
    Text.resize(static_cast<std::size_t>(Size - Start));
    std::size_t Have = 0;
    while (Have < Text.size()) {
        std::int64_t Got = fread(Id, Start + Have, Text.data() + Have, Text.size() - Have);
        if (Got < 0) {
            Text.clear();
            return Refused("the record file would not be read", Got);
        }
        if (Got == 0) break;
        Have += static_cast<std::size_t>(Got);
    }
    Text.resize(Have);

    if (Start != 0) {
        std::size_t Break = Text.find('\n');
        Text.erase(0, Break == std::string::npos ? Text.size() : Break + 1);
    }
    return std::nullopt;
}

std::optional<std::string> AppendRecordLine(std::string_view Line)
{
    file_id_t Id = 0;
    int       Found = FindRecord(Id);
    if (Found < 0) return Refused("the volume would not list its files, so the run was not recorded", Found);
    if (Found == 0) {
        std::int64_t Made = create_everywhere(box::tagfs::my_tags, RecordName, RecordTags);
        if (Made <= 0) return Refused("the volume would not make the record file", Made);
        Id = static_cast<file_id_t>(Made);
    }

    std::uint64_t Size = 0;
    if (std::optional<std::string> Trouble = SizeOf(Id, Size)) return Trouble;

    int Code = file_span(Id, Size + Line.size(), 0);
    if (Code != 0) return Refused("the record file would not grow", Code);

    std::int64_t Wrote = fwrite(Id, Size, Line.data(), Line.size());
    if (Wrote < 0) return Refused("the record file would not take the run", Wrote);
    if (static_cast<std::size_t>(Wrote) != Line.size()) return std::string("the record file took only part of the run");

    Code = anchor(Id);
    if (Code != 0) return Refused("the run was written but not settled on the medium", Code);
    return std::nullopt;
}
