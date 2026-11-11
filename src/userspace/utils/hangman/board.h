/* board.h — the words beside the scene: the category, the word's slots with
 * their letters as they are found, the mistakes, the alphabet with the letters
 * used marked, the hints and the standing; and when the round is over, how it
 * ended. It paints what it is given and keeps nothing. */
#ifndef HANGMAN_BOARD_H
#define HANGMAN_BOARD_H

#include <bitset>
#include <cstdint>
#include <string_view>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "ledger.h"
#include "look.h"
#include "round.h"

namespace hangman {

enum class NoteKind : std::uint8_t { Plain, Good, Bad, Warn };

struct Facts {
    const Round                      &Playing;
    const Look                       &Shown;
    const Tally                      &Total;
    const std::vector<std::uint64_t> &Lit;       // when each place of the word was found, 0 while hidden
    std::bitset<Letters>              Given;     // the letters a hint gave
    std::uint64_t                     Now;
    std::uint64_t                     EndedAt;   // 0 while the round goes on
    std::uint32_t                     Earned;    // the points of a won round
    bool                              Counted;   // whether the round is in the ledger (a friend's word is not)
    char                              Refused;   // the letter said twice, and when
    std::uint64_t                     RefusedAt;
    std::string_view                  Note;
    NoteKind                          NoteWas;
    std::uint64_t                     NoteAt;
    std::uint32_t                     HintsLeft;
    std::string_view                  HintKey;   // as the person would say it, "ctrl+t"
};

// How long ago Moment was, 0 for a moment still to come.
inline std::uint64_t Since(const Facts &Now, std::uint64_t Moment)
{
    return Now.Now > Moment ? Now.Now - Moment : 0;
}

// The paper of the page, under the scene and the words.
Color PaperOf(const Look &Shown);

void PaintBoard(painter::tui::Frame &Out, painter::core::Rect Area, const Facts &Now);

// The lines at the top and the bottom of the glass.
void PaintTopBar(painter::tui::Frame &Out, const Look &Shown);
void PaintHintBar(painter::tui::Frame &Out, const Look &Shown, std::string_view Text);

}  // namespace hangman

#endif
