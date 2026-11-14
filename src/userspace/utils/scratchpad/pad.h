/* pad.h — the pad: the notes that are open, the one in front, and its keys.
 * The picture is paint.cpp's; the words are text's, the files notes'. */
#ifndef SCRATCHPAD_PAD_H
#define SCRATCHPAD_PAD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "look.h"
#include "notes.h"

namespace scratchpad {

class Pad {
public:
    // The pad on Screen with Opened notes, the first in front; at least one.
    Pad(painter::tui::Stage Screen, std::vector<Note> Opened);
    // Until the person leaves, or the pad is asked to finish.
    void Run();
    // The files of the notes that have one and were not deleted: what it gives back.
    std::vector<file_id_t> Given() const;
    // What was saved and what was left as it was, in a sentence; empty when
    // nothing was.
    std::string Summary() const;

private:
    // What a window of the pad is asking, in front of the note.
    enum class Asking : std::uint8_t { Nothing, Trash, Help };

    bool          OnKey(const painter::tui::KeyEvent &Key);
    bool          OnWriting(const painter::tui::KeyEvent &Key);
    void          Write(const painter::tui::KeyEvent &Key);
    bool          Leave();
    void          Turn(std::ptrdiff_t By);
    void          NewPaper();
    void          TrashHere();
    void          OpenAll();
    Note         &Here() { return Notes[At]; }
    const Note   &Here() const { return Notes[At]; }
    std::string   Spelled(PadKey Key) const;
    std::int32_t  TextWidth() const;

    // paint.cpp: the picture.
    void                Paint();
    painter::core::Rect Window() const;
    painter::core::Rect TextArea() const;
    void                PaintFooter(painter::tui::Frame &Out, painter::core::Rect Area, const Sheet &Colors) const;
    void                PaintHelp(painter::tui::Frame &Out) const;

    painter::tui::Stage  Screen;
    box::settings::watch Watch;
    Look                 Shown;
    std::vector<Note>    Notes;
    std::size_t          At = 0;
    Asking               Now = Asking::Nothing;
    std::string          Status;       // the pad's latest word
    bool                 Warned = false;
    std::vector<std::string> Saved;    // names of the notes saved on leaving
};

}  // namespace scratchpad

#endif
