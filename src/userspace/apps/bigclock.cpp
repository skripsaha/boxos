#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "box/clock.h"
#include "box/cxx/console.h"
#include "painter/tui/stage.h"

namespace {

constexpr box::color kBackground = box::color::rgb(0x05, 0x07, 0x0B);
constexpr box::color kDigitsColor = box::color::rgb(0xF6, 0xF2, 0xE9);
constexpr unsigned kWidth = 25;
constexpr unsigned kHeight = 7;
constexpr std::array<std::array<std::uint8_t, kHeight>, 10> kDigits{{
    {0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111},
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111},
    {0b11111, 0b00001, 0b00001, 0b11111, 0b10000, 0b10000, 0b11111},
    {0b11111, 0b00001, 0b00001, 0b11111, 0b00001, 0b00001, 0b11111},
    {0b10001, 0b10001, 0b10001, 0b11111, 0b00001, 0b00001, 0b00001},
    {0b11111, 0b10000, 0b10000, 0b11111, 0b00001, 0b00001, 0b11111},
    {0b11111, 0b10000, 0b10000, 0b11111, 0b10001, 0b10001, 0b11111},
    {0b11111, 0b00001, 0b00001, 0b00001, 0b00001, 0b00001, 0b00001},
    {0b11111, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b11111},
    {0b11111, 0b10001, 0b10001, 0b11111, 0b00001, 0b00001, 0b11111},
}};

using PaintColor = painter::core::Color;

PaintColor paint_color(box::color color) {
  return PaintColor::FromRaw(color.raw());
}

painter::tui::Cell block(box::color color) {
  const PaintColor Ink = paint_color(color);
  return painter::tui::Cell{' ', Ink, Ink};
}

class BigClock {
public:
  explicit BigClock(painter::tui::Stage &Screen)
      : Screen_(Screen), frame_(Screen.FrameBuffer()),
        rows_(static_cast<unsigned>(Screen.Size().Height)),
        cols_(static_cast<unsigned>(Screen.Size().Width)),
        block_width_(block_width(cols_)), block_height_(block_height(rows_)),
        origin_x_((cols_ - kWidth * block_width_) / 2),
        origin_y_((rows_ - kHeight * block_height_) / 2) {}

  bool update() {
    BoxTime now{};
    if (clock_boxtime_local(&now) != OK)
      return false;
    const unsigned time = now.hour * 60u + now.minute;
    if (time == displayed_)
      return true;
    frame_.Clear({paint_color(kBackground), paint_color(kBackground)});
    digit(now.hour / 10, 0);
    digit(now.hour % 10, 6);
    square(12, 2, kDigitsColor);
    square(12, 4, kDigitsColor);
    digit(now.minute / 10, 14);
    digit(now.minute % 10, 20);
    if (!Screen_.Present()) return false;
    displayed_ = time;
    return true;
  }

private:
  painter::tui::Stage &Screen_;
  painter::tui::Frame &frame_;
  unsigned rows_;
  unsigned cols_;
  unsigned block_width_;
  unsigned block_height_;
  unsigned origin_x_;
  unsigned origin_y_;
  unsigned displayed_ = 24 * 60;

  static unsigned block_width(unsigned cols) {
    const unsigned margin = std::max(4u, cols / 6);
    const unsigned usable = cols > margin * 2 ? cols - margin * 2 : cols;
    return std::max(1u, usable / kWidth);
  }

  static unsigned block_height(unsigned rows) {
    const unsigned margin = std::max(2u, rows / 16);
    const unsigned usable = rows > margin * 2 ? rows - margin * 2 : rows;
    return std::max(1u, usable / kHeight);
  }

  void square(unsigned x, unsigned y, box::color color) {
    const unsigned left = origin_x_ + x * block_width_;
    const unsigned top = origin_y_ + y * block_height_;
    for (unsigned row = top; row < top + block_height_; ++row)
      for (unsigned col = left; col < left + block_width_; ++col)
        frame_.Put({static_cast<std::int32_t>(col), static_cast<std::int32_t>(row)},
                   block(color));
  }

  void digit(unsigned number, unsigned left) {
    for (unsigned row = 0; row < kHeight; ++row)
      for (unsigned col = 0; col < 5; ++col)
        if (kDigits[number][row] & (1u << (4 - col)))
          square(left + col, row, kDigitsColor);
  }
};

int run() {
  auto Opened = painter::tui::Stage::Open(kWidth, kHeight);
  if (!Opened) {
    std::puts(Opened.error().Describe("bigclock").c_str());
    return 1;
  }
  painter::tui::Stage Screen = std::move(*Opened);
  BigClock clock(Screen);
  for (;;) {
    if (!clock.update())
      return 1;
    const painter::tui::Event Event = Screen.Wait(250);
    if (Event.Kind == painter::tui::EventKind::Gone) return 1;
    if (Event.Kind == painter::tui::EventKind::Key &&
        Event.Key.Character == painter::tui::KeyCodes::Escape)
      return 0;
  }
}

} // namespace

int main() { return run(); }
