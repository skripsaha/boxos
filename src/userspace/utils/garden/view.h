/* view.h — the garden as the person sees it: what the gardener kept, ranked
 * as they set, the plant they have chosen and the ones they marked, and its
 * keys. The picture is paint.cpp's; the ranking is growth's, the sky's and the
 * plants' own are sky's and flora's. */
#ifndef GARDEN_VIEW_H
#define GARDEN_VIEW_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "box/cxx/settings.h"
#include "box/touch.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "growth.h"
#include "look.h"
#include "record.h"
#include "scene.h"
#include "sky.h"

namespace garden {

class View {
public:
    // The garden on Screen, the plant whose name holds Focus chosen.
    View(painter::tui::Stage Screen, const std::string &Focus);
    ~View();
    View(const View &)            = delete;
    View &operator=(const View &) = delete;

    // Until the person leaves, or the garden is asked to finish.
    void Run();
    // The names of the plants the person marked: what it gives back.
    const std::vector<std::string> &Given() const noexcept { return Marked; }

private:
    // A plant as it is drawn: it grows toward the height its starts make.
    struct Grower {
        std::string   Name;
        double        Height = 0;
        std::int32_t  Pulse = 0;       // frames left of the sparkle for a new start
        std::uint32_t Starts = 0;
    };
    // Where the garden's parts are on the glass.
    struct Layout {
        Scene        Sky;
        std::int32_t LabelRow = 0;     // the first of the three rows of a plant's words
        std::int32_t Slots = 1;        // plants side by side
        std::int32_t SlotWidth = 9;
        std::int32_t Room = 0;         // the tallest a plant can stand, in pixels
    };

    void           Read();
    void           Rank();
    void           Advance();
    void           Scroll(std::size_t Visible);
    bool           OnKey(const painter::tui::KeyEvent &Key);
    void           Move(std::ptrdiff_t By);
    void           Mark();
    Layout         LayoutOf() const;
    Grower        &GrowerOf(const Plant &One);
    const Plant   *Chosen() const noexcept;
    std::string    Spelled(GardenKey Key) const;

    // paint.cpp: the picture.
    void Paint();
    void PaintGarden(painter::tui::Frame &Out, const Layout &Where, const Light &Lit);
    void PaintWords(painter::tui::Frame &Out, const Layout &Where, std::size_t Index, std::int32_t Left,
                    const Palette &Colors) const;
    void PaintNothing(painter::tui::Frame &Out) const;
    void PaintList(painter::tui::Frame &Out, painter::core::Rect Area);
    void PaintTop(painter::tui::Frame &Out, const Light &Lit) const;
    void PaintBottom(painter::tui::Frame &Out) const;
    void PaintHelp(painter::tui::Frame &Out) const;
    bool IsMarked(const Plant &One) const;

    painter::tui::Stage        Screen;
    box::settings::watch       Watch;
    Look                       Shown;
    TouchTag                   Grew = TOUCH_TAG_INVALID;
    Record                     Kept;
    std::vector<const Plant *> Ranking;
    std::vector<Grower>        Growers;
    std::vector<std::string>   Marked;
    std::string                Status;
    std::string                Wanted;            // the chosen plant's name, kept through a new ranking
    std::size_t                Choice = 0;
    std::size_t                First = 0;         // the first plant shown, in the garden or the list
    double                     Time = 0;          // the wind's, in seconds at the person's speed
    std::uint64_t              LastUs = 0;
    bool                       Settled = false;   // the first reading is done: a plant new after it sparkles
    bool                       Keeper = false;    // a gardener is aboard
    bool                       List = false;
    bool                       Help = false;
};

}  // namespace garden

#endif
