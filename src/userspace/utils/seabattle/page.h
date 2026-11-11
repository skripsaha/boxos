/* page.h — a page of the sea battle: one screen that is drawn, moves of itself
 * and answers keys. Every page paints in the three layers there are — pixels,
 * round shapes, words — and a page that paints otherwise says so in Paint. */
#ifndef SEABATTLE_PAGE_H
#define SEABATTLE_PAGE_H

#include <cstdint>
#include <memory>

#include "painter/tui/fine_canvas.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "context.h"

namespace seabattle {

class Page {
public:
    explicit Page(Context &Shared) : Ctx(Shared) {}
    virtual ~Page() = default;
    Page(const Page &) = delete;
    Page &operator=(const Page &) = delete;

    // Time has passed: what moves of itself moves.
    virtual void Step(std::uint32_t ElapsedMs) { (void)ElapsedMs; }
    virtual void Paint(painter::tui::Frame &Out);
    virtual void Key(const painter::tui::KeyEvent &Pressed) = 0;
    // How soon the page is drawn again when no key comes.
    virtual std::uint32_t FrameMs() const { return 125; }

protected:
    virtual void Pixels(Canvas &Out) = 0;
    virtual void Shapes(painter::tui::FineCanvas &Out) { (void)Out; }
    virtual void Words(painter::tui::Frame &Out) = 0;

    Context &Ctx;
};

std::unique_ptr<Page> MakePage(PageId Which, Context &Shared);

}  // namespace seabattle

#endif
