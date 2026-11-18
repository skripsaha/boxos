/* board.h — the board: what it read of the machine, what the person chose on
 * it, and its keys. The picture is picture.cpp's on a framebuffer — items,
 * described a frame at a time — and paint.cpp's on a text page — cells; the
 * reading is machine's, programs', history's and journal's. */
#ifndef TASKBOARD_BOARD_H
#define TASKBOARD_BOARD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/gui/area.h"
#include "painter/gui/frame.h"
#include "painter/tui/frame.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/stage.h"

#include "history.h"
#include "journal.h"
#include "look.h"
#include "machine.h"
#include "programs.h"

namespace taskboard {

// A program by its main strand, in one life.
struct Who {
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;

    bool operator==(const Who &) const = default;
};

class Board {
public:
    // The board on a text page's cells, or on a framebuffer's area with the
    // keyboard — the list kept to what Filter holds.
    Board(painter::tui::Stage Screen, std::string Filter);
    Board(painter::gui::Area Screen, painter::tui::Keyboard Keys, std::string Filter);
    // Until the person leaves, or the board is asked to finish.
    void Run();
    // The files of the programs the person marked: what it gives back.
    std::vector<file_id_t> Given() const;

private:
    // One line of the list: a group's heading, a program, or one of its strands.
    struct Line {
        enum class Kind : std::uint8_t { Group, Program, Strand };
        Kind           What = Kind::Program;
        const Program *Of = nullptr;
        Living         Group = Living::Task;
        std::size_t    Strand = 0;
    };
    struct Mark {
        Who       Program;
        file_id_t File = 0;
    };

    void              Read();
    bool              OnKey(const painter::tui::KeyEvent &Key);
    void              Move(std::ptrdiff_t By);
    void              ToggleOpen();
    void              ToggleMark();
    void              EndChosen();
    std::vector<Line> Lines() const;
    const Program    *Chosen() const;
    std::string       Spelled(BoardKey Key) const;

    // paint.cpp: the picture.
    void Paint();
    void PaintHeader(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintLoad(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintCores(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintMachine(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintPrograms(painter::tui::Frame &Out, painter::core::Rect Area);
    void PaintChosen(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintJournal(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintStatus(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintHelp(painter::tui::Frame &Out) const;
    // picture.cpp: the picture on a framebuffer.
    void Describe(painter::gui::Frame &Out);

    std::optional<painter::tui::Stage>    CellScreen;   // a text page's
    std::optional<painter::gui::Area>     ItemScreen;   // a framebuffer's
    std::optional<painter::tui::Keyboard> Keys;         //   and its keys
    box::settings::watch   Watch;
    Look                   Shown;
    Programs               Known;
    History                Load;
    Journal                Said;
    std::optional<Reading> Now;
    std::uint64_t          NextReadUs = 0;
    std::string            Filter;
    std::string            Status;          // the board's latest word
    std::optional<Who>     Choice;
    std::size_t            ChoiceAt = 0;    // its place among the programs listed: where a choice that ended goes on
    std::vector<Who>       Open;            // programs whose strands are shown
    std::vector<Mark>      Marked;
    std::vector<Who>       Asked;           // asked to finish: the end key again ends them
    std::size_t            Top = 0;         // the first line of the list shown
    bool                   Help = false;
};

}  // namespace taskboard

#endif
