/* history.h — the lines typed at the prompt, each with the Use Context it was
 * typed in, newest last.
 *
 * They live on the volume, in the file `history` (tags hub and history), so
 * they outlast the hub and the night and travel with the medium, and every hub
 * reads the same file: a line typed at one is there at another's Up at once.
 * With no volume to keep them they live as long as the hub. An answer to a
 * question and an Itinerary's step are not typed at the prompt and are not
 * here. The history keeps lines and says nothing: when the volume would not
 * keep one, it hands back why, and the one who keeps the line says so. */
#ifndef HUB_HISTORY_H
#define HUB_HISTORY_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/touch.h"
#include "box/types.h"

namespace hub {

class History {
public:
    struct Line {
        std::string Text;
        std::string Context;   // comma-joined tags, "" for none
    };

    // Why the volume did not keep a line: its refusal (Rc, a negative
    // -error_t), or Rc 0 when it kept only part of the line.
    struct Unkept {
        int Rc = 0;
    };

    // The file found and read whole, and the volume asked to say when it grows.
    void Begin();

    // A line typed in a context: added to the file's end when ToFile — and
    // so to what every hub holds — or held by this hub alone. The same line
    // in the same context as the newest is kept once. A line the volume
    // would not keep is held here all the same; the first such line of this
    // hub's life hands back why, every later one nothing.
    std::optional<Unkept> Keep(std::string_view Text, std::string_view Context, bool ToFile);

    // What other hubs added since it was last read, read now. Cheap when
    // nothing was added: the volume says when it was.
    void Catch();

    std::size_t Count() const noexcept { return Lines.size(); }
    // The line Back lines before the newest (0: the newest).
    const Line &Before(std::size_t Back) const noexcept { return Lines[Lines.size() - 1 - Back]; }

    // How Up walks: every line, the newest first — with HereFirst, the lines
    // typed in Context first and then the rest. As indices into Before.
    std::vector<std::size_t> Walk(std::string_view Context, bool HereFirst) const;

private:
    std::expected<file_id_t, int> Reach();
    void                          Remember(std::string_view Text, std::string_view Context);
    void                          ReadOn();
    std::optional<Unkept>         Write(const std::string &Record);

    std::vector<Line> Lines;
    file_id_t         File = 0;         // 0 until found or made
    file_id_t         KnownOf = 0;      // the file Known counts the bytes of
    std::uint64_t     Known = 0;        // its bytes read so far, whole lines
    TouchTag          Grown = TOUCH_TAG_INVALID;
    bool              Told = false;     // a line the volume would not keep was handed back
};

}  // namespace hub

#endif
