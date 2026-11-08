#ifndef READ_READER_H
#define READ_READER_H

/* read as a whole: the tabs, the window in front of the page when one is
 * open, the keys, and leaving — kept the way edit keeps its own, so the two
 * are one family: the same tabs and bars, the same file chooser and card, the
 * same keys for the same things.
 *
 * The page is the book in the tab in front, laid in a column across the
 * middle of the glass. The tabs are above it; below it, what the book is and
 * how far through it the reading line is, which page, how long the rest
 * takes, and the heading it is under. One window at a time is in front, and
 * while it is the keys are its own: the file chooser, find, the contents, the
 * bookmarks, the help, the file's card. Every frame is the whole picture
 * drawn again from what is held here.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "box/file.h"
#include "painter/tui/stage.h"

#include "kin/bars.h"
#include "kin/card.h"
#include "kin/keys.h"
#include "kin/look.h"
#include "kin/picker.h"

#include "keymap.h"
#include "lists.h"
#include "marks.h"
#include "search.h"
#include "tab.h"

namespace read {

using namespace kin;

class Reader {
public:
    Reader(tui::Stage Screen, Memory Remembered);

    /* A file of the volume in a tab of its own, or the tab it is in already. */
    void Open(file_id_t File);
    void Run();

    /* Every file read had open, for whoever started it. */
    std::vector<file_id_t> Given() const { return Opened; }
    /* What is to be said once the glass is given back: what could not be
     * remembered. */
    std::string            Afterwords() const;

private:
    /* The column when nobody said: as wide as prose reads well. */
    static constexpr std::int32_t UsualWidth = 72;
    static constexpr std::int32_t LeastWidth = 24;
    static constexpr std::int32_t WidthStep  = 4;

    enum class Mode { Reading, Picking, Finding, Listing, Helping, Carding };
    enum class Window { Contents, Bookmarks };

    /* reader.cpp — tabs and files */
    Tab       &Active() noexcept { return *Tabs[Current]; }
    const Tab &Active() const noexcept { return *Tabs[Current]; }
    void       Show(std::size_t Index);
    void       Close(std::size_t Index);
    void       Leave(const Tab &One);
    void       OpenChooser();
    void       Rescope();
    void       Note(std::string Words, bool Trouble = false);

    /* reader.cpp — keys */
    void OnKey(const tui::KeyEvent &Key);
    void OnReading(const tui::KeyEvent &Key);
    void Do(const Heard &Asked);
    void OnPicking(const tui::KeyEvent &Key);
    void OnListing(const tui::KeyEvent &Key);
    void OnCarding(const tui::KeyEvent &Key);
    void OpenList(Window Which);
    void ToggleMark();
    void Copy();
    void Widen(std::int32_t By);

    /* page.cpp — the picture */
    bool         Paint();
    void         PaintRow(tui::Frame &Out, std::int32_t Y, std::size_t Row) const;
    void         PaintText(tui::Frame &Out, std::int32_t Y, std::size_t Row, core::Color Under) const;
    void         PaintStatus(tui::Frame &Out) const;
    std::int32_t Column() const noexcept;
    std::int32_t Left() const noexcept;
    std::size_t  PageRows() const noexcept;
    std::string  HeadingAbove() const;

    tui::Stage                        Screen;
    core::Size                        Bounds;
    Memory                            Remembered;
    std::int32_t                      Width = 0;     /* the column the person asked for; 0: the usual */
    std::vector<std::unique_ptr<Tab>> Tabs;
    std::size_t                       Current = 0;
    Mode                              Now     = Mode::Reading;
    Scope                             Looking = Scope::InContext;
    bool                              Done    = false;

    Picker Chooser;
    Search Finder;
    Card   FileCard;
    Help   Keys;
    List   Shown;
    Window Front = Window::Contents;

    std::string Message;
    bool        MessageTrouble = false;

    std::vector<file_id_t>   Opened;
    std::vector<std::string> Troubles;
};

}  // namespace read

#endif
