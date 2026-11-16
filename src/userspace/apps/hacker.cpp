#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "painter/tui/stage.h"

namespace {

constexpr std::chrono::milliseconds kFrame{55};
constexpr char kGlyphs[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "@#$%&*+-=<>?";

constexpr painter::core::Color kBackground = painter::core::Color::Rgb(0x00, 0x00, 0x00);
constexpr painter::core::Color kHead = painter::core::Color::Rgb(0xD8, 0xFF, 0xD8);
constexpr painter::core::Color kBright = painter::core::Color::Rgb(0x38, 0xFF, 0x58);
constexpr painter::core::Color kGreen = painter::core::Color::Rgb(0x00, 0xC8, 0x38);
constexpr painter::core::Color kDim = painter::core::Color::Rgb(0x00, 0x58, 0x20);

struct Drop {
  std::int16_t head;
  std::uint8_t length;
  std::uint8_t speed;
  std::uint8_t delay;
};

class MatrixRain {
public:
  MatrixRain(std::int32_t rows, std::int32_t cols)
      : rows_(rows), cols_(cols), drops_(static_cast<std::size_t>(cols)), rng_(seed()) {
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

  void render(painter::tui::Frame &frame) {
    frame.Clear({kBackground, kBackground});

    for (std::int32_t x = 0; x < cols_; ++x) {
      const Drop &drop = drops_[static_cast<std::size_t>(x)];
      for (std::uint8_t tail = 0; tail < drop.length; ++tail) {
        const std::int32_t y = drop.head - tail;
        if (y < 0 || y >= rows_)
          continue;

        frame.Put({x, y}, glyph(), {color_for(tail, drop.length), kBackground});
      }
    }
  }

private:
  std::int32_t rows_;
  std::int32_t cols_;
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
    const std::uint32_t max_length = rows_ > 10 ? static_cast<std::uint32_t>(rows_) / 2 : 5;
    drop.length = static_cast<std::uint8_t>(
        2 + random_below(max_length > 2 ? max_length - 1 : 1));
    if (drop.length > rows_)
      drop.length = static_cast<std::uint8_t>(rows_);
    drop.speed = static_cast<std::uint8_t>(1 + random_below(2));
    drop.delay = static_cast<std::uint8_t>(random_below(5));

    const std::uint32_t span = static_cast<std::uint32_t>(rows_) + drop.length;
    if (initial) {
      drop.head = static_cast<std::int16_t>(random_below(span)) - drop.length;
    } else {
      drop.head = static_cast<std::int16_t>(-drop.length) -
                  static_cast<std::int16_t>(random_below(static_cast<std::uint32_t>(rows_) / 2 + 3));
    }
  }

  char glyph() {
    return kGlyphs[random_below(sizeof(kGlyphs) - 1)];
  }

  static painter::core::Color color_for(std::uint8_t tail, std::uint8_t length) {
    if (tail == 0)
      return kHead;
    if (tail == 1)
      return kBright;
    if (static_cast<std::uint16_t>(tail) * 3 < length * 2)
      return kGreen;
    return kDim;
  }
};

bool is_exit_key(const painter::tui::KeyEvent &key) {
  return key.Character == painter::tui::KeyCodes::Escape || key.Character == 'q' || key.Character == 'Q';
}

} // namespace

int main() {
  auto opened = painter::tui::Stage::Open(1, 1);
  if (!opened) {
    printf("%s\n", opened.error().Describe("hacker").c_str());
    return 1;
  }
  painter::tui::Stage screen = std::move(*opened);

  MatrixRain rain(screen.Size().Height, screen.Size().Width);
  rain.render(screen.FrameBuffer());
  if (!screen.Present())
    return 1;

  auto next_frame = std::chrono::steady_clock::now() + kFrame;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now);
      const auto timeout = static_cast<std::uint32_t>(
          remaining.count() < 1 ? 1 : remaining.count());
      const painter::tui::Event event = screen.Wait(timeout);
      if (event.Kind == painter::tui::EventKind::Gone)
        break;
      if (event.Kind == painter::tui::EventKind::Key && is_exit_key(event.Key))
        break;
      continue;
    }

    rain.step();
    rain.render(screen.FrameBuffer());
    if (!screen.Present())
      break;
    next_frame += kFrame;
    if (next_frame < std::chrono::steady_clock::now())
      next_frame = std::chrono::steady_clock::now() + kFrame;
  }

  return 0;
}
