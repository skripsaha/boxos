/*
 * spell — a word in large letters that move: type, and each letter drops into
 * its place on a spring, turning straight as it lands, each in a colour of its
 * own along the rainbow as the eye sees it, the letters before it making room;
 * backspace lets the last one fall away; enter shuffles the letters into an
 * anagram, each leaping up and over to its new place, and again puts them
 * back; tab sends a wave along the word. A word too long for the screen
 * shakes its head. Escape leaves. spell says only where each letter goes and
 * how; the display plays it while spell waits for the next key.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "painter/core/color.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"
#include "painter/tui/keyboard.h"

namespace {

using namespace std::chrono_literals;
namespace gui = painter::gui;
using painter::core::Color;
using painter::core::PointF;

constexpr float         Big        = 120;    // the letters' size, in points
constexpr float         Gap        = 4;      // between letters
constexpr float         Space      = 44;     // a space's width
constexpr std::size_t   LettersMost = 40;
constexpr std::uint32_t Fell = 1u << 20, Leapt = 2u << 20, Waved = 3u << 20, Shook = 4u << 20;

std::uint32_t Random()
{
    static std::uint32_t State = 0x2545F491u;
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

float Between(float Low, float High) { return Low + (High - Low) * static_cast<float>(Random() % 10000) / 10000.0f; }

/* The Nth colour along a rainbow as the eye sees it: one lightness, one
 * strength, the hue turning. */
Color Hue(std::uint32_t Nth)
{
    const float Turn = 0.62f * static_cast<float>(Nth) + 0.4f;
    float       R = 0, G = 0, B = 0;
    painter::core::RgbOf(painter::core::Oklab{0.78f, 0.15f * std::cos(Turn), 0.15f * std::sin(Turn)}, R, G, B);
    const auto Byte = [](float Of) { return static_cast<std::uint8_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 255)); };
    return Color::Rgb(Byte(R), Byte(G), Byte(B));
}

std::string Utf8Of(char32_t Character)
{
    std::string Made;
    if (Character < 0x80) {
        Made += static_cast<char>(Character);
    } else if (Character < 0x800) {
        Made += static_cast<char>(0xC0 | (Character >> 6));
        Made += static_cast<char>(0x80 | (Character & 0x3F));
    } else if (Character < 0x10000) {
        Made += static_cast<char>(0xE0 | (Character >> 12));
        Made += static_cast<char>(0x80 | ((Character >> 6) & 0x3F));
        Made += static_cast<char>(0x80 | (Character & 0x3F));
    } else {
        Made += static_cast<char>(0xF0 | (Character >> 18));
        Made += static_cast<char>(0x80 | ((Character >> 12) & 0x3F));
        Made += static_cast<char>(0x80 | ((Character >> 6) & 0x3F));
        Made += static_cast<char>(0x80 | (Character & 0x3F));
    }
    return Made;
}

/* A letter as typed: its glyph — none for a space — and how wide it is. */
struct Letter {
    std::optional<gui::Text> Glyph;
    float                    Wide = Space;
};

class Spell {
public:
    Spell(gui::Area Opened, painter::tui::Keyboard Heard)
        : Screen(std::move(Opened)), Keys(std::move(Heard)), Page(Screen.Rectangle(gui::Role::Page)),
          Hint(Screen.Text("type a word    backspace  a letter falls    enter  an anagram    tab  a wave    esc  leave", gui::Role::Hint))
    {
        Keys.HearEnd();
    }

    void Run()
    {
        Lay(false);
        Hint.SetOpacity(0);
        Screen.Animate(gui::Animation::Out(900ms), [&] { Hint.SetOpacity(1); });
        (void)Screen.Present();
        for (;;) {
            const gui::Event Heard = Screen.Wait(&Keys);
            if (Heard.What == gui::Event::Kind::Gone || Heard.What == gui::Event::Kind::EndAsked) return;
            if (Heard.What == gui::Event::Kind::Key && !Key(Heard.Key)) return;
            if (Heard.What == gui::Event::Kind::Area) Lay(true);
            if (Heard.What == gui::Event::Kind::AnimationEnd)
                std::erase_if(Falling, [&](const auto &Each) { return Each.first == Heard.Number; });
            (void)Screen.Present();
        }
    }

private:
    static gui::TextStyle StyleOf(Color Of)
    {
        gui::TextStyle Made;
        Made.Size   = Big;
        Made.Weight = 800;
        Made.Color  = Of;
        return Made;
    }

    float Height() const { return Big * 1.25f; }
    float Across() const
    {
        float Wide = 0;
        for (const Letter &Each : Word) Wide += Each.Wide + Gap;
        return Word.empty() ? 0 : Wide - Gap;
    }
    float Top() const { return Screen.Size().Y / 2 - Height() / 2; }

    /* Where each letter stands, left to right in the order shown. */
    std::vector<float> Lefts() const
    {
        std::vector<float> Made(Word.size());
        float              At = Screen.Size().X / 2 - Across() / 2;
        for (std::size_t Shown : Order) {
            Made[Shown] = At;
            At += Word[Shown].Wide + Gap;
        }
        return Made;
    }

    /* The page and the words where they lie; the letters gliding to their
     * places when In time. */
    void Lay(bool InTime)
    {
        Page.SetPlace({0, 0}).SetSize(Screen.Size()).SetFill(gui::Fill::Of(Color::Rgb(0x10, 0x10, 0x16)));
        Hint.SetPlace({32, Screen.Size().Y - 40});
        const auto Glide = [&] {
            const std::vector<float> At = Lefts();
            for (std::size_t I = 0; I < Word.size(); I++)
                if (Word[I].Glyph) Word[I].Glyph->SetPlace({At[I], Top()}).SetTurn(0);
        };
        if (InTime) Screen.Animate(gui::Animation::Spring(550ms, 0.2f), Glide);
        else Glide();
    }

    void Type(char32_t Character)
    {
        if (Word.size() >= LettersMost || Across() + Big > Screen.Size().X * 0.94f) {
            Shake();
            return;
        }
        Letter Made;
        if (Character != U' ') {
            const std::string Said  = Utf8Of(Character);
            const Color       Ink   = Hue(Typed++);
            const auto        Style = StyleOf(Ink);
            Made.Wide               = gui::Measure(Screen, Said, gui::Role::Heading, gui::ByContent, Style).X;
            Made.Glyph              = Screen.Text(Said, gui::Role::Heading);
            Made.Glyph->SetStyle(Style);
        }
        Word.push_back(std::move(Made));
        Order.push_back(Word.size() - 1);
        const std::vector<float> At = Lefts();
        if (std::optional<gui::Text> &New = Word.back().Glyph) {
            /* From above, turned, unseen: dropped onto its place. */
            New->SetPlace({At.back(), Top() - 260}).SetTurn(Between(-30, 30)).SetAnchor({Word.back().Wide / 2, Height() / 2}).SetOpacity(0);
            Screen.Animate(gui::Animation::Spring(800ms, 0.35f), [&] { New->SetPlace({At.back(), Top()}).SetTurn(0); });
            Screen.Animate(gui::Animation::Out(250ms), [&] { New->SetOpacity(1); });
        }
        Lay(true);
    }

    void Drop()
    {
        if (Word.empty()) return;
        const float Stood = Lefts().back();
        Letter      Last  = std::move(Word.back());
        Word.pop_back();
        Order.erase(std::remove(Order.begin(), Order.end(), Word.size()), Order.end());
        if (Last.Glyph) {
            /* Down off the screen, turning, fading: let go once it has. */
            const std::uint32_t Number = Fell + (Random() & 0xFFFFF);
            gui::Animation      Fall   = gui::Animation::In(500ms);
            Fall.Number                = Number;
            Screen.Animate(Fall, [&] { Last.Glyph->SetPlace({Stood, Screen.Size().Y + 40}).SetTurn(Between(-70, 70)).SetOpacity(0); });
            Falling.push_back({Number, std::move(*Last.Glyph)});
        }
        Lay(true);
    }

    /* Every letter leaps up and over to its place in a new order — or back
     * to the word as typed. */
    void Shuffle()
    {
        if (Word.size() < 2) return;
        Shuffled = !Shuffled;
        const std::vector<float> Was = Lefts();
        if (Shuffled) {
            for (std::size_t I = Order.size() - 1; I > 0; I--) std::swap(Order[I], Order[Random() % (I + 1)]);
        } else {
            std::iota(Order.begin(), Order.end(), std::size_t{0});
        }
        const std::vector<float> At = Lefts();
        for (std::size_t I = 0; I < Word.size(); I++) {
            if (!Word[I].Glyph) continue;
            gui::Text     &Glyph = *Word[I].Glyph;
            gui::Animation Up    = gui::Animation::Out(220ms);
            Up.Delay             = std::chrono::milliseconds(35 * I);
            Up.Number            = Leapt + static_cast<std::uint32_t>(I);
            Screen.Animate(Up, [&] { Glyph.SetPlace({(Was[I] + At[I]) / 2, Top() - Big * 0.9f}).SetTurn(Between(-20, 20)); });
            gui::Animation Down = gui::Animation::Spring(700ms, 0.3f);
            Down.After          = Leapt + static_cast<std::uint32_t>(I);
            Screen.Animate(Down, [&] { Glyph.SetPlace({At[I], Top()}).SetTurn(0); });
        }
    }

    /* A wave along the word: each letter up, and down on a spring that
     * bounces, one after another. */
    void Wave()
    {
        const std::vector<float> At = Lefts();
        for (std::size_t Shown = 0; Shown < Order.size(); Shown++) {
            const std::size_t I = Order[Shown];
            if (!Word[I].Glyph) continue;
            gui::Text     &Glyph = *Word[I].Glyph;
            gui::Animation Up    = gui::Animation::Out(160ms);
            Up.Delay             = std::chrono::milliseconds(55 * Shown);
            Up.Number            = Waved + static_cast<std::uint32_t>(I);
            Screen.Animate(Up, [&] { Glyph.SetPlace({At[I], Top() - Big * 0.45f}); });
            gui::Animation Down = gui::Animation::Spring(650ms, 0.5f);
            Down.After          = Waved + static_cast<std::uint32_t>(I);
            Screen.Animate(Down, [&] { Glyph.SetPlace({At[I], Top()}); });
        }
    }

    /* No room: the word shakes its head. */
    void Shake()
    {
        const std::vector<float> At = Lefts();
        for (std::size_t I = 0; I < Word.size(); I++) {
            if (!Word[I].Glyph) continue;
            gui::Text     &Glyph = *Word[I].Glyph;
            gui::Animation Side  = gui::Animation::InOut(55ms);
            Side.Repeats         = 4;
            Side.Back            = true;
            Side.Number          = Shook;
            Screen.Animate(Side, [&] { Glyph.SetPlace({At[I] + 14, Top()}); });
            gui::Animation Home = gui::Animation::Out(80ms);
            Home.After          = Shook;
            Screen.Animate(Home, [&] { Glyph.SetPlace({At[I], Top()}); });
        }
    }

    bool Key(const painter::tui::KeyEvent &Pressed)
    {
        if (Pressed.IsExtended() || Pressed.IsCtrl() || Pressed.IsAlt()) return true;
        switch (Pressed.Character) {
        case painter::tui::KeyCodes::Escape: return false;
        case painter::tui::KeyCodes::Backspace: Drop(); return true;
        case painter::tui::KeyCodes::Enter: Shuffle(); return true;
        case painter::tui::KeyCodes::Tab: Wave(); return true;
        default:
            if (Pressed.IsPrintable()) Type(Pressed.Character);
            return true;
        }
    }

    gui::Area                                        Screen;
    painter::tui::Keyboard                           Keys;
    gui::Shape                                       Page;
    gui::Text                                        Hint;
    std::vector<Letter>                              Word;      // as typed
    std::vector<std::size_t>                         Order;     // as shown, left to right
    std::vector<std::pair<std::uint32_t, gui::Text>> Falling;   // let go once fallen
    std::uint32_t                                    Typed    = 0;
    bool                                             Shuffled = false;
};

}  // namespace

int main()
{
    std::expected<gui::Area, box::error> Opened = gui::Area::Open(gui::Placement::Whole);
    if (!Opened || Opened->On() != gui::Glass::Pixels) {
        std::printf("spell draws on a framebuffer's pixels; this screen is a text page\n");
        return 1;
    }
    std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
    if (!Keys) {
        std::printf("spell: the keyboard is not to be had\n");
        return 1;
    }
    Spell(std::move(*Opened), std::move(*Keys)).Run();
    return 0;
}
