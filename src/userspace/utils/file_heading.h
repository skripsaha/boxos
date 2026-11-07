#ifndef UTILS_FILE_HEADING_H
#define UTILS_FILE_HEADING_H

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "box/file.h"

/* What hexer and stringview share: the line over a file they look into, and
 * how wide its offsets are written. */

/* Its name, its size and its tags, the tags written as `files` lists them. */
inline std::string FileHeading(const FileMetadata &Metadata)
{
    std::string_view Tags = Metadata.Tags ? Metadata.Tags : "";
    std::string      Spaced;
    for (std::size_t I = 0; I < Tags.size(); I++) {
        Spaced += Tags[I];
        if (Tags[I] == ',' && I + 1 < Tags.size() && Tags[I + 1] != ' ') Spaced += ' ';
    }
    return std::format("{}  {} byte{}  [{}]\n", Metadata.Filename ? Metadata.Filename : "a file with no name",
                       Metadata.Size, Metadata.Size == 1 ? "" : "s", Spaced);
}

/* Eight hex digits, or as many as the last offset of `Size` bytes takes. */
inline unsigned OffsetDigits(std::uint64_t Size)
{
    unsigned Digits = 8;
    while (Digits < 16 && Size > 0 && ((Size - 1) >> (4 * Digits)) != 0) Digits++;
    return Digits;
}

#endif
