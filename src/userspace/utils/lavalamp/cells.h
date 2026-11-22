/* cells.h — the lamp in the console's cells (painter::tui), when the settings
 * say tui on or the screen is a text page: the same wax, the same colours,
 * each cell cut into the parts its glass draws exactly — two across, and
 * four, three or two down — and settled on the glyph and the two colours
 * that show them best (FineCanvas::PutMosaic); the panel a window of the
 * console's look over it. A cell of the liquid alone stays as it was drawn:
 * only those the wax reaches, or reached a frame before, are drawn again. */
#ifndef LAVALAMP_CELLS_H
#define LAVALAMP_CELLS_H

#include <cstdint>
#include <vector>

#include "painter/tui/stage.h"

#include "composition.h"
#include "field.h"
#include "lamp.h"
#include "setup.h"
#include "shades.h"

namespace lavalamp {

class CellLamp {
public:
    /* Forced: drawn in cells for the screen shows only text, whatever tui
     * says. */
    CellLamp(painter::tui::Stage Screen, Setup &With, bool Forced);

    /* Until the person leaves — false — or turns tui off where the screen
     * has pixels: true. The screen is given back before it returns; Closed
     * false when it could not be. */
    bool Run(bool &Closed);

private:
    void Retune();
    void Paint();
    void PaintPanel();
    void PaintKeys();
    /* What a key or the settings did; WasOpen, whether the panel was open
     * before. */
    void Take(Heard What, bool WasOpen);

    painter::tui::Stage  Screen;
    Setup               &Set;
    bool                 Forced;
    Lamp                 Wax;
    Field                Grid;
    Shades               Colours;
    int                  Cols = 0, Rows = 0, Parts = 2;
    float                SlopeBy = 1;
    std::vector<Rgb>     Behind;           // the liquid and the glass at each part of a cell
    std::vector<std::uint8_t> Lit, WasLit; // the cells the wax reaches now and a frame before
    bool                 Whole = true;     // every cell drawn in the next frame
    painter::core::Rect  Window{};         // the panel's window and its shadow, which darkens what it lies on
    bool                 KeysShown = false;
    std::uint64_t        KeysUntilUs = 0;  // the line of keys shown until then
    std::uint64_t        NextUs      = 0;  // the next frame drawn then
    bool                 Going       = true;
    bool                 ToPixels    = false;
};

}  // namespace lavalamp

#endif
