/* tower.h — the wizard's tower: what is in the cauldron, what he has said and
 * cast, and the keys. The picture is paint.cpp's, the pictures' parts are
 * art's, what he knows is lore's. The only thing it asks of the machine is the
 * glass, through box::glass. */
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

#include "art.h"
#include "look.h"
#include "lore.h"

namespace wizard {

// Where everything stands: the picture's scale and size in its own pixels, the
// rows the art takes, and the wizard's and the cauldron's places in it.
struct Scene {
    std::int32_t ArtTopRow, ArtRows, Scale, OriginY;
    painter::core::Size Size;
    std::int32_t WizardX, WizardY, CauldronX, CauldronY;
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
    void Door();
    bool OnKey(const painter::tui::KeyEvent &Key);
    void CastSpell();
    void Put(std::size_t Ingredient);
    void Drink();
    void Empty();
    void Lift();
    void Lay(Casting Spelt);
    void Step(std::uint32_t ElapsedMs);
    void Remark(std::string Words);
    void Burst(std::int32_t X, std::int32_t Y, Color Ink, std::uint32_t Count);
    std::uint32_t LeftSeconds() const;
    std::string   Spelled(WizardKey Key) const;
    Scene         Placed() const;

    // paint.cpp: the picture.
    void Paint();
    void PaintBars(painter::tui::Frame &Out) const;
    void PaintSpeech(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintShelf(painter::tui::Frame &Out, painter::core::Rect Area) const;
    void PaintHelp(painter::tui::Frame &Out) const;

    painter::tui::Stage    Screen;
    box::settings::watch   Watch;
    Look                   Shown;
    Dice                   Luck;
    std::vector<std::string> Words;
    std::vector<std::uint8_t> Mixed;       // what is in the cauldron, by place in Ingredients()
    std::optional<Casting> Spoken;         // what he said of the last he cast
    const Charm           *LastCharm = nullptr;
    std::uint64_t          LaidUntilMs = 0;   // 0: nothing lies over the glass by his hand
    std::string            Status;
    std::uint64_t          StatusUntilMs = 0;
    std::uint64_t          LastMs = 0;
    std::uint64_t          BubbleMs = 0;
    std::uint32_t          Excitement = 0;
    Specks                 Sparks;
    bool                   Help = false;
};

}  // namespace wizard

#endif
