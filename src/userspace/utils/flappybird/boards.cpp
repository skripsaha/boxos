/* boards.cpp — the score, the words and the board over the world. See
 * boards.h. Laid in units of the field (world.h), Unit points each. */

#include "boards.h"

#include <chrono>
#include <cmath>
#include <string>

#include "paint.h"

namespace flappybird {

namespace {

using namespace std::chrono_literals;
using painter::core::Corners;
using painter::core::PointF;

constexpr float BoardWide = 226, BoardTall = 114;
constexpr float MedalX = 50, MedalY = 64, MedalR = 22;

/* The medal a score earns, as the original game gave them: bronze at ten,
 * silver at twenty, gold at thirty, platinum at forty. */
struct MedalLook {
    unsigned From;
    Color    Light, Body, Dark;
};
constexpr MedalLook Medals[] = {
    {40, Color::Rgb(0xF8, 0xFC, 0xFF), Color::Rgb(0xD6, 0xE6, 0xEE), Color::Rgb(0x8E, 0xAA, 0xBA)},
    {30, Color::Rgb(0xFF, 0xF2, 0x9C), Color::Rgb(0xF6, 0xC6, 0x2E), Color::Rgb(0xB8, 0x84, 0x10)},
    {20, Color::Rgb(0xFF, 0xFF, 0xFF), Color::Rgb(0xD2, 0xD4, 0xD8), Color::Rgb(0x8C, 0x90, 0x96)},
    {10, Color::Rgb(0xF6, 0xC8, 0x96), Color::Rgb(0xD0, 0x86, 0x48), Color::Rgb(0x8A, 0x4E, 0x22)},
};

painter::core::Path StarPath(float Outer, float Inner, int Points)
{
    painter::core::Path Made;
    for (int I = 0; I < Points * 2; I++) {
        const float Turn = 3.14159265f * static_cast<float>(I) / static_cast<float>(Points) - 1.5707963f;
        const float R    = I % 2 == 0 ? Outer : Inner;
        const PointF At{R * std::cos(Turn), R * std::sin(Turn)};
        if (I == 0) Made.MoveTo(At);
        else Made.LineTo(At);
    }
    return Made.Close();
}

gui::TextBox Centred()
{
    gui::TextBox Box;
    Box.Align = ITEM_TEXT_CENTER;
    Box.Wrap  = false;
    return Box;
}

gui::TextBox ToRight()
{
    gui::TextBox Box;
    Box.Align = ITEM_TEXT_RIGHT;
    Box.Wrap  = false;
    return Box;
}

}  // namespace

Boards::Boards(gui::Area &On, float Unit, float Width) : Screen(On), U(Unit), Wide(Width)
{
    Points = Line("0", 44, ink::White, 40);
    Points.SetAnchor({Wide / 2, 26 * U}).Hide();
    Title = Line("Get Ready", 38, ink::Title, 120);
    Hint  = Line("space, up or enter flies    p pauses    esc leaves", 11, ink::Hint, 296);
    Best  = Line("", 11, ink::Hint, 318);
    Pause = Line("Paused", 38, ink::Title, 170);
    PauseHint = Line("p goes on    esc leaves", 11, ink::Hint, 222);
    Pause.Hide();
    PauseHint.Hide();
    OverTitle = Line("Game Over", 38, ink::Title, 118);
    OverHint  = Line("space flies again    esc leaves", 11, ink::Hint, 324);
    OverTitle.Hide();
    OverHint.Hide();
    MakeBoard();

    Blink = Screen.Rectangle();
    Blink.SetSize(Screen.Size()).SetFill(gui::Fill::Of(ink::White)).SetOpacity(0);
    Curtain = Screen.Rectangle();
    Curtain.SetSize(Screen.Size()).SetFill(gui::Fill::Of(ink::Black)).SetOpacity(0);
}

/* A line of words across the whole width, its middle in the middle, its top
 * Y units down; Size units tall, in its colour on the game's dark. */
gui::Text Boards::Line(std::string_view Words, float Size, painter::core::Color With, float Y)
{
    gui::Text Made = Screen.Text(Words, gui::Role::Heading);
    Made.SetPlace({0, Y * U}).SetSize({Wide, gui::ByContent}).SetBox(Centred()).SetStyle(paint::Words(Size * U, With)).SetShadow(paint::Under(U));
    return Made;
}

void Boards::MakeBoard()
{
    Board = Screen.Group();
    Board.Hide();
    Plate = Board.Rectangle();
    Plate.SetSize({BoardWide * U, BoardTall * U})
        .SetCorners(Corners::All(7 * U))
        .SetFill(paint::Down(0, BoardTall * U, {{0, ink::BoardLine}, {0.08f, ink::Board}, {1, ink::Board}}))
        .SetStroke(paint::Edge(2.6f * U))
        .SetShadow(gui::Shadow{{0, 4 * U}, 0, 0, Color::Rgba(0x54, 0x38, 0x47, 120), false});
    const auto Label = [&](std::string_view Words, float X, float Y, float Width, bool Right) {
        gui::Text Made = Board.Text(Words, gui::Role::Label);
        Made.SetPlace({X * U, Y * U}).SetSize({Width * U, gui::ByContent}).SetBox(Right ? ToRight() : Centred()).SetStyle(paint::Words(10 * U, ink::Label));
        return Made;
    };
    const auto Number = [&](float Y) {
        gui::Text Made = Board.Text("0", gui::Role::Heading);
        Made.SetPlace({100 * U, Y * U}).SetSize({110 * U, gui::ByContent}).SetBox(ToRight()).SetStyle(paint::Words(22 * U, ink::White)).SetShadow(paint::Under(0.8f * U));
        return Made;
    };
    MedalLabel  = Label("MEDAL", MedalX - 34, 16, 68, false);
    ScoreLabel  = Label("SCORE", 100, 16, 110, true);
    ScoreNumber = Number(28);
    BestLabel   = Label("BEST", 100, 62, 110, true);
    BestNumber  = Number(74);
    NewBadge    = Board.Rectangle();
    NewBadge.SetPlace({112 * U, 82 * U}).SetSize({30 * U, 13 * U}).SetCorners(Corners::All(3 * U)).SetFill(gui::Fill::Of(ink::New));
    NewWord = Board.Text("NEW", gui::Role::Label);
    NewWord.SetPlace({112 * U, 82.5f * U}).SetSize({30 * U, gui::ByContent}).SetBox(Centred()).SetStyle(paint::Words(9 * U, ink::White));
    Socket = Board.Ellipse();
    Socket.SetPlace({(MedalX - MedalR) * U, (MedalY - MedalR) * U})
        .SetSize({2 * MedalR * U, 2 * MedalR * U})
        .SetFill(gui::Fill::Of(Color::Transition(ink::Board, ink::Outline, 0.18f)));
    MakeMedal();
}

/* A medal: a disc lit from its top left, a ring pressed into it, a star, and
 * a glint that comes and goes on it for as long as it is shown. */
void Boards::MakeMedal()
{
    Medal = Board.Subgroup();
    Medal.SetPlace({MedalX * U, MedalY * U});
    Disc = Medal.Ellipse();
    Disc.SetPlace({-MedalR * U, -MedalR * U}).SetSize({2 * MedalR * U, 2 * MedalR * U}).SetStroke(paint::Edge(2 * U));
    Ring = Medal.Ellipse();
    Ring.SetPlace({-15 * U, -15 * U}).SetSize({30 * U, 30 * U}).SetFills({});
    Star = Medal.Outline(StarPath(10 * U, 4.2f * U, 5));
    Sparkle = Medal.Outline(StarPath(5 * U, 1.2f * U, 4));
    Sparkle.SetPlace({-9 * U, -10 * U}).SetFill(gui::Fill::Of(ink::White));
    gui::Animation Glint = gui::Animation::InOut(700ms);
    Glint.Repeats        = gui::Animation::Forever;
    Glint.Back           = true;
    Sparkle.SetScale({0.2f, 0.2f});
    Screen.Animate(Glint, [&] { Sparkle.SetScale({1, 1}); });
    Medal.Hide();
}

void Boards::ShowMedal(unsigned Score)
{
    for (const MedalLook &One : Medals) {
        if (Score < One.From) continue;
        Disc.SetFill(paint::Round({0, 0}, MedalR * U, {-8 * U, -8 * U}, {{0, One.Light}, {0.55f, One.Body}, {1, One.Dark}}));
        gui::Stroke Pressed = paint::Edge(1.6f * U, One.Dark);
        Ring.SetStroke(Pressed);
        Star.SetFill(gui::Fill::Of(One.Light)).SetStroke(paint::Edge(1.2f * U, One.Dark));
        Medal.Show();
        return;
    }
    Medal.Hide();
}

void Boards::Ready(unsigned Record)
{
    for (gui::Text *One : {&Title, &Hint, &Best}) One->Show().SetOpacity(1);
    Best.SetText(Record ? "best " + std::to_string(Record) : std::string());
    Points.Hide();
    for (gui::Text *One : {&OverTitle, &OverHint}) One->Hide();
    Board.Hide();
}

void Boards::Flying()
{
    Points.SetText("0").Show().SetOpacity(0);
    Screen.Animate(gui::Animation::Out(300ms), [&] {
        for (gui::Text *One : {&Title, &Hint, &Best}) One->SetOpacity(0);
        Points.SetOpacity(1);
    });
}

void Boards::Score(unsigned Now)
{
    Points.SetText(std::to_string(Now)).SetScale({1.22f, 1.22f});
    Screen.Animate(gui::Animation::Spring(320ms, 0.2f), [&] { Points.SetScale({1, 1}); });
}

void Boards::Flash()
{
    Blink.SetOpacity(0.85f);
    Screen.Animate(gui::Animation::Out(360ms), [&] { Blink.SetOpacity(0); });
}

void Boards::Over(unsigned Score, unsigned Record, bool New)
{
    Screen.Animate(gui::Animation::Out(250ms), [&] { Points.SetOpacity(0); });
    OverTitle.Show().SetPlace({0, -60 * U});
    gui::Animation Drop = gui::Animation::Spring(700ms, 0.35f);
    Drop.Delay          = 250ms;
    Screen.Animate(Drop, [&] { OverTitle.SetPlace({0, 118 * U}); });

    const float Left = (Wide - BoardWide * U) / 2;
    Board.Show().SetPlace({Left, Screen.Size().Y + 8 * U});
    ScoreNumber.SetText("0");
    BestNumber.SetText(std::to_string(Record));
    NewBadge.SetOpacity(0);
    NewWord.SetOpacity(0);
    Medal.Hide();
    gui::Animation Rise = gui::Animation::Spring(800ms, 0.22f);
    Rise.Delay          = 550ms;
    Screen.Animate(Rise, [&] { Board.SetPlace({Left, 184 * U}); });

    gui::Animation Late = gui::Animation::Out(450ms);
    Late.Delay          = 1300ms;
    Screen.Animate(Late, [&] {
        ShowMedal(Score);
        if (New) NewBadge.SetOpacity(1), NewWord.SetOpacity(1);
    });
    OverHint.Show().SetOpacity(0);
    Late.Delay = 1500ms;
    Screen.Animate(Late, [&] { OverHint.SetOpacity(1); });
}

void Boards::Count(unsigned Shown)
{
    ScoreNumber.SetText(std::to_string(Shown));
}

void Boards::Paused(bool On)
{
    if (On) {
        Pause.Show().SetOpacity(0);
        PauseHint.Show().SetOpacity(0);
    }
    Screen.Animate(gui::Animation::Out(200ms), [&] {
        Pause.SetOpacity(On ? 1.0f : 0.0f);
        PauseHint.SetOpacity(On ? 1.0f : 0.0f);
    });
}

void Boards::CloseCurtain()
{
    Curtain.SetOpacity(0);
    gui::Animation Close = gui::Animation::In(200ms);
    Close.Number         = CurtainClosed;
    Screen.Animate(Close, [&] { Curtain.SetOpacity(1); });
}

void Boards::OpenCurtain()
{
    Screen.Animate(gui::Animation::Out(320ms), [&] { Curtain.SetOpacity(0); });
}

}  // namespace flappybird
