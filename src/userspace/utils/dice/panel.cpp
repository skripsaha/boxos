/* panel.cpp — the result as lines and as the big total. */

#include "panel.h"

#include <algorithm>
#include <array>
#include <format>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

#include "digits.h"

namespace dice {

namespace {

namespace tui = painter::tui;
using painter::core::Rect;

// The panel's rule, then the least rows the total's digits need: a label
// above them and three cells of pixels.
constexpr int LeastBigRows = 5;
constexpr int LeastBlock   = 7;
constexpr int LeastTextWide = 24;
constexpr int MostLabel    = 14;

// Lines of runs, broken where a line is full; a line that goes on is indented
// under the label.
class Flow {
public:
    Flow(int Width, int Indent) : Width(Width), Indent(Indent) {}

    void Begin(const std::string &Label, Color Ink)
    {
        Finish();
        Current.push_back(Run{Label + std::string(static_cast<std::size_t>(std::max(1, Indent - static_cast<int>(Label.size()))), ' '), Ink});
        Used = Indent;
    }

    void Add(const std::string &Text, Color Ink)
    {
        const int Size = static_cast<int>(Text.size());
        if (Used > Indent && Used + 1 + Size > Width) {
            Lines.push_back(std::move(Current));
            Current = Line{Run{std::string(static_cast<std::size_t>(Indent), ' '), Ink}};
            Used    = Indent;
        }
        Current.push_back(Run{(Used > Indent ? " " : "") + Text, Ink});
        Used += (Used > Indent ? 1 : 0) + Size;
    }

    std::vector<Line> Done()
    {
        Finish();
        return std::move(Lines);
    }

private:
    void Finish()
    {
        if (!Current.empty()) Lines.push_back(std::move(Current));
        Current.clear();
    }

    int               Width;
    int               Indent;
    int               Used = 0;
    Line              Current;
    std::vector<Line> Lines;
};

std::string LabelOf(const GroupRoll &Group)
{
    if (Group.Of.Count == 0) return std::format("{}{}", Group.Sum < 0 ? '-' : '+', Group.Sum < 0 ? -Group.Sum : Group.Sum);
    return (Group.Of.Negative ? "-" : "") + Group.Of.Spelled;
}

std::vector<Line> LinesOf(const Roll &Thrown, int Width, const Look &Shown)
{
    int Indent = 0;
    for (const GroupRoll &Group : Thrown.Groups) Indent = std::max(Indent, static_cast<int>(LabelOf(Group).size()));
    Indent = std::min(Indent, MostLabel) + 1;
    Flow Out(Width, Indent);
    for (const GroupRoll &Group : Thrown.Groups) {
        Out.Begin(LabelOf(Group), Shown.Quiet);
        if (Group.Of.Count == 0) {
            Out.Add("modifier", Shown.Quiet);
            continue;
        }
        for (const Die &One : Group.Dice) {
            if (!One.Counts) {
                Out.Add(std::format("({})", One.Value), Shown.Quiet);
                continue;
            }
            const Standing Stands = StandingOf(Group.Of.Sides, One.Value);
            Out.Add(std::format("{}", One.Value),
                    Stands == Standing::High ? Shown.High : Stands == Standing::Low ? Shown.Low : Shown.Text);
        }
        Out.Add(std::format("= {}", Group.Sum), Shown.Body);
    }
    return Out.Done();
}

int BigWidth(const Roll &Thrown)
{
    return DigitsWide(std::format("{}", Thrown.Total).size(), 1);
}

void PaintBig(tui::Frame &Out, Rect Area, int Block, const Roll &Thrown, const Look &Shown)
{
    const int Left = Area.X + Area.Width - Block;
    Out.Write({Left + (Block - 5) / 2, Area.Y + 1}, "total", {Shown.Quiet, Shown.Panel});
    const Canvas On{Out, {Area.X, Area.Y * 2, Area.Width, Area.Height * 2}};
    DrawDigits(On, {Left + 1 + (Block - 2 - BigWidth(Thrown)) / 2, (Area.Y + 2) * 2 + 1}, std::format("{}", Thrown.Total),
               1, Shown.Body);
}

}  // namespace

Panel Compose(const Roll &Thrown, int Width, int MostRows, const Look &Shown)
{
    Panel Made;
    const int Block = std::max(LeastBlock, BigWidth(Thrown) + 2);
    Made.Big        = MostRows >= LeastBigRows && Width - Block - 1 >= LeastTextWide;
    Made.Lines      = LinesOf(Thrown, Made.Big ? Width - Block - 3 : Width - 2, Shown);
    if (!Made.Big)
        Made.Lines.push_back(Line{Run{"total ", Shown.Quiet}, Run{std::format("{}", Thrown.Total), Shown.Body}});
    const int Wanted = static_cast<int>(Made.Lines.size()) + 1;
    Made.Rows        = std::clamp(Wanted, Made.Big ? LeastBigRows : 2, std::max(2, MostRows));
    if (Wanted > Made.Rows) {
        Made.Lines.resize(static_cast<std::size_t>(Made.Rows - 1));
        Made.Lines.back().push_back(Run{" ...", Shown.Quiet});
    }
    return Made;
}

void PaintPanel(tui::Frame &Out, Rect Area, const Panel &Of, const Roll &Thrown, bool Revealed, const Look &Shown)
{
    Out.Fill(Area, tui::Glyphs::UpperHalf, {Shown.Panel, Shown.Panel});
    Out.Fill({Area.X, Area.Y, Area.Width, 1}, tui::Glyphs::RuleAcross, {Shown.Quiet, Shown.Panel});
    Out.Write({Area.X + 2, Area.Y}, " result ", {Shown.Text, Shown.Panel});
    if (!Revealed) {
        Out.Write({Area.X + 1, Area.Y + 1}, "rolling ...", {Shown.Quiet, Shown.Panel});
        return;
    }
    const int Block = std::max(LeastBlock, BigWidth(Thrown) + 2);
    const int Right = Area.X + Area.Width - 1 - (Of.Big ? Block + 1 : 0);
    for (std::size_t Each = 0; Each < Of.Lines.size(); Each++) {
        int X = Area.X + 1;
        for (const Run &One : Of.Lines[Each])
            X += tui::PutText(Out, {X, Area.Y + 1 + static_cast<int>(Each)}, One.Text, Right - X, One.Ink, Shown.Panel);
    }
    if (Of.Big) PaintBig(Out, Area, Block, Thrown, Shown);
}

}  // namespace dice
