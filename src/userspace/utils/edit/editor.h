#ifndef EDIT_EDITOR_H
#define EDIT_EDITOR_H

/* edit as a whole: the tabs, the window in front of the text when one is
 * open, the keys, and leaving.
 *
 * One window at a time is in front, and while it is the keys are its own: the
 * file chooser, find and replace, go to a line, the help, the file's card, and
 * the question of what to do with changes not saved. Every frame is the whole
 * picture drawn again from what is held here, so nothing on the glass can
 * disagree with the text.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "box/file.h"
#include "painter/tui/stage.h"

#include "card.h"
#include "field.h"
#include "keymap.h"
#include "paint.h"
#include "picker.h"
#include "search.h"
#include "tab.h"

namespace edit {

class Editor {
public:
    explicit Editor(tui::Stage Screen);

    /* A file of the volume in a tab of its own, or the tab it is in already. */
    void Open(file_id_t File);
    /* A name no file has yet: the file made now, empty, where edit is looking
     * — so it wears from the start what the volume gives a file there — and
     * let go again if its tab closes with nothing ever written to it. */
    void OpenNew(std::string Name);
    void Run();

    /* Every file edit had open, for whoever started it. */
    std::vector<file_id_t> Given() const;
    std::string            Summary() const;

private:
    enum class Mode { Writing, Picking, Finding, GoingTo, Helping, Carding, Leaving };

    /* editor.cpp — keys */
    void OnKey(const tui::KeyEvent &Key);
    void OnWriting(const tui::KeyEvent &Key);
    void Do(const Heard &Asked);
    void DoBeyondText(const Heard &Asked);
    void OnPicking(const tui::KeyEvent &Key);
    void OnGoingTo(const tui::KeyEvent &Key);
    void OnHelping(const tui::KeyEvent &Key);
    void OnCarding(const tui::KeyEvent &Key);
    void OnLeaving(const tui::KeyEvent &Key);

    /* editor.cpp — tabs and files */
    Tab        &Active() noexcept { return *Tabs[Current]; }
    const Tab  &Active() const noexcept { return *Tabs[Current]; }
    std::size_t IndexOf(const Tab *Which) const noexcept;
    void        Close(std::size_t Index);
    void        OpenChooser();
    void        Save();
    void        AskToLeave(bool All);
    void        GoOnLeaving();
    void        LetGo(const Tab &One);
    void        Rescope();
    void        Remember(file_id_t File);
    void        Note(std::string Words, bool Trouble = false);

    /* paint.cpp — the picture */
    bool         Paint();
    void         PaintTabs(tui::Frame &Out) const;
    void         PaintText(tui::Frame &Out);
    void         PaintLine(tui::Frame &Out, std::int32_t Y, std::size_t Line);
    void         PaintStatus(tui::Frame &Out) const;
    void         PaintGoTo(tui::Frame &Out) const;
    void         PaintHelp(tui::Frame &Out) const;
    void         PaintLeaving(tui::Frame &Out) const;
    std::int32_t GutterWidth() const noexcept;
    std::size_t  TextRows() const noexcept;
    std::size_t  TextCols() const noexcept;

    tui::Stage                        Screen;
    core::Size                        Bounds;
    std::vector<std::unique_ptr<Tab>> Tabs;
    std::size_t                       Current = 0;
    Mode                              Now = Mode::Writing;
    Scope                             Looking = Scope::InContext;
    bool                              Done = false;
    bool                              Overtype = false;
    Clip                              Clipboard;

    Picker   Chooser;
    Search   Finder;
    Card     FileCard;
    Field    LineField{"line"};
    Position LineOrigin;
    std::size_t HelpFirst = 0;

    std::vector<Tab *> Unsaved;      /* the tabs the question of leaving is about */
    bool               LeavingAll = false;
    Tab               *Closing = nullptr;

    std::string Message;
    bool        MessageTrouble = false;

    std::vector<file_id_t>   Touched;
    std::vector<std::string> SavedNames;
};

}  // namespace edit

#endif
