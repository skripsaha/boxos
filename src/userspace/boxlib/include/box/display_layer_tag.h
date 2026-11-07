#ifndef BOXLIB_DISPLAY_LAYER_TAG_H
#define BOXLIB_DISPLAY_LAYER_TAG_H

#include <cstdint>
#include <expected>
#include <string>

#include "box/cxx/error.h"
#include "box/display.h"

namespace box::display {

/*
 * Layer Bays are addressed by a canonical name derived from the process
 * incarnation and a caller-owned nonce. The name is an implementation detail
 * of the Bay primitive; it is deliberately not copied through the IPC wire.
 * Both sides use this one helper so the protocol has no arbitrary tag buffer
 * or duplicated spelling algorithm.
 */
inline std::expected<std::string, box::error> LayerBayTag(std::uint32_t Pid,
                                                          std::uint32_t Generation,
                                                          std::uint64_t Nonce) noexcept
{
    try {
        std::string Tag = DISPLAY_LAYER_TAG_PREFIX;
        Tag += std::to_string(Pid);
        Tag += ':';
        Tag += std::to_string(Generation);
        Tag += ':';
        Tag += std::to_string(Nonce);
        return Tag;
    } catch (...) {
        return std::unexpected(box::error{box::errc::no_memory});
    }
}

}  // namespace box::display

#endif  // BOXLIB_DISPLAY_LAYER_TAG_H
