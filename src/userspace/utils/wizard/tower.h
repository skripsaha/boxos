/* tower.h — the wizard's tower: what is in the cauldron, what he has said and
 * cast, what he is doing with his hands and his face, and the keys. The room is
 * room's, the wizard figure's, the words speech's, the book grimoire's and book's,
 * what he knows lore's. The only thing it asks of the machine is the glass,
 * through box::glass. */
#ifndef WIZARD_TOWER_H
#define WIZARD_TOWER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "canvas.h"
#include "figure.h"
#include "grimoire.h"
#include "look.h"
#include "lore.h"
#include "room.h"
#include "scene.h"
#include "specks.h"
#include "speech.h"

namespace wizard {

// A thing on its way from his hand to the cauldron.
struct Flight {
    std::size_t   Which;
    std::uint64_t Began;
    float         FromX, FromY;
};

// What he is about to do with his hands before the glass changes: drinking and
// casting take a moment, and the spell comes at the top of it.
struct Pending {
    Casting       What;
    std::uint64_t At;       // when it is laid
    bool          Drinking;
};

// The grimoire as it is open on the glass.
struct BookView {
    bool Open = false;
    int  Page = 0;          // 0 the potions, 1 the spells
    int  Cursor[2] = {0, 0};
};

class Tower {
public:
    // The tower on Screen. Words are what was asked at the door: one charm to
    // cast at once, or things to put in the cauldron and drink (checked by
    // Misunderstood() before the screen is taken).
    Tower(painter::tui::Stage Screen, std::vector<std::string> Words);
    // Until the person leaves, or the wizard is asked to finish.
    void Run();

    // Whether Words are a charm, or ingredients; else the first that is neither.
    static std::optional<std::string> Misunderstood(const std::vector<std::string> &Words);

private:
    // tower.cpp: the keys and what they do.
    void Door();
    bool OnKey(const painter::tui::KeyEvent &Key);
    bool OnBookKey(const painter::tui::KeyEvent &Key);
    void Browse(int By);
    void Throw();
    void TakeBack();
    void Drink();
    void CastRandom();
    void Cast(const Charm &Which);
    void Empty();
    void Lift();
    void Lay(Casting Spelt);
    std::string Spelled(WizardKey Key) const;

    // tower_life.cpp: what moves of itself.
    void Step(std::uint32_t ElapsedMs);
    void Land();
    void Burst(float X, float Y, Color Ink, std::uint32_t Count, SpeckKind Kind);
    void Tell(std::string Title, std::string Heard);
    Pose Posed() const;
    Show ShowNow() const;

    // paint.cpp: the picture, and the words round it.
    void Paint();
    void PaintBar(painter::tui::Frame &Out) const;
    void PaintShelf(painter::tui::Frame &Out) const;
    void PaintHints(painter::tui::Frame &Out) const;
    void PaintHelp(painter::tui::Frame &Out) const;
    void DrawShelf();
    // book.cpp: the grimoire's pages.
    void PaintBook(painter::tui::Frame &Out) const;

    std::uint32_t LeftSeconds() const;

    painter::tui::Stage       Screen;
    box::settings::watch      Watch;
    Look                      Chosen;
    Dice                      Luck;
    std::vector<std::string>  Words;
    Scene                     Where;
    Canvas                    Picture;
    Room                      Hall;
    Figure                    Wizard;
    Specks                    Dust;
    Speech                    Said;
    Grimoire                  Book;
    BookView                  Reading;
    std::vector<std::uint8_t> Brewing;         // what is in the cauldron, by place in Ingredients()
    std::size_t               Pick = 0;      // the thing on the shelf he is looking at
    std::optional<Flight>     Falling;
    std::optional<Pending>    Soon;
    std::optional<Casting>    Spoken;        // the last he cast
    const Charm              *LastCharm = nullptr;
    std::uint64_t             LaidUntilMs = 0;   // 0: nothing lies over the glass by his hand
    std::uint64_t             LastMs = 0;
    std::uint64_t             NextBlinkMs = 0;
    std::uint64_t             BlinkMs = 0;
    std::uint64_t             StirUntilMs = 0;
    std::uint64_t             CastUntilMs = 0;
    std::uint64_t             DrinkUntilMs = 0;
    std::uint64_t             GazeUntilMs = 0;
    std::uint32_t             SparkMs = 0;
    float                     Excite = 0.0f;
    float                     Splash = 0.0f;
    float                     Spin = 0.0f;
    float                     GazeX = 0.0f, GazeY = 0.0f, GazeToward = 0.0f, GazeTowardUp = 0.0f;
    float                     StirLevel = 0.0f, CastLevel = 0.0f, DrinkLevel = 0.0f;
    float                     MoodWeight = 0.0f;
    Mood                      Face = Mood::Pleased;
    bool                      Help = false;
};

}  // namespace wizard

#endif
