/* board.h — the map: what it read of the machine, the cells made of it, where
 * the person's cursor is, and the keys. The picture is paint.cpp's; the
 * reading is machine's, owners' and mosaic's. */
#ifndef MEMMAP_BOARD_H
#define MEMMAP_BOARD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "look.h"
#include "machine.h"
#include "mosaic.h"
#include "owners.h"

namespace memmap {

class Board {
public:
    // The map on Screen, the owners whose name or tags hold Filter stood out.
    Board(painter::tui::Stage Screen, std::string Filter);
    // Until the person leaves, or the map is asked to finish.
    void Run();
    // The files of the programs the person marked: what it gives back.
    std::vector<file_id_t> Given() const;

private:
    // Where each part of the picture stands, for the glass as it is.
    struct Plan {
        painter::core::Rect Map;          // the map's panel
        painter::core::Rect Legend;
        painter::core::Rect Info;
        painter::core::Rect Inside;       // what the cells are drawn in
        bool                Pixels = false;   // cells are pixels, not characters
        std::int32_t        Side = 1;         // a cell's side with its gap, in pixels
        std::int32_t        Cols = 0;
        std::int32_t        Rows = 0;
    };
    struct Mark {
        std::string Key;
        file_id_t   File = 0;
    };

    void            Read();
    void            Remake();
    void            Rebuild();
    void            Lay();
    Plan            Planned() const;
    bool            OnKey(const painter::tui::KeyEvent &Key);
    void            MoveCursor(std::ptrdiff_t By);
    void            ToggleMark();
    const Owner    *Chosen() const;
    std::string     Spelled(MapKey Key) const;

    // paint.cpp: the picture.
    using InfoLine = std::pair<std::string, Color>;
    std::vector<InfoLine> InfoLines() const;
    void Paint();
    void PaintHeader(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintStrip(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintMap(painter::tui::Frame &Out) const;
    void PaintCells(painter::tui::Frame &Out) const;
    void PaintLegend(painter::tui::Frame &Out) const;
    void PaintInfo(painter::tui::Frame &Out) const;
    void PaintStatus(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintHelp(painter::tui::Frame &Out) const;

    painter::tui::Stage    Screen;
    box::settings::watch   Watch;
    Look                   Shown;
    Machine                Source;
    Ledger                 Kept;
    std::optional<Reading> Now;
    std::vector<Segment>   Parts;            // Now's regions, laid over one another
    std::vector<Owner>     Fine;             // every program and kind of memory, as read
    std::vector<Owner>     Owners;           // those the view shows, grouped and ordered
    Mosaic                 Laid;
    Plan                   Grid;
    std::size_t            LaidFor = 0;      // the cells Laid was made to fit
    bool                   Stale = true;     // Laid is of owners that have since changed
    std::uint64_t          NextReadUs = 0;
    std::uint64_t          Seen = 0;         // what the latest reading would be drawn as, by its fingerprint
    std::string            Filter;
    std::string            Status;           // the map's latest word
    std::size_t            Cursor = 0;       // the cell the person is on
    std::vector<Mark>      Marked;
    bool                   Paused = false;
    bool                   Help = false;
    bool                   Dirty = true;     // the picture is older than what it shows
    bool                   Ticks = false;    // the picture says how long a program has lived: it moves every reading
};

}  // namespace memmap

#endif
