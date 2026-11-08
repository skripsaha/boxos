#ifndef KIN_VOLUME_H
#define KIN_VOLUME_H

/* The volume as edit and read ask it: what a file is called and wears, its
 * bytes in and out, its name and tags changed, the tags files wear. Every
 * refusal is a sentence for the person, and every word of it is the volume's
 * own: neither program keeps a copy of the rules a file's tags or name follow
 * — the volume says no, the program says why, and what a file wears is shown
 * as the volume says it.
 */

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/cxx/tagfs.h"
#include "box/file.h"

namespace kin {

/* How a program looks at the volume: through the Use Context, as every ask of the
 * volume is by default, or at all of it. */
enum class Scope { InContext, Everywhere };

struct FileFacts {
    std::string   Name;
    std::string   Tags;      /* comma-joined, as the volume says them */
    std::uint64_t Size    = 0;
    bool          Trashed = false;
};

std::expected<FileFacts, std::string>   ReadFacts(file_id_t File);
/* True only when the volume says it has no file of that id: a file it would
 * not speak of for another reason is not called gone. */
bool                                    FileGone(file_id_t File);
std::expected<std::string, std::string> ReadBytes(file_id_t File, std::uint64_t Size);

/* A new, empty file: inside the Use Context the volume stamps the context's
 * tags on it, as it stamps the tag of its name. */
std::expected<file_id_t, std::string> MakeFile(std::string_view Name, Scope Looking);
std::optional<std::string>            EraseFile(file_id_t File);

/* The file's bytes become exactly Bytes: written, its old tail cut, and the
 * volume asked to settle it on the medium. */
std::optional<std::string> WriteBytes(file_id_t File, std::string_view Bytes);

std::optional<std::string> Rename(file_id_t File, std::string_view Name);
/* What came off between Had and Wanted comes off, what is new goes on. */
std::optional<std::string> Retag(file_id_t File, const std::vector<std::string> &Had,
                                 const std::vector<std::string> &Wanted);

/* The ids of the files the program can see. */
std::expected<std::vector<file_id_t>, std::string> FilesSeen(Scope Looking);
/* The tags files wear, the most worn first. */
std::vector<box::tagfs::tag_count> TagsWorn(Scope Looking);
/* A tag as the volume spells it: "key" or "key:value". */
std::string Spelled(const box::tagfs::tag &One);

/* The Use Context's tags, comma-joined; "" when the person has said none. */
std::string ContextTags();
/* Where the program is looking, for a window's title: "in code, cpp", or "the whole
 * volume" — which is also what the Use Context is when it holds nothing. */
std::string ScopeNote(Scope Looking);

/* A tag list as the person types one: blanks or commas between tags. */
std::vector<std::string> TagWords(std::string_view Text);
std::string              Joined(const std::vector<std::string> &Words, std::string_view Between);

}  // namespace kin

#endif
