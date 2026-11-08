#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

#include "box/cxx/console.h"
#include "painter/tui/stage.h"

namespace {

struct Cell {
  std::uint8_t x;
  std::uint8_t y;
};

constexpr box::color kBackground = box::color::rgb(0x08, 0x10, 0x18);
constexpr box::color kWall = box::color::rgb(0x65, 0x78, 0x8C);
constexpr box::color kText = box::color::rgb(0xD8, 0xE6, 0xF3);
constexpr box::color kSnake = box::color::rgb(0x49, 0xDC, 0x00);
constexpr box::color kSnakeHead = box::color::rgb(0x9C, 0xF5, 0x17);
constexpr box::color kApple = box::color::rgb(0xFf, 0x23, 0x23);

constexpr std::uint8_t kCellWidth = 2;
constexpr std::chrono::milliseconds kNormalTick{150};
constexpr std::chrono::milliseconds kFastestTick{35};
constexpr std::uint8_t kAccelerationPercent = 15;

using PaintColor = painter::core::Color;

PaintColor paint_color(box::color color) {
  return PaintColor::FromRaw(color.raw());
}

painter::tui::Cell make_cell(char ch, box::color fg,
                             box::color bg = kBackground) {
  return painter::tui::Cell{ch, paint_color(fg), paint_color(bg)};
}

painter::tui::Cell make_block(box::color color) {
  return make_cell(' ', color, color);
}

bool same(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }

enum class Direction { up, right, down, left };

bool opposite(Direction a, Direction b) {
  return (a == Direction::up && b == Direction::down) ||
         (a == Direction::right && b == Direction::left) ||
         (a == Direction::down && b == Direction::up) ||
         (a == Direction::left && b == Direction::right);
}

enum class InputResult { ignored, accelerated, turned, restarted, quit };

class SnakeGame {
public:
  explicit SnakeGame(painter::tui::Stage &Screen)
      : Screen_(Screen), frame_(Screen.FrameBuffer()),
        width_(static_cast<std::uint8_t>(Screen.Size().Width / kCellWidth)),
        height_(static_cast<std::uint8_t>(Screen.Size().Height - 1)),
        screen_width_(static_cast<std::uint8_t>(Screen.Size().Width)),
        rng_(seed()) {
    reset();
  }

  void reset() {
    snake_.clear();
    direction_ = Direction::right;
    next_direction_ = direction_;
    tick_ = kNormalTick;
    game_over_ = false;

    const Cell center{static_cast<std::uint8_t>(width_ / 2),
                      static_cast<std::uint8_t>(height_ / 2)};
    snake_.push_back(Cell{static_cast<std::uint8_t>(center.x - 2), center.y});
    snake_.push_back(Cell{static_cast<std::uint8_t>(center.x - 1), center.y});
    snake_.push_back(center);
    place_apple();
  }

  bool game_over() const { return game_over_; }

  InputResult handle_key(const painter::tui::KeyEvent &Key) {
    if (Key.Scancode == painter::tui::KeyCodes::Escape ||
        Key.Ascii == painter::tui::KeyCodes::Escape)
      return InputResult::quit;

    if (game_over_) {
      const char ch = Key.Ascii;
      if (ch == painter::tui::KeyCodes::Enter || ch == '\r' || ch == ' ') {
        reset();
        render();
        return InputResult::restarted;
      }
      return InputResult::ignored;
    }

    Direction requested;
    if (!decode_direction(Key, requested))
      return InputResult::ignored;

    if (requested == direction_ && requested == next_direction_) {
      accelerate();
      return InputResult::accelerated;
    }

    // A snake can turn once per movement step. Keeping one pending turn makes
    // rapid key sequences deterministic and prevents two turns from becoming
    // an accidental 180-degree reversal before the next frame.
    if (next_direction_ != direction_ || opposite(direction_, requested))
      return InputResult::ignored;

    next_direction_ = requested;
    tick_ = kNormalTick;
    return InputResult::turned;
  }

  std::chrono::milliseconds tick_interval() const { return tick_; }

  void advance() {
    direction_ = next_direction_;
    Cell head = snake_.back();
    switch (direction_) {
    case Direction::up:
      --head.y;
      break;
    case Direction::right:
      ++head.x;
      break;
    case Direction::down:
      ++head.y;
      break;
    case Direction::left:
      --head.x;
      break;
    }

    const bool ate_apple = same(head, apple_);
    if (!ate_apple)
      snake_.erase(snake_.begin());

    if (!inside(head) || contains(head)) {
      game_over_ = true;
      return;
    }

    snake_.push_back(head);
    if (ate_apple)
      place_apple();
  }

  bool render() {
    frame_.Clear({paint_color(kText), paint_color(kBackground)});

    for (std::uint8_t y = 0; y < height_; ++y) {
      for (std::uint8_t x = 0; x < width_; ++x) {
        const bool border =
            x == 0 || y == 0 || x + 1 == width_ || y + 1 == height_;
        paint_block(x, y,
                    border && game_over_ ? kApple
                    : border             ? kWall
                                         : kBackground);
      }
      if (screen_width_ % kCellWidth != 0) {
        const bool border = y == 0 || y + 1 == height_;
        put_screen(static_cast<std::uint8_t>(screen_width_ - 1), y,
                   make_block(border && game_over_ ? kApple
                              : border             ? kWall
                                                   : kBackground));
      }
    }

    if (!game_over_)
      paint_block(apple_.x, apple_.y, kApple);
    for (std::size_t i = 0; i < snake_.size(); ++i) {
      paint_block(snake_[i].x, snake_[i].y,
                  i + 1 == snake_.size() ? kSnakeHead : kSnake);
    }

    if (game_over_) {
      draw_text(height_ / 2, "GAME OVER", kApple);
      draw_text(static_cast<std::uint8_t>(height_ / 2 + 1),
                "ENTER restart  ESC exit", kText);
    }

    return static_cast<bool>(Screen_.Present());
  }

private:
  painter::tui::Stage &Screen_;
  painter::tui::Frame &frame_;
  std::uint8_t width_;
  std::uint8_t height_;
  std::uint8_t screen_width_;
  std::vector<Cell> snake_;
  Cell apple_{};
  Direction direction_ = Direction::right;
  Direction next_direction_ = Direction::right;
  std::chrono::milliseconds tick_ = kNormalTick;
  std::uint32_t rng_;
  bool game_over_ = false;

  static std::uint32_t seed() {
    return static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  std::uint32_t random() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
  }

  void accelerate() {
    auto next = tick_.count() * (100 - kAccelerationPercent) / 100;
    if (next < kFastestTick.count())
      next = kFastestTick.count();
    tick_ = std::chrono::milliseconds(next);
  }

  void put_screen(std::uint8_t x, std::uint8_t y, painter::tui::Cell Value) {
    frame_.Put({static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(y) + 1}, Value);
  }

  void paint_block(std::uint8_t x, std::uint8_t y, box::color color) {
    const auto screen_x = static_cast<std::uint8_t>(x * kCellWidth);
    put_screen(screen_x, y, make_block(color));
    put_screen(static_cast<std::uint8_t>(screen_x + 1), y, make_block(color));
  }

  bool inside(Cell cell) const {
    return cell.x > 0 && cell.y > 0 && cell.x + 1 < width_ &&
           cell.y + 1 < height_;
  }

  bool contains(Cell cell) const {
    for (const Cell part : snake_)
      if (same(part, cell))
        return true;
    return false;
  }

  void place_apple() {
    const std::uint16_t available_width = width_ - 2;
    const std::uint16_t available_height = height_ - 2;
    const std::uint32_t available =
        static_cast<std::uint32_t>(available_width) * available_height;

    for (std::uint32_t attempt = 0; attempt < available; ++attempt) {
      const std::uint32_t n = random() % available;
      const Cell candidate{static_cast<std::uint8_t>(1 + n % available_width),
                           static_cast<std::uint8_t>(1 + n / available_width)};
      if (!contains(candidate)) {
        apple_ = candidate;
        return;
      }
    }
    apple_ = Cell{0, 0};
  }

  void draw_text(std::uint8_t row, std::string_view text, box::color color) {
    if (row >= height_)
      return;
    const std::size_t start =
        text.size() < screen_width_ ? (screen_width_ - text.size()) / 2 : 0;
    const std::size_t count =
        text.size() < screen_width_ ? text.size() : screen_width_;
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t column = start + i;
      put_screen(static_cast<std::uint8_t>(column), row,
                 make_cell(text[i], color));
    }
  }

  static bool decode_direction(const painter::tui::KeyEvent &Key,
                               Direction &direction) {
    switch (Key.Scancode) {
    case painter::tui::KeyCodes::Up:
      direction = Direction::up;
      return true;
    case painter::tui::KeyCodes::Right:
      direction = Direction::right;
      return true;
    case painter::tui::KeyCodes::Down:
      direction = Direction::down;
      return true;
    case painter::tui::KeyCodes::Left:
      direction = Direction::left;
      return true;
    default:
      break;
    }

    char ch = Key.Ascii;
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
    switch (ch) {
    case 'w':
      direction = Direction::up;
      return true;
    case 'd':
      direction = Direction::right;
      return true;
    case 's':
      direction = Direction::down;
      return true;
    case 'a':
      direction = Direction::left;
      return true;
    default:
      return false;
    }
  }
};

} // namespace

int main() {
  auto Opened = painter::tui::Stage::Open(24, 9);
  if (!Opened) {
    printf("%s\n", Opened.error().Describe("snake").c_str());
    return 1;
  }
  painter::tui::Stage Screen = std::move(*Opened);
  SnakeGame game(Screen);
  if (!game.render())
    return 1;

  auto next_tick = std::chrono::steady_clock::now() + game.tick_interval();
  for (;;) {
    if (game.game_over()) {
      const painter::tui::Event Event = Screen.Wait();
      if (Event.Kind == painter::tui::EventKind::Gone) return 1;
      if (Event.Kind == painter::tui::EventKind::Key) {
        const InputResult result = game.handle_key(Event.Key);
        if (result == InputResult::quit)
          return 0;
        if (result == InputResult::restarted)
          next_tick = std::chrono::steady_clock::now() + game.tick_interval();
      }
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_tick) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(next_tick -
                                                                now);
      const auto timeout = static_cast<std::uint32_t>(
          remaining.count() < 1 ? 1 : remaining.count());
      const painter::tui::Event Event = Screen.Wait(timeout);
      if (Event.Kind == painter::tui::EventKind::Gone) return 1;
      if (Event.Kind == painter::tui::EventKind::Key) {
        const InputResult result = game.handle_key(Event.Key);
        if (result == InputResult::quit)
          return 0;
        if (result == InputResult::restarted)
          next_tick = std::chrono::steady_clock::now() + game.tick_interval();
      }
    }

    const auto after_wait = std::chrono::steady_clock::now();
    if (after_wait >= next_tick) {
      game.advance();
      if (!game.render())
        return 1;
      next_tick = after_wait + game.tick_interval();
    }
  }
}
