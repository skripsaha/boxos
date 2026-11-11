/* desk.cpp — the casino floor (desk.h). */

#include "desk.h"

#include <format>
#include <utility>

#include "box/clock.h"
#include "common.h"
#include "painter/tui/text.h"

namespace casino {

namespace {

namespace tui = painter::tui;

// While nothing moves the glass waits this long for a key, and so for a
// changed setting, before it looks again.
constexpr std::uint32_t IdleMs = 500;

}  // namespace

Desk::Desk(tui::Stage Opened, Sitting Start)
    : Screen(std::move(Opened)), Ctx(ReadLook()), Wheel(Ctx), Machine(Ctx),
      Seat(Start == Sitting::Slots ? static_cast<Table *>(&Machine) : static_cast<Table *>(&Wheel))
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
        Seat->Advance(Now);
        if (!Seat->Busy()) Refill();
        const std::uint32_t Frame = Seat->FrameMs(Now);
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
    Wheel.Leave();
    Machine.Leave();
    Ctx.Chips.Save();
}

// A player with nothing is given a fresh stack, said after what was said.
void Desk::Refill()
{
    if (const std::optional<std::string> Words = Ctx.Chips.Refilled(Ctx.Shown))
        Ctx.Say(Ctx.Notice.empty() ? *Words : Ctx.Notice + "  " + *Words, Mood::Good);
}

void Desk::Change()
{
    if (Seat->Busy()) {
        Ctx.Say("Wait until the round is over before going to the other table.");
        return;
    }
    Seat->Leave();
    Seat = Seat == &Wheel ? static_cast<Table *>(&Machine) : static_cast<Table *>(&Wheel);
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
        Change();
        return true;
    }
    if (Plain && Key.Ascii == tui::KeyCodes::Escape) return false;
    if (Plain && (Key.Ascii == '1' || Key.Ascii == '2')) {
        Table *Wanted = Key.Ascii == '1' ? static_cast<Table *>(&Wheel) : static_cast<Table *>(&Machine);
        if (Wanted != Seat) Change();
        return true;
    }
    Seat->OnKey(Key, NowUs);
    return true;
}

void Desk::PaintHeader(tui::Frame &Out) const
{
    const Look        &Shown = Ctx.Shown;
    const tui::TextStyle Bar{Shown.Text, Shown.Page};
    Out.Write({1, 0}, "CASINO", tui::TextStyle{Shown.Gold, Shown.Page});
    const bool OnWheel = Seat == &Wheel;
    const tui::TextStyle Chosen{Shown.Page, Shown.Gold};
    Out.Write({9, 0}, " 1 Roulette ", OnWheel ? Chosen : Bar);
    Out.Write({22, 0}, " 2 Slots ", OnWheel ? Bar : Chosen);
    Out.Write({33, 0}, std::format("chips {}", Counted(Ctx.Chips.Now().Chips)), Bar);
}

void Desk::PaintNotice(tui::Frame &Out) const
{
    const std::int32_t Row   = Out.Size().Height - 2;
    const std::string &Fault = Ctx.Chips.Trouble();
    const Look        &Shown = Ctx.Shown;
    const std::string  Words = Fault.empty() ? Ctx.Notice : "The chips are not kept: " + Fault;
    const Color        Ink   = !Fault.empty() || Ctx.Feeling == Mood::Bad ? Shown.Lost : Ctx.Feeling == Mood::Good ? Shown.Won : Shown.Text;
    Out.Write({1, Row}, tui::Shortened(Words, static_cast<std::size_t>(Out.Size().Width - 2)), tui::TextStyle{Ink, Shown.Page});
}

void Desk::PaintHelp(tui::Frame &Out) const
{
    std::vector<std::string> Lines = Seat->Keys();
    Lines.push_back(Entry(std::format("tab, 1, 2, {}", Spelled(Ctx.Shown.Keys[KeyTable])), "the other table"));
    Lines.push_back(Entry(Spelled(Ctx.Shown.Keys[KeyPay]), "the pay table and the odds"));
    Lines.push_back(Entry("esc", "leave: the chips are kept for next time"));
    const Color        Back = Color::Rgb(0x10, 0x10, 0x10);
    const std::int32_t Wide = 68, High = static_cast<std::int32_t>(Lines.size()) + 4;
    const painter::core::Rect Area{(Out.Size().Width - Wide) / 2, std::max(1, (Out.Size().Height - High) / 2), Wide, High};
    Box(Out, Area, Ctx.Shown.Gold, Back, "Keys", Ctx.Shown.Gold);
    for (std::size_t At = 0; At < Lines.size(); At++)
        Out.Write({Area.X + 3, Area.Y + 2 + static_cast<std::int32_t>(At)}, Lines[At], tui::TextStyle{Ctx.Shown.Text, Back});
}

void Desk::Paint(std::uint64_t NowUs)
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear(tui::TextStyle{Ctx.Shown.Page, Ctx.Shown.Page});
    Seat->Paint(Out, NowUs);
    PaintHeader(Out);
    PaintNotice(Out);
    if (Paying) Seat->PaintPay(Out);
    if (Help) PaintHelp(Out);
}

}  // namespace casino
