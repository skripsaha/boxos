#ifndef EDIT_VOLUME_H
#define EDIT_VOLUME_H

/* The volume side: what a file is called and wears, its bytes in and out, its
 * name and tags changed. Every refusal is a sentence for the person, and
 * every word of it is the volume's own: edit keeps no copy of the rules a
 * file's tags or name must follow — the kernel says no, and edit says why.
 */

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/file.h"

namespace edit {

struct FileFacts {
    std::string   Name;
    std::string   Tags;      /* comma-joined, as the volume says them */
    std::uint64_t Size    = 0;
    bool          Trashed = false;
};

std::expected<FileFacts, std::string>   ReadFacts(file_id_t File);
std::expected<std::string, std::string> ReadBytes(file_id_t File, std::uint64_t Size);

/* A new file, inside the Use Context: create stamps the context's tags too. */
std::expected<file_id_t, std::string> MakeFile(std::string_view Name, std::string_view Tags);

/* The file's bytes become exactly Bytes: written, its old tail cut, and the
 * volume asked to settle it on the medium. */
std::optional<std::string> WriteBytes(file_id_t File, std::string_view Bytes);

std::optional<std::string> Rename(file_id_t File, std::string_view Name);
/* The file's tags become Wanted: what is gone comes off, what is new goes on. */
std::optional<std::string> Retag(file_id_t File, std::string_view Name, std::string_view Had,
                                 const std::vector<std::string> &Wanted);

/* The Use Context's tags, comma-joined; "" when the person has said none. */
std::string ContextTags();

/* A tag list as the person types one: blanks or commas between tags. */
std::vector<std::string> TagWords(std::string_view Text);
/* A file's tags as the person is told them: all but the one every file wears
 * for its name, which comes and goes with the name. */
std::vector<std::string> ShownTags(std::string_view Tags, std::string_view Name);
std::string              Joined(const std::vector<std::string> &Words, std::string_view Between);

}  // namespace edit

#endif
