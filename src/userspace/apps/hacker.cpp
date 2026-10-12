#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"

namespace {

constexpr std::chrono::milliseconds kFrame{55};
constexpr char kGlyphs[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "@#$%&*+-=<>?";

constexpr box::color kBackground = box::color::rgb(0x00, 0x00, 0x00);
constexpr box::color kHead = box::color::rgb(0xD8, 0xFF, 0xD8);
constexpr box::color kBright = box::color::rgb(0x38, 0xFF, 0x58);
constexpr box::color kGreen = box::color::rgb(0x00, 0xC8, 0x38);
constexpr box::color kDim = box::color::rgb(0x00, 0x58, 0x20);

struct Drop {
  std::int16_t head;
  std::uint8_t length;
  std::uint8_t speed;
  std::uint8_t delay;
};

TextCell make_cell(char ch, box::color fg) {
  TextCell cell{};
  cell.fg = fg.raw();
  cell.bg = kBackground.raw();
  cell.ch = ch;
  return cell;
}

class MatrixRain {
public:
  MatrixRain(std::uint8_t rows, std::uint8_t cols)
      : rows_(rows), cols_(cols), frame_(static_cast<std::size_t>(rows) * cols),
        drops_(cols), rng_(seed()) {
    for (Drop &drop : drops_)
      reset(drop, true);
  }

  void step() {
    for (Drop &drop : drops_) {
      if (drop.delay != 0) {
        --drop.delay;
        continue;
      }

      drop.head = static_cast<std::int16_t>(drop.head + drop.speed);
      if (drop.head - drop.length >= rows_)
        reset(drop, false);
    }
  }

  bool render() {
    for (TextCell &cell : frame_)
      cell = make_cell(' ', kBackground);

    for (std::uint8_t x = 0; x < cols_; ++x) {
      const Drop &drop = drops_[x];
      for (std::uint8_t tail = 0; tail < drop.length; ++tail) {
        const std::int16_t y =
            static_cast<std::int16_t>(drop.head - tail);
        if (y < 0 || y >= rows_)
          continue;

        frame_[index(x, static_cast<std::uint8_t>(y))] =
            make_cell(glyph(), color_for(tail, drop.length));
      }
    }

    return vga_paint(0, 0, rows_, cols_, frame_.data()) == 0;
  }

private:
  std::uint8_t rows_;
  std::uint8_t cols_;
  std::vector<TextCell> frame_;
  std::vector<Drop> drops_;
  std::uint32_t rng_;

  static std::uint32_t seed() {
    return static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  std::uint32_t random() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
  }

  std::uint32_t random_below(std::uint32_t limit) {
    return limit == 0 ? 0 : random() % limit;
  }

  void reset(Drop &drop, bool initial) {
    const std::uint8_t max_length = rows_ > 10 ? rows_ / 2 : 5;
    drop.length = static_cast<std::uint8_t>(
        2 + random_below(max_length > 2 ? max_length - 1 : 1));
    if (drop.length > rows_)
      drop.length = rows_;
    drop.speed = static_cast<std::uint8_t>(1 + random_below(2));
    drop.delay = static_cast<std::uint8_t>(random_below(5));

    const std::uint32_t span = rows_ + drop.length;
    if (initial) {
      drop.head = static_cast<std::int16_t>(random_below(span)) - drop.length;
    } else {
      drop.head = static_cast<std::int16_t>(-drop.length) -
                  static_cast<std::int16_t>(random_below(rows_ / 2 + 3));
    }
  }

  std::size_t index(std::uint8_t x, std::uint8_t y) const {
    return static_cast<std::size_t>(y) * cols_ + x;
  }

  char glyph() {
    return kGlyphs[random_below(sizeof(kGlyphs) - 1)];
  }

  static box::color color_for(std::uint8_t tail, std::uint8_t length) {
    if (tail == 0)
      return kHead;
    if (tail == 1)
      return kBright;
    if (static_cast<std::uint16_t>(tail) * 3 < length * 2)
      return kGreen;
    return kDim;
  }
};

bool is_exit_key(const box::key &key) {
  return key.ch() == KEY_ESCAPE || key.ch() == 'q' || key.ch() == 'Q';
}

} // namespace

int main() {
  const auto dimensions = box::vga::size();
  if (dimensions.rows == 0 || dimensions.cols == 0) {
    printf("hacker: screen is unavailable\n");
    return 1;
  }

  MatrixRain rain(dimensions.rows, dimensions.cols);
  if (!rain.render())
    return 1;

  box::key_stream keyboard;
  auto next_frame = std::chrono::steady_clock::now() + kFrame;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
      const auto timeout = static_cast<std::uint32_t>(
          remaining.count() < 1 ? 1 : remaining.count());
      if (const auto key = keyboard.wait(timeout); key && is_exit_key(*key))
        break;
      continue;
    }

    rain.step();
    if (!rain.render())
      break;
    next_frame += kFrame;
    if (next_frame < std::chrono::steady_clock::now())
      next_frame = std::chrono::steady_clock::now() + kFrame;
  }

  box::vga::clear(kBackground);
  box::vga::move_cursor(0, 0);
  return 0;
}
