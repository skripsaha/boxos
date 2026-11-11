/* desk.cpp — the casino floor (desk.h). */

#include "desk.h"

#include <format>
#include <utility>

#include "box/clock.h"
#include "common.h"
#include "ornament.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

namespace casino {

namespace {

namespace tui = painter::tui;

// While nothing moves the glass waits this long for a key, and so for a
// changed setting, before it looks again.
constexpr std::uint32_t IdleMs = 500;

constexpr const char *TableNames[TableCount] = {"Roulette", "Slots", "Blackjack"};

}  // namespace

Desk::Desk(tui::Stage Opened, Sitting Start)
    : Screen(std::move(Opened)), Ctx(ReadLook()), Wheel(Ctx), Machine(Ctx), Cards(Ctx),
      Tables{&Wheel, &Machine, &Cards}, Seat(static_cast<int>(Start))
{
}

void Desk::Run()
{
    Screen.HearEnd();
    Refill();
    bool Drawn = false;
    for (;;) {
        if (Watch.changed()) {
            Ctx.Shown = ReadLook();
            Ctx.Revision++;
            Drawn = false;
        }
        const std::uint64_t Now = clock_uptime_us();
        Tables[Seat]->Advance(Now);
        if (!Tables[Seat]->Busy()) Refill();
        const std::uint32_t Frame = Tables[Seat]->FrameMs(Now);
        if (!Drawn || Frame > 0) {
            Paint(Now);
            if (!Screen.Present()) break;
            Drawn = true;
        }

        const tui::Event Heard = Screen.Wait(Frame > 0 ? Frame : IdleMs);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) break;
        if (Heard.Kind == tui::EventKind::Key) {
            if (!OnKey(Heard.Key, clock_uptime_us())) break;
            Drawn = false;
        }
    }
    for (Table *One : Tables) One->Leave();
    Ctx.Chips.Save();
}

// A player with nothing is given a fresh stack, said after what was said.
void Desk::Refill()
{
    if (const std::optional<std::string> Words = Ctx.Chips.Refilled(Ctx.Shown))
        Ctx.Say(Ctx.Notice.empty() ? *Words : Ctx.Notice + "  " + *Words, Mood::Good);
}

void Desk::Sit(int Which)
{
    if (Which == Seat) return;
    if (Tables[Seat]->Busy()) {
        Ctx.Say("Wait until the round is over before going to the other table.");
        return;
    }
    Tables[Seat]->Leave();
    Seat = Which;
    Ctx.Notice.clear();
}

bool Desk::OnKey(const tui::KeyEvent &Key, std::uint64_t NowUs)
{
    const bool Plain = !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
    if (Help || Paying) {
        Help = Paying = false;
        return true;
    }
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    if (KeyChordMatches(&Ctx.Shown.Keys[KeyHelp], &Raw)) {
        Help = true;
        return true;
    }
    if (KeyChordMatches(&Ctx.Shown.Keys[KeyPay], &Raw)) {
        Paying = true;
        return true;
    }
    if (KeyChordMatches(&Ctx.Shown.Keys[KeyTable], &Raw) || (Plain && Key.Ascii == tui::KeyCodes::Tab)) {
        Sit((Seat + 1) % TableCount);
        return true;
    }
    if (Plain && Key.Ascii == tui::KeyCodes::Escape) return false;
    if (Plain && Key.Ascii >= '1' && Key.Ascii < '1' + TableCount) {
        Sit(Key.Ascii - '1');
        return true;
    }
    Tables[Seat]->OnKey(Key, NowUs);
    return true;
}

// The casino's name and a tab for each table, the one in front lit.
void Desk::PaintHeader(tui::Frame &Out) const
{
    const Look          &Shown = Ctx.Shown;
    const tui::TextStyle Bar{Shown.Text, Shown.Page};
    Out.Write({1, 0}, "CASINO", tui::TextStyle{Shown.Gold, Shown.Page});
    std::int32_t Col = 8;
    for (int At = 0; At < TableCount; At++) {
        const std::string Tab = std::format(" {} {} ", At + 1, TableNames[At]);
        if (At > 0) Out.Put({Col++, 0}, tui::Glyphs::RuleDown, tui::TextStyle{Shown.Quiet, Shown.Page});
        if (At == Seat) {
            Out.Write({Col, 0}, Tab, tui::TextStyle{Shown.Page, Shown.Gold});
        } else {
            Out.Write({Col, 0}, Tab, Bar);
            Out.Put({Col + 1, 0}, static_cast<char>('1' + At), tui::TextStyle{Shown.Gold, Shown.Page});
        }
        Col += static_cast<std::int32_t>(Tab.size());
    }
}

// The line said, and the chips on the right of it.
void Desk::PaintNotice(tui::Frame &Out) const
{
    const std::int32_t Row   = Out.Size().Height - 2;
    const std::string &Fault = Ctx.Chips.Trouble();
    const Look        &Shown = Ctx.Shown;
    const std::string  Words = Fault.empty() ? Ctx.Notice : "The chips are not kept: " + Fault;
    const Color        Ink   = !Fault.empty() || Ctx.Feeling == Mood::Bad ? Shown.Lost : Ctx.Feeling == Mood::Good ? Shown.Won : Shown.Text;
    const std::string  Purse = Counted(Ctx.Chips.Now().Chips);
    const auto         Wide  = static_cast<std::int32_t>(Purse.size()) + 6;
    Out.Write({1, Row}, tui::Shortened(Words, static_cast<std::size_t>(std::max(0, Out.Size().Width - 3 - Wide))), tui::TextStyle{Ink, Shown.Page});
    Out.Write({Out.Size().Width - 1 - Wide, Row}, "chips ", tui::TextStyle{Shown.Quiet, Shown.Page});
    Out.Write({Out.Size().Width - 1 - Wide + 6, Row}, Purse, tui::TextStyle{Shown.Gold, Shown.Page});
}

void Desk::PaintHelp(tui::Frame &Out) const
{
    constexpr std::size_t KeysWide = 18;
    const Look           &Shown    = Ctx.Shown;
    const std::vector<std::string> Own = Tables[Seat]->Keys();
    const std::vector<std::string> Common = {
        Entry(std::format("tab, 1 2 3, {}", Spelled(Shown.Keys[KeyTable])), "go to another table"),
        Entry(Spelled(Shown.Keys[KeyPay]), "the pay table and the odds"),
        Entry("esc", "leave: the chips are kept for next time")};
    const Color          Back = Color::Rgb(0x10, 0x10, 0x10);
    const std::int32_t   Wide = 70, High = static_cast<std::int32_t>(Own.size() + Common.size()) + 5;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, std::max(1, (Out.Size().Height - High) / 2), Wide, High};
    Box(Out, Area, Shown.Gold, Back, "Keys", Shown.Gold);
    std::int32_t Row = Area.Y + 2;
    const auto   Put = [&](const std::string &Line) {
        Out.Write({Area.X + 3, Row}, Line.substr(0, KeysWide), tui::TextStyle{Shown.Gold, Back});
        if (Line.size() > KeysWide) Out.Write({Area.X + 3 + static_cast<std::int32_t>(KeysWide), Row}, Line.substr(KeysWide), tui::TextStyle{Shown.Text, Back});
        Row++;
    };
    for (const std::string &Line : Own) Put(Line);
    BoxRule(Out, Area, Row++, Shade(Shown.Gold, 0.6f), Back);
    for (const std::string &Line : Common) Put(Line);
}

void Desk::Paint(std::uint64_t NowUs)
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear(tui::TextStyle{Ctx.Shown.Page, Ctx.Shown.Page});
    Tables[Seat]->Paint(Out, NowUs);
    PaintHeader(Out);
    PaintNotice(Out);
    if (Paying) Tables[Seat]->PaintPay(Out);
    if (Help) PaintHelp(Out);
}

}  // namespace casino
