#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "box/clock.h"
#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"

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

TextCell block(box::color color) {
  TextCell cell{};
  cell.ch = ' ';
  cell.fg = cell.bg = color.raw();
  return cell;
}

class BigClock {
public:
  explicit BigClock(box::vga::dimensions size)
      : size_(size), frame_(static_cast<std::size_t>(size.rows) * size.cols),
        block_width_(block_width(size.cols)),
        block_height_(block_height(size.rows)),
        origin_x_((size.cols - kWidth * block_width_) / 2),
        origin_y_((size.rows - kHeight * block_height_) / 2) {}

  ~BigClock() {
    box::vga::clear(kBackground);
    box::vga::move_cursor(0, 0);
  }

  bool update() {
    BoxTime now{};
    if (clock_boxtime_local(&now) != OK)
      return false;
    const unsigned time = now.hour * 60u + now.minute;
    if (time == displayed_)
      return true;
    std::fill(frame_.begin(), frame_.end(), block(kBackground));
    digit(now.hour / 10, 0);
    digit(now.hour % 10, 6);
    square(12, 2, kDigitsColor);
    square(12, 4, kDigitsColor);
    digit(now.minute / 10, 14);
    digit(now.minute % 10, 20);
    if (vga_paint(0, 0, size_.rows, size_.cols, frame_.data()) != OK)
      return false;
    displayed_ = time;
    return true;
  }

private:
  box::vga::dimensions size_;
  std::vector<TextCell> frame_;
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
        frame_[row * size_.cols + col] = block(color);
  }

  void digit(unsigned number, unsigned left) {
    for (unsigned row = 0; row < kHeight; ++row)
      for (unsigned col = 0; col < 5; ++col)
        if (kDigits[number][row] & (1u << (4 - col)))
          square(left + col, row, kDigitsColor);
  }
};

int run() {
  const auto size = box::vga::size();
  if (size.rows < kHeight || size.cols < kWidth) {
    std::puts("bigclock: requires at least 25x7 console cells");
    return 1;
  }
  BigClock clock(size);
  box::key_stream keyboard;
  for (;;) {
    if (!clock.update())
      return 1;
    const auto key = keyboard.wait(250);
    if (key && key->ch() == KEY_ESCAPE)
      return 0;
  }
}

} // namespace

int main() { return run(); }
