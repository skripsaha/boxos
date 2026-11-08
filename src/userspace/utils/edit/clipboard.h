#ifndef EDIT_CLIPBOARD_H
#define EDIT_CLIPBOARD_H

/* The clipboard side of edit: the machine's list of what was copied, said in
 * edit's words. The list is every program's, and its clips are bytes; what
 * they mean to edit is edit's to keep (clips.h). Every refusal comes back as
 * a sentence for the status line.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/cxx/clipboard.h"

namespace edit {

/* A clip's bytes, with the number they were taken under. */
struct Taken {
    std::uint64_t Number = 0;
    std::string   Bytes;
};

/* The bytes as the newest clip; its number. */
std::expected<std::uint64_t, std::string> PutClip(std::string_view Bytes);
/* The newest clip, whole — nothing when the list is empty. A clip with takes
 * left spends one. */
std::expected<std::optional<Taken>, std::string> TakeNewest();
/* Every clip, newest first. */
std::expected<std::vector<box::clip>, std::string> ListClips();
/* The clip, whole; one with takes left spends one. */
std::expected<std::string, std::string> TakeWhole(const box::clip &Which);
/* No more than Most of the clip's first bytes; nothing when the clip would
 * not be looked at — gone since it was listed. Never asked of a clip with
 * takes left: that one is taken only whole, and a look would spend it. */
std::optional<std::string>              TakeHead(const box::clip &Which, std::size_t Most);
std::optional<std::string>              DropClip(std::uint64_t Number);

/* The first line of a clip, every control character a blank: what a field,
 * which holds one line, makes of it. */
std::string FirstLine(std::string_view Bytes);

}  // namespace edit

#endif
