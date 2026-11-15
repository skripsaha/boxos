#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "box/cxx/console.h"
#include "painter/tui/stage.h"
#include "tree.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr box::color kBackground = box::color::rgb(0x08, 0x0C, 0x0A);
constexpr box::color kPot = box::color::rgb(0xC8, 0x9B, 0x80);
constexpr std::uint32_t kStepMs = 30;
constexpr bonsai::Parameters kParameters{32, 5};
constexpr std::uint32_t kColors[] = {0x398C38, 0x936441, 0x9AED5C, 0xE8BB78};
constexpr std::string_view kLeaves = "&+*";

using PaintColor = painter::core::Color;

painter::tui::Cell cell(char glyph, box::color foreground) {
  return painter::tui::Cell{glyph, PaintColor::FromRaw(foreground.raw()),
                            PaintColor::FromRaw(kBackground.raw())};
}

class Bonsai {
public:
  explicit Bonsai(painter::tui::Stage &Screen)
      : Screen_(Screen), frame_(Screen.FrameBuffer()),
        rows_(static_cast<unsigned>(Screen.Size().Height)),
        cols_(static_cast<unsigned>(Screen.Size().Width)),
        wood_(static_cast<std::size_t>(rows_) * cols_) { restart(); }

  bool paint() const {
    return static_cast<bool>(Screen_.Present());
  }

  bool finished() const { return next_mark_ == tree_.marks.size(); }

  void restart() {
    const auto now = static_cast<std::uint32_t>(
        Clock::now().time_since_epoch().count());
    tree_ = bonsai::generate(rows_ - 4, cols_ - 4,
                             now + generation_++, kParameters);
    frame_.Clear({PaintColor::FromRaw(kBackground.raw()),
                  PaintColor::FromRaw(kBackground.raw())});
    std::fill(wood_.begin(), wood_.end(), false);
    next_mark_ = 0;
    growth_ = 0;
    draw_pot(rows_ - 4, tree_.root_col + 2);
  }

  void step() {
    growth_ += 1.0f / (kParameters.life * 3);
    while (!finished() && tree_.marks[next_mark_].birth <= growth_)
      draw(tree_.marks[next_mark_++]);
  }

  void finish() {
    while (!finished())
      draw(tree_.marks[next_mark_++]);
  }

private:
  painter::tui::Stage &Screen_;
  painter::tui::Frame &frame_;
  unsigned rows_;
  unsigned cols_;
  std::vector<bool> wood_;
  bonsai::Tree tree_;
  std::size_t next_mark_ = 0;
  std::uint32_t generation_ = 0;
  float growth_ = 0;

  void draw(const bonsai::Mark &mark) {
    const std::size_t index = (mark.row + 1) * cols_ + mark.col + 2;
    const bool wood = mark.material == bonsai::Material::wood;
    if (index >= wood_.size() || (!wood && wood_[index]))
      return;
    wood_[index] = wood;
    const unsigned dark = wood ? 1 : 0;
    const unsigned bright = dark + 2;
    const auto a = box::color::from_raw(kColors[dark]);
    const auto b = box::color::from_raw(kColors[bright]);
    const unsigned blend = std::min<unsigned>(mark.shade, 3);
    const auto color = box::color::rgb(
        (a.r() * (3 - blend) + b.r() * blend) / 3,
        (a.g() * (3 - blend) + b.g() * blend) / 3,
        (a.b() * (3 - blend) + b.b() * blend) / 3);
    const char glyph = wood ? mark.glyph
                            : kLeaves[(mark.col + mark.row + mark.shade) %
                                      kLeaves.size()];
    frame_.Put({static_cast<std::int32_t>(mark.col + 2),
                static_cast<std::int32_t>(mark.row + 1)}, cell(glyph, color));
  }

  void draw_pot(unsigned top, unsigned root) {
    const unsigned center = std::min(root, cols_ - 1);
    const unsigned half = std::min(cols_ / 6,
                                   std::min(center, cols_ - center - 1));
    for (unsigned offset = 0; offset < 2 * half + 1; ++offset) {
      const unsigned col = center - half + offset;
      frame_.Put({static_cast<std::int32_t>(col), static_cast<std::int32_t>(top + 1)},
                  cell('_', kPot));
      if (offset > 1 && offset + 2 < 2 * half + 1)
        frame_.Put({static_cast<std::int32_t>(col), static_cast<std::int32_t>(top + 3)},
                    cell('_', kPot));
    }
    frame_.Put({static_cast<std::int32_t>(center - half), static_cast<std::int32_t>(top + 2)}, cell('\\', kPot));
    frame_.Put({static_cast<std::int32_t>(center + half), static_cast<std::int32_t>(top + 2)}, cell('/', kPot));
    frame_.Put({static_cast<std::int32_t>(center - half + 1), static_cast<std::int32_t>(top + 3)}, cell('\\', kPot));
    frame_.Put({static_cast<std::int32_t>(center + half - 1), static_cast<std::int32_t>(top + 3)}, cell('/', kPot));
  }
};

int run() {
  auto Opened = painter::tui::Stage::Open(32, 16);
  if (!Opened) {
    std::printf("%s\n", Opened.error().Describe("bonsai").c_str());
    return 1;
  }
  painter::tui::Stage Screen = std::move(*Opened);
  Bonsai tree(Screen);
  bool paused = false;
  auto deadline = Clock::now() + std::chrono::milliseconds{kStepMs};
  if (!tree.paint())
    return 1;

  for (;;) {
    if (!paused && !tree.finished() && Clock::now() >= deadline) {
      tree.step();
      deadline = Clock::now() + std::chrono::milliseconds{kStepMs};
      if (!tree.paint())
        return 1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()).count();
    const painter::tui::Event Event = Screen.Wait(paused || tree.finished()
                                                       ? 0
                                                       : static_cast<std::uint32_t>(
                                                             std::max(1LL, remaining)));
    if (Event.Kind == painter::tui::EventKind::Quiet)
      continue;
    if (Event.Kind == painter::tui::EventKind::Gone) return 1;
    if (Event.Key.Character == painter::tui::KeyCodes::Escape)
      return 0;
    if (Event.Key.Character == ' ') {
      paused = !paused;
    } else if (Event.Key.Character == 'r' || Event.Key.Character == 'R') {
      tree.restart();
      paused = false;
      deadline = Clock::now() + std::chrono::milliseconds{kStepMs};
    } else if (Event.Key.Character == painter::tui::KeyCodes::Enter || Event.Key.Character == '\r') {
      tree.finish();
    } else {
      continue;
    }
    if (!tree.paint())
      return 1;
  }
}

} // namespace

int main() { return run(); }
