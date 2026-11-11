/* orbits — a gravity sandbox: bodies circle a star and leave coloured trails
 * that fade; the person throws more, and watches them settle into orbits,
 * fall into the star, merge or fly away.
 *
 *   orbits               the sky the settings name — a calm solar system at first
 *   orbits binary        begun in another: solar, binary, eight, chaos or lone
 *
 *   left right, up down  turn the aim, throw harder or softer; shift for fine steps
 *   w a s d              move the launcher
 *   enter, space         throw a body, on the dotted path the aim shows
 *   [ ]                  a lighter body, a heavier one
 *   p                    pause          + -   time faster, slower
 *   1 2 3 4 5            another sky    page up, page down   nearer, farther
 *   esc                  leave
 *
 * settings orbits sets how long a trail lasts, the gravity, the speed of time,
 * the colours and the keys that clear the trails, begin the sky again, hide the
 * aim and show every key. What it is made of: sky (the bodies, gravity and
 * touches, which know nothing of the glass), presets (the five skies), look
 * (the settings), paint (the picture), panel (the words) and show (time and
 * keys). */

#include <cstddef>
#include <format>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "presets.h"
#include "show.h"

BOX_FORM("takes optional name \"the sky it begins with: solar, binary, eight, chaos or lone\"\n");

namespace {

constexpr std::int32_t LeastCols = 40;
constexpr std::int32_t LeastRows = 12;

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;
        std::optional<std::size_t> First;
        if (Count > 0) {
            const std::size_t Named = orbits::PresetNamed(Words[0]);
            if (Named == orbits::PresetCount) {
                Say(std::format("orbits: there is no sky called {} - solar, binary, eight, chaos or lone", Words[0]));
                return 1;
            }
            First = Named;
        }

        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("orbits"));
            return 1;
        }
        orbits::Show Sky(std::move(*Opened), First);
        const bool Painted = Sky.Run();
        Sky.Close();
        Say(Painted ? std::format("[ORBITS] {} bodies left, {} merged, {} gone", Sky.Count(), Sky.Merged(), Sky.Escaped())
                    : "The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a sky the size of this screen.");
        return 1;
    }
}
