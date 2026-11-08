#ifndef KIN_CARD_H
#define KIN_CARD_H

/* The file's card, Ctrl+T: its name and every tag it wears, as the volume
 * says them — the Use Context's it was born with and the one for its name
 * among them. What the card changes is what changed between the tags it was
 * opened with and the tags it is left with; the name after them. Tab in the
 * tags offers the tags files wear (Suggest).
 */

#include <string>
#include <vector>

#include "box/file.h"

#include "field.h"
#include "look.h"
#include "suggest.h"
#include "volume.h"

namespace kin {

class Card {
public:
    /* Done: the card was kept, and Said says what the file wears now. */
    enum class Outcome { Stay, Closed, Done };

    /* The card of File, called Name and wearing Tags, as the program last
     * heard them from the volume. */
    void    Begin(file_id_t File, std::string Name, std::string Tags, Scope Looking, core::Size Screen);
    /* The program now looks elsewhere: an open list of tags is asked again. */
    void    Rescope(Scope Looking);
    Outcome OnKey(const tui::KeyEvent &Key, std::string &Said);
    void    Paint(tui::Frame &Out) const;

    /* The file's name and tags as the volume holds them since the card last
     * asked to change them — whether it agreed or not. */
    const std::string &Name() const noexcept { return Called; }
    const std::string &Tags() const noexcept { return Worn; }

private:
    bool       Keep(std::string &Said);
    void       Offer();
    void       Take(const std::string &Tag);
    core::Rect Area() const noexcept;

    file_id_t                File = 0;
    std::string              Called;
    std::string              Worn;
    Field                    NameField{"name"};
    Field                    TagsField{"tags"};
    Suggest                  Hints;
    std::vector<std::string> Had;
    Scope                    Looking = Scope::InContext;
    bool                     OnTags  = false;
    std::string              Trouble;
    core::Size               Screen;
};

}  // namespace kin

#endif
