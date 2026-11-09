/* A length the way a person reads one. */

#include "look.h"

#include <format>

namespace kin {

std::string SizeText(std::uint64_t Bytes)
{
    if (Bytes < 1024) return std::format("{} B", Bytes);
    if (Bytes < 1024 * 1024) return std::format("{} KB", (Bytes + 1023) / 1024);
    return std::format("{}.{} MB", Bytes / (1024 * 1024), Bytes % (1024 * 1024) * 10 / (1024 * 1024));
}

}  // namespace kin
