/* app.cpp — the loop (app.h) and the way a page is laid on the glass. */

#include "app.h"

#include "box/clock.h"

#include "page.h"

namespace seabattle {

namespace tui = painter::tui;

void Page::Paint(tui::Frame &Out)
{
    Pixels(Ctx.Picture);
    Out.PutPixels(Ctx.Picture.All());
    tui::FineCanvas Fine(Out);
    Shapes(Fine);
    Words(Out);
}

void Run(Context &Shared, PageId First)
{
    Shared.Screen.HearEnd();
    std::unique_ptr<Page> Shown = MakePage(First, Shared);
    std::uint64_t         Last = clock_uptime_ms();
    for (;;) {
        if (Shared.Watch.changed()) Shared.Chosen = Adapted(ReadLook(), Shared.Text());
        if (const std::optional<PageId> Next = Shared.TakeAsked()) {
            if (*Next == PageId::Quit) break;
            Shown = MakePage(*Next, Shared);
        }
        Shared.Now = clock_uptime_ms();
        Shown->Step(static_cast<std::uint32_t>(Shared.Now - Last));
        Last = Shared.Now;
        Shared.Screen.FrameBuffer().SetCaret(std::nullopt);
        Shown->Paint(Shared.Screen.FrameBuffer());
        if (!Shared.Screen.Present()) break;

        tui::Event Heard = Shared.Screen.Wait(Shown->FrameMs());
        while (Heard.Kind == tui::EventKind::Key) {
            Shared.Chance.Stir(clock_uptime_us());
            Shown->Key(Heard.Key);
            if (Shared.Asking()) break;
            Heard = Shared.Screen.Poll();
        }
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) break;
    }
}

}  // namespace seabattle
