#ifndef BENCHMARK_H
#define BENCHMARK_H

/* benchmark — what its parts share (benchmark.cpp). */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace painter::gui {
class Area;
}
namespace painter::tui {
class Keyboard;
}

namespace benchmark {

/* What a measuring finds, line by line: kept as it is said, printed when the
 * screen is the console's (Print) — never while a scene is measured, so the
 * console's lines cost the display nothing then — and written to a file on
 * the volume (Save). */
class Report {
public:
    void Say(const char *Format, ...) __attribute__((format(printf, 2, 3)));
    // The lines said since the last Print, printed.
    void Print();
    // Every line written to the volume as Name; false, said why, when it
    // could not be.
    bool Save(const char *Name);

private:
    std::vector<std::string> Lines;
    std::size_t              Printed = 0;
};

/* `benchmark graphics` (graphics.cpp): the engine (Engine), the scenes the
 * display shows (Scenes) and their summary, said and saved — 0 when every
 * scene was measured. */
int Graphics();

/* What each of the engine's ways costs a pixel on this processor, on a tile
 * the size the display composes in, and what setting and drawing a page of
 * text costs (engine.cpp). */
void Engine(Report &Out);

/* How the scenes went (scenes.cpp). */
struct Tally {
    int           Smooth = 0, Uneven = 0, Slideshow = 0;
    int           Count   = 0;       // scenes there are
    bool          Stopped = false;   // the person stopped it, or the area went
    std::uint32_t Refused = 0;       // changes the display refused, in every scene
};

/* Every scene shown on On, the screen whole, each measured and its line said
 * to Out; Escape stops it. */
Tally Scenes(painter::gui::Area &On, painter::tui::Keyboard &Keys, Report &Out);

}  // namespace benchmark

#endif  // BENCHMARK_H
