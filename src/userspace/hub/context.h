/* context.h — the Use Context, as the kernel holds it.
 *
 * The context is the machine's, one for everyone: the hub reads it before
 * every line rather than keeping a copy of what it last said, so whoever said
 * it last — this hub, another, a system program, a volume that came up with
 * its memory — is what the person sees. */
#ifndef HUB_CONTEXT_H
#define HUB_CONTEXT_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hub::context {

// The context's tags, comma-joined; "" for none. Nothing when the kernel refused.
std::optional<std::string> Whole();

// How many tags a comma-joined list holds.
std::size_t Count(std::string_view Tags);

// Whether a comma-joined list holds this tag.
bool Holds(std::string_view Tags, std::string_view Tag);

// The same tags, in whatever order each list says them.
bool Same(std::string_view Left, std::string_view Right);

}  // namespace hub::context

#endif
