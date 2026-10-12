#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"
#include "tree.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr box::color kBackground = box::color::rgb(0x08, 0x0C, 0x0A);
constexpr box::color kPot = box::color::rgb(0xC8, 0x9B, 0x80);
constexpr std::uint32_t kStepMs = 30;
constexpr bonsai::Parameters kParameters{32, 5};
constexpr std::uint32_t kColors[] = {0x398C38, 0x936441, 0x9AED5C, 0xE8BB78};
constexpr std::string_view kLeaves = "&+*";

TextCell cell(char glyph, box::color foreground) {
  TextCell result{};
  result.ch = glyph;
  result.fg = foreground.raw();
  result.bg = kBackground.raw();
  return result;
}

class Bonsai {
public:
  explicit Bonsai(box::vga::dimensions size)
      : size_(size), frame_(static_cast<std::size_t>(size.rows) * size.cols),
        wood_(frame_.size()) { restart(); }

  ~Bonsai() {
    box::vga::clear(kBackground);
    box::vga::move_cursor(0, 0);
  }

  bool paint() const {
    return vga_paint(0, 0, size_.rows, size_.cols, frame_.data()) == OK;
  }

  bool finished() const { return next_mark_ == tree_.marks.size(); }

  void restart() {
    const auto now = static_cast<std::uint32_t>(
        Clock::now().time_since_epoch().count());
    tree_ = bonsai::generate(size_.rows - 4, size_.cols - 4,
                             now + generation_++, kParameters);
    std::fill(frame_.begin(), frame_.end(), cell(' ', kBackground));
    std::fill(wood_.begin(), wood_.end(), false);
    next_mark_ = 0;
    growth_ = 0;
    draw_pot(size_.rows - 4, tree_.root_col + 2);
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
  box::vga::dimensions size_;
  std::vector<TextCell> frame_;
  std::vector<bool> wood_;
  bonsai::Tree tree_;
  std::size_t next_mark_ = 0;
  std::uint32_t generation_ = 0;
  float growth_ = 0;

  void draw(const bonsai::Mark &mark) {
    const std::size_t index = (mark.row + 1) * size_.cols + mark.col + 2;
    const bool wood = mark.material == bonsai::Material::wood;
    if (index >= frame_.size() || (!wood && wood_[index]))
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
    frame_[index] = cell(glyph, color);
  }

  void draw_pot(unsigned top, unsigned root) {
    const unsigned center = std::min(root, static_cast<unsigned>(size_.cols - 1));
    const unsigned half = std::min(static_cast<unsigned>(size_.cols / 6),
                                   std::min(center, static_cast<unsigned>(size_.cols - center - 1)));
    for (unsigned offset = 0; offset < 2 * half + 1; ++offset) {
      const unsigned col = center - half + offset;
      frame_[(top + 1) * size_.cols + col] = cell('_', kPot);
      if (offset > 1 && offset + 2 < 2 * half + 1)
        frame_[(top + 3) * size_.cols + col] = cell('_', kPot);
    }
    frame_[(top + 2) * size_.cols + center - half] = cell('\\', kPot);
    frame_[(top + 2) * size_.cols + center + half] = cell('/', kPot);
    frame_[(top + 3) * size_.cols + center - half + 1] = cell('\\', kPot);
    frame_[(top + 3) * size_.cols + center + half - 1] = cell('/', kPot);
  }
};

int run() {
  const auto size = box::vga::size();
  if (size.rows < 16 || size.cols < 32)
    return 1;
  Bonsai tree(size);
  box::key_stream keyboard;
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
    const auto key = keyboard.wait(paused || tree.finished()
                                       ? 0
                                       : static_cast<std::uint32_t>(
                                             std::max(1LL, remaining)));
    if (!key)
      continue;
    if (key->ch() == KEY_ESCAPE)
      return 0;
    if (key->ch() == ' ') {
      paused = !paused;
    } else if (key->ch() == 'r' || key->ch() == 'R') {
      tree.restart();
      paused = false;
      deadline = Clock::now() + std::chrono::milliseconds{kStepMs};
    } else if (key->ch() == '\n' || key->ch() == '\r') {
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
