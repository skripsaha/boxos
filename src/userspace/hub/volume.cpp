/* volume.cpp — records and bytes read off the volume. */

#include "volume.h"

#include <utility>

#include "box/memory.h"

namespace hub::volume {

namespace {

// What one read asks for at most: the volume answers a long file in pieces.
constexpr std::uint64_t Piece = 64 * 1024;

}  // namespace

std::expected<Record, int> Record::Of(file_id_t File)
{
    Record Found;
    const int Rc = FileMetadataRead(File, &Found.Held);
    if (Rc != 0) return std::unexpected(Rc < 0 ? Rc : -Rc);
    return Found;
}

Record::Record(Record &&Other) noexcept : Held(std::exchange(Other.Held, FileMetadata{})) {}

Record &Record::operator=(Record &&Other) noexcept
{
    if (this != &Other) {
        FileMetadataRelease(&Held);
        Held = std::exchange(Other.Held, FileMetadata{});
    }
    return *this;
}

Record::~Record()
{
    FileMetadataRelease(&Held);
}

std::expected<std::string, int> Read(file_id_t File, std::uint64_t At, std::uint64_t Count)
{
    std::string Bytes(static_cast<std::size_t>(Count), '\0');
    std::uint64_t Got = 0;
    while (Got < Count) {
        const std::uint64_t Want = Count - Got < Piece ? Count - Got : Piece;
        const int64_t Answer = fread(File, At + Got, Bytes.data() + Got, static_cast<std::size_t>(Want));
        if (Answer < 0) return std::unexpected(static_cast<int>(Answer));
        if (Answer == 0) break;
        Got += static_cast<std::uint64_t>(Answer);
    }
    Bytes.resize(static_cast<std::size_t>(Got));
    return Bytes;
}

std::expected<std::string, int> ReadAll(file_id_t File)
{
    std::expected<Record, int> Held = Record::Of(File);
    if (!Held) return std::unexpected(Held.error());
    return Read(File, 0, Held->Size());
}

std::vector<file_id_t> Taken(file_id_t *Ids, int Count)
{
    std::vector<file_id_t> Found;
    if (Ids && Count > 0) Found.assign(Ids, Ids + Count);
    free(Ids);
    return Found;
}

}  // namespace hub::volume
