#ifndef EDIT_CARD_H
#define EDIT_CARD_H

/* The file's card, Ctrl+T: its name and every tag it wears, as the volume
 * says them — the Use Context's it was born with and the one for its name
 * among them. What the card changes is what changed between the tags it was
 * opened with and the tags it is left with; the name after them. Tab in the
 * tags offers the tags files wear (Suggest).
 */

#include <string>
#include <vector>

#include "field.h"
#include "paint.h"
#include "suggest.h"
#include "tab.h"

namespace edit {

class Card {
public:
    enum class Outcome { Stay, Closed, Done };

    void    Begin(const Tab &For, Scope Looking, core::Size Screen);
    /* edit now looks elsewhere: an open list of tags is asked again. */
    void    Rescope(Scope Looking);
    Outcome OnKey(const tui::KeyEvent &Key, Tab &For, std::string &Said);
    void    Paint(tui::Frame &Out) const;

private:
    bool       Keep(Tab &For, std::string &Said);
    void       Offer();
    void       Take(const std::string &Tag);
    core::Rect Area() const noexcept;

    Field                    Name{"name"};
    Field                    Tags{"tags"};
    Suggest                  Hints;
    std::vector<std::string> Had;
    Scope                    Looking = Scope::InContext;
    bool                     OnTags  = false;
    std::string              Trouble;
    core::Size               Screen;
};

/* The tab's text into its file, and the tab marked saved; the sentence that
 * says what happened. False when the volume refused, and Said says why. */
bool SaveTab(Tab &For, std::string &Said);

}  // namespace edit

#endif
