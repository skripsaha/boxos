/* pixels.h — the lamp in the screen's own pixels: a canvas as large as the
 * screen (painter::gui), the wax's field worked out on a grid a few pixels
 * apart and every pixel coloured from it, the panel's items over it. Only
 * the cells of the grid the wax is in, or was in either of the two frames
 * before — the page drawn on now last held the picture two frames old — are
 * drawn again, and the display is told only what they cover; the next frame
 * is drawn while the display shows the last. */
#ifndef LAVALAMP_PIXELS_H
#define LAVALAMP_PIXELS_H

#include <cstdint>
#include <optional>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/core/canvas.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/keyboard.h"

#include "composition.h"
#include "field.h"
#include "lamp.h"
#include "panel.h"
#include "shades.h"
#include "tuning.h"

namespace lavalamp {

class PixelLamp {
public:
    PixelLamp(gui::Area Screen, painter::tui::Keyboard Keys, const Composition &Start);

    /* Until the person leaves; the lamp as it was then. */
    Composition Run();

private:
    /* The canvas, the grid and the tables for the screen as it is now. */
    void Build();
    /* The composition changed: the wax and its colours with it, all of the
     * picture drawn again. */
    void Retune();
    void Frame();
    void Paint(painter::core::Canvas &Page, painter::core::Rect Changed);
    void PaintCell(painter::core::Canvas &Page, int Col, int Row);
    /* Which cells to draw, and their bounds in cells; none when nothing changed. */
    std::optional<painter::core::Rect> Mark();
    void ColourPoints(painter::core::Rect Cells);
    bool OnEvent(const gui::Event &Told);

    gui::Area              Screen;
    painter::tui::Keyboard Keys;
    box::settings::watch   Watch;
    Composition            Current;
    Lamp                   Wax;
    Field                  Grid;
    Shades                 Colours;
    Tuning                 Tuned;
    gui::Group             Under;            // the canvas, beneath the panel
    gui::Canvas            Board;
    std::optional<Panel>   Pane;
    int                    Wide = 0, High = 0, Step = 4, Cols = 0, CellRows = 0;
    float                  SlopeBy = 1;      // a level's fall a pixel, to its slope
    std::vector<Rgb>       LiquidRow, Points;   // the liquid's colour a row; the wax's at each point
    std::vector<float>     Edges;               // how much the level changes across a pixel, at each point
    std::vector<float>     BulbRow, RoundCol, BulbCol;
    std::vector<std::uint8_t> Lit, WasLit, LongLit;   // the cells the wax reached now, a frame and two frames before
    int                    Whole  = 2;       // frames still to draw every pixel of: one for each page
    bool                   Hidden = false;
    std::uint64_t          LastUs = 0;
    std::uint64_t          Seen   = 0;       // the last step the glass showed
};

}  // namespace lavalamp

#endif
