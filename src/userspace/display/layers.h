#ifndef BOXOS_DISPLAY_LAYERS_H
#define BOXOS_DISPLAY_LAYERS_H

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "box/core/result.h"
#include "console_model.h"
#include "easel.h"

// Layers — the pictures programs keep on the screen over the console's text
// (box/display.h). Composed by role, then by age; a layer's memory is made by
// the display for the asker's program alone and named to it.

// A layer taken to be folded into the console's text: where it lies, its
// cells, the base rows scrolled away first, the caret it leaves, and the text
// the fold says into the log ring — what the layer itself shows, never the
// text beneath it, which the ring holds already.
struct LayerCommit {
    std::int32_t X = 0;
    std::int32_t Y = 0;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::vector<DisplayLayerCell> Cells;
    std::uint32_t ScrollRows = 0;
    std::uint32_t CursorX = 0;
    std::uint32_t CursorY = 0;
    bool CursorVisible = false;
    std::string Text;
};

// DISP_CMD_LAYER_OPEN, asked of this display (Asked) by its sender, whom From
// names: answered here. A new layer shows nothing until its first frame.
void LayersOpen(const Result &Asked, const AskFrom &From, const DisplayLayerOpenRequest &Request,
                const ConsoleModel &Model) noexcept;

// DISP_CMD_LAYER_FRAME and _CLOSE from `Requester`; they answer nothing. True
// when the picture changed.
bool LayersFrame(std::uint32_t Requester, std::uint32_t Generation, std::uint32_t LayerId) noexcept;
bool LayersClose(std::uint32_t Requester, std::uint32_t Generation, std::uint32_t LayerId) noexcept;

// DISP_CMD_LAYER_COMMIT by (Pid, Generation): the layer's newest frame taken,
// checked against the console, and the layer handed over and gone — or why
// not, and the layer stays.
std::expected<LayerCommit, error_t> LayersCommit(std::uint32_t Pid, std::uint32_t Generation,
                                                 const DisplayLayerCommitRequest &Request,
                                                 const ConsoleModel &Model) noexcept;

// Everything (Pid, Generation) held, gone with it.
bool LayersDropProcess(std::uint32_t Pid, std::uint32_t Generation) noexcept;

// Take every layer's newest published frame.
bool LayersStep() noexcept;

// The model's text with every layer laid over it, each cell with its mark, and
// the caret. A caret is part of the picture it stands in — the console's, or a
// layer's that shows one — and a layer above it covers it as it covers the
// cell it stands on.
void LayersCompose(const ConsoleModel &Model, std::vector<GlassCell> &Composed,
                   Caret &At) noexcept;

#endif
