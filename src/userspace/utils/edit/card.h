#ifndef EDIT_CARD_H
#define EDIT_CARD_H

/* The file's card: its name and its tags, Ctrl+T. A file that is not on the
 * volume yet is made from its card by its first save — named and tagged as
 * it is born, the Use Context's tags stamped on it with the rest.
 */

#include <string>

#include "field.h"
#include "paint.h"
#include "tab.h"

namespace edit {

class Card {
public:
    /* What edit does once the card has done its work. */
    enum class Then { Stay, GoOnLeaving };
    /* Closed with words in Said: the file was made and its text was not
     * written, and Said says why. */
    enum class Outcome { Stay, Closed, Done };

    void    Begin(const Tab &For, Then After, core::Size Screen);
    Outcome OnKey(const tui::KeyEvent &Key, Tab &For, std::string &Said);
    Then    After() const noexcept { return Next; }
    void    Paint(tui::Frame &Out, const Tab &For) const;

private:
    bool Make(Tab &For, std::string &Said);
    bool Change(Tab &For, std::string &Said);

    Field       Name{"name"};
    Field       Tags{"tags"};
    bool        OnTags = false;
    Then        Next   = Then::Stay;
    std::string Context;
    std::string Trouble;
    core::Size  Screen;
};

/* The tab's text into its file, and the tab marked saved; the sentence that
 * says what happened. False when the volume refused, and Said says why. */
bool SaveTab(Tab &For, std::string &Said);

}  // namespace edit

#endif
