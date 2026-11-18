/*
 * fonts.cpp — the program's fonts, as the display keeps them for it. See
 * fonts.h.
 */

#include "fonts.h"

#include "font_opened.h"

#include <memory>
#include <mutex>
#include <vector>

#include "painter/core/system_fonts.h"

namespace painter::gui::detail {

namespace {

std::mutex       g_lock;
typecase::Fonts *g_book = nullptr;   // made at the first asking, kept for the program's life

/* A program that measures hears of every font its areas open, from its start. */
const bool g_hearing = (FontOpened = &ProgramFontsAdd, true);

/* The book, the system's faces put in it first. Under the lock. */
typecase::Fonts &Book()
{
    if (!g_book) {
        auto Made = std::make_unique<typecase::Fonts>();
        if (const std::shared_ptr<const typecase::Fonts> System = core::SystemFonts()) {
            for (const auto &Face : System->Faces()) Made->Add(Face);
            Made->SetFallback(System->Fallbacks());
            Made->SetDefault(System->Default());
        }
        g_book = Made.release();
    }
    return *g_book;
}

}  // namespace

const typecase::Fonts &ProgramFontsLocked() { return Book(); }

void ProgramFontsLock() noexcept { g_lock.lock(); }

void ProgramFontsUnlock() noexcept { g_lock.unlock(); }

void ProgramFontsAdd(std::span<const std::uint8_t> Bytes)
{
    std::expected<typecase::Font, typecase::Fault> Face = typecase::Font::Read(std::vector<std::uint8_t>(Bytes.begin(), Bytes.end()));
    if (!Face) return;   // the display reads it no more than this does
    const std::lock_guard<std::mutex> Holding(g_lock);
    Book().Add(std::move(*Face));
}

}  // namespace painter::gui::detail
