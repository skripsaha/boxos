/* context.h — the Use Context, as the kernel holds it.
 *
 * The context is the machine's, one for everyone: the hub reads it before
 * every line rather than keeping a copy of what it last said, so whoever said
 * it last — this hub, another, a system program, a volume that came up with
 * its memory — is what the person sees. Its tags are a list as tags.h reads
 * one. */
#ifndef HUB_CONTEXT_H
#define HUB_CONTEXT_H

#include <optional>
#include <string>

namespace hub::context {

// The context's tags, comma-joined; "" for none. Nothing when the kernel refused.
std::optional<std::string> Whole();

}  // namespace hub::context

#endif
