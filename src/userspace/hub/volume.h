/* volume.h — the volume as the hub reads it: a file's record, and its bytes.
 * A record is of a file that lives: the kernel answers no other.
 *
 * What the volume hands over on the heap is the hub's to let go of; here it
 * is let go of by whatever holds it, so no caller frees by hand. */
#ifndef HUB_VOLUME_H
#define HUB_VOLUME_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"
#include "box/types.h"

namespace hub::volume {

// A file's record — its name, the person's tags, its size and its first bytes
// — read whole, as the person sees it.
class Record {
public:
    // The record of File; not read: the volume's refusal, a negative -error_t.
    static std::expected<Record, int> Of(file_id_t File);

    Record(Record &&Other) noexcept;
    Record &operator=(Record &&Other) noexcept;
    Record(const Record &)            = delete;
    Record &operator=(const Record &) = delete;
    ~Record();

    file_id_t        Id() const noexcept { return Held.FileId; }
    std::string_view Name() const noexcept { return Held.Filename ? Held.Filename : ""; }
    std::string_view Tags() const noexcept { return Held.Tags ? Held.Tags : ""; }
    std::uint64_t    Size() const noexcept { return Held.Size; }
    std::string_view Head() const noexcept
    {
        return {reinterpret_cast<const char *>(Held.Head), Held.HeadLength};
    }

private:
    Record() = default;

    FileMetadata Held{};
};

// Count bytes of File from At, read a piece at a time: fewer when the file
// ends sooner. Not read: the volume's refusal, a negative -error_t.
std::expected<std::string, int> Read(file_id_t File, std::uint64_t At, std::uint64_t Count);

// The whole of File, as long as its record says it is.
std::expected<std::string, int> ReadAll(file_id_t File);

// The ids a query handed over on the heap, Count of them (none when Count is
// a refusal), taken into a list and the heap let go of.
std::vector<file_id_t> Taken(file_id_t *Ids, int Count);

}  // namespace hub::volume

#endif
