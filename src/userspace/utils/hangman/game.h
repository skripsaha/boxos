/* game.h — hangman as it is played: the glass, the settings, the chances, the
 * ledger, the scene and the round in hand, and the keys. The rules are round's,
 * the picture scene's, the words beside it board's, and the two windows
 * panels'. It asks the machine for nothing but the screen and the clock. */
#ifndef HANGMAN_GAME_H
#define HANGMAN_GAME_H

#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/tui/stage.h"

#include "board.h"
#include "ledger.h"
#include "look.h"
#include "luck.h"
#include "panels.h"
#include "round.h"
#include "scene.h"
#include "words.h"

namespace hangman {

// What was asked at the door: the words to come from one category, at one level, this time.
struct Request {
    std::optional<std::size_t> Only;
    std::optional<Level>       Hardness;
};

class Game {
public:
    Game(painter::tui::Stage Opened, Request Asked);
    // Until the person leaves.
    void Run();

private:
    enum class Overlay : std::uint8_t { None, Help, Secret };

    // game.cpp: the loop, the keys and the picture.
    bool OnKey(const painter::tui::KeyEvent &Pressed);
    bool OnPlainKey(const painter::tui::KeyEvent &Pressed);
    void Paint();
    bool Chose(HangKey Which, const painter::tui::KeyEvent &Pressed) const;
    std::string Spelled(HangKey Which) const;
    painter::core::Rect SceneCells(painter::core::Size Size) const;
    bool Moving() const;

    // play.cpp: the rounds.
    void Next();
    void Begin(std::string Word, std::string Label, bool Counted);
    void Say(char Letter);
    void TakeHint();
    void GiveUp();
    void Finish();
    void Light(char Letter);
    void Tell(std::string Words, NoteKind Kind);
    std::uint32_t Points() const;

    painter::tui::Stage  Screen;
    box::settings::watch Watch;
    Look                 Chosen;
    Request              Asked;
    Luck                 Chance;
    Deck                 Words;
    Ledger               Book;
    Scene                Picture;
    SecretEntry          Secret;
    std::optional<Round> Now;
    std::vector<std::uint64_t> Lit;
    std::bitset<Letters> Given;
    Overlay              Window = Overlay::None;
    std::uint64_t        Time = 0;
    std::uint64_t        EndedAt = 0;
    std::uint64_t        RefusedAt = 0;
    std::uint64_t        NoteAt = 0;
    std::uint32_t        Earned = 0;
    std::uint32_t        Paid = 0;           // the points the hints of this word have taken
    char                 Refused = 0;
    bool                 Counted = true;
    std::string          Note;
    NoteKind             NoteWas = NoteKind::Plain;
};

}  // namespace hangman

#endif
