#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "painter/tui/stage.h"

namespace {

constexpr std::chrono::milliseconds kFrame{45};
constexpr std::chrono::seconds kResetPeriod{30};
constexpr std::uint8_t kWalkerCount = 5;
constexpr std::uint8_t kCellWidth = 2;
constexpr painter::core::Color kBackground = painter::core::Color::Rgb(0x00, 0x00, 0x00);

enum class Direction : std::uint8_t { up, right, down, left };

struct Cell {
  std::uint8_t links = 0;
  std::uint8_t color = 0;
};

struct Walker {
  std::int32_t x = 0;
  std::int32_t y = 0;
  Direction direction = Direction::right;
  std::uint8_t color = 0;
  bool active = false;
};

constexpr std::uint8_t bit(Direction direction) {
  return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(direction));
}

Direction opposite(Direction direction) {
  switch (direction) {
  case Direction::up:
    return Direction::down;
  case Direction::right:
    return Direction::left;
  case Direction::down:
    return Direction::up;
  case Direction::left:
    return Direction::right;
  }
  return Direction::right;
}

painter::core::Color pipe_color(std::uint8_t index) {
  switch (index % 6) {
  case 0:
    return painter::core::Color::Rgb(0x00, 0xFF, 0xFF);
  case 1:
    return painter::core::Color::Rgb(0xFF, 0x00, 0xA8);
  case 2:
    return painter::core::Color::Rgb(0xFF, 0xD0, 0x00);
  case 3:
    return painter::core::Color::Rgb(0xB0, 0xFF, 0x20);
  case 4:
    return painter::core::Color::Rgb(0x90, 0x00, 0xFF);
  default:
    return painter::core::Color::Rgb(0xFF, 0xA0, 0x00);
  }
}

class PipeField {
public:
  PipeField(std::int32_t rows, std::int32_t screen_cols)
      : rows_(rows), cols_(screen_cols / kCellWidth),
        cells_(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols_)),
        walkers_(kWalkerCount), rng_(seed()) {
    reset();
  }

  void step() {
    bool any_active = false;
    for (Walker &walker : walkers_) {
      if (!walker.active)
        continue;
      any_active = true;
      advance(walker);
    }

    for (Walker &walker : walkers_) {
      if (!walker.active)
        spawn(walker);
    }

    if (!any_active && !has_empty_cell())
      reset();
  }

  void render(painter::tui::Frame &frame) {
    frame.Clear({kBackground, kBackground});

    for (std::int32_t y = 0; y < rows_; ++y) {
      for (std::int32_t x = 0; x < cols_; ++x) {
        const Cell &cell = cells_[index(x, y)];
        if (cell.links == 0)
          continue;
        const painter::core::Color color = pipe_color(cell.color);
        frame.Fill({x * kCellWidth, y, kCellWidth, 1}, ' ', {color, color});
      }
    }
  }

  void restart() { reset(); }

private:
  std::int32_t rows_;
  std::int32_t cols_;
  std::vector<Cell> cells_;
  std::vector<Walker> walkers_;
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

  std::size_t index(std::int32_t x, std::int32_t y) const {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(cols_) +
           static_cast<std::size_t>(x);
  }

  bool inside(std::int32_t x, std::int32_t y) const {
    return x >= 0 && y >= 0 && x < cols_ && y < rows_;
  }

  bool empty(std::int32_t x, std::int32_t y) const {
    return inside(x, y) && cells_[index(x, y)].links == 0;
  }

  bool has_empty_cell() const {
    for (const Cell &cell : cells_)
      if (cell.links == 0)
        return true;
    return false;
  }

  void reset() {
    for (Cell &cell : cells_)
      cell = {};
    for (Walker &walker : walkers_) {
      walker.active = false;
      spawn(walker);
    }
  }

  void spawn(Walker &walker) {
    const std::size_t total = cells_.size();
    if (total == 0)
      return;

    const std::size_t start = random_below(static_cast<std::uint32_t>(total));
    for (std::size_t offset = 0; offset < total; ++offset) {
      const std::size_t candidate = (start + offset) % total;
      if (cells_[candidate].links != 0)
        continue;

      walker.x = static_cast<std::int32_t>(candidate % static_cast<std::size_t>(cols_));
      walker.y = static_cast<std::int32_t>(candidate / static_cast<std::size_t>(cols_));
      walker.direction = static_cast<Direction>(random_below(4));
      walker.color = static_cast<std::uint8_t>(random_below(6));
      walker.active = true;
      cells_[candidate].color = walker.color;
      return;
    }

    walker.active = false;
  }

  void advance(Walker &walker) {
    const Direction forward = walker.direction;
    const Direction left = turn_left(forward);
    const Direction right = turn_right(forward);
    const bool forward_open = can_move(walker, forward);
    Direction turns[2];
    std::uint8_t turn_count = 0;
    if (can_move(walker, left))
      turns[turn_count++] = left;
    if (can_move(walker, right))
      turns[turn_count++] = right;

    Direction next = forward;
    if (!forward_open && turn_count == 0) {
      walker.active = false;
      return;
    }
    if (!forward_open) {
      next = turns[random_below(turn_count)];
    } else if (turn_count != 0 && random_below(100) >= 80) {
      next = turns[random_below(turn_count)];
    }

    const std::int32_t next_x = walker.x + dx(next);
    const std::int32_t next_y = walker.y + dy(next);
    Cell &current = cells_[index(walker.x, walker.y)];
    Cell &destination = cells_[index(next_x, next_y)];
    current.links = static_cast<std::uint8_t>(current.links | bit(next));
    destination.links =
        static_cast<std::uint8_t>(destination.links | bit(opposite(next)));
    destination.color = walker.color;
    walker.x = next_x;
    walker.y = next_y;
    walker.direction = next;
  }

  bool can_move(const Walker &walker, Direction direction) const {
    return empty(walker.x + dx(direction), walker.y + dy(direction));
  }

  static Direction turn_left(Direction direction) {
    switch (direction) {
    case Direction::up:
      return Direction::left;
    case Direction::right:
      return Direction::up;
    case Direction::down:
      return Direction::right;
    case Direction::left:
      return Direction::down;
    }
    return Direction::left;
  }

  static Direction turn_right(Direction direction) {
    switch (direction) {
    case Direction::up:
      return Direction::right;
    case Direction::right:
      return Direction::down;
    case Direction::down:
      return Direction::left;
    case Direction::left:
      return Direction::up;
    }
    return Direction::right;
  }

  static std::int32_t dx(Direction direction) {
    return direction == Direction::right ? 1 : direction == Direction::left ? -1 : 0;
  }

  static std::int32_t dy(Direction direction) {
    return direction == Direction::down ? 1 : direction == Direction::up ? -1 : 0;
  }

};

bool is_exit_key(const painter::tui::KeyEvent &key) {
  return key.Ascii == painter::tui::KeyCodes::Escape || key.Ascii == 'q' || key.Ascii == 'Q';
}

} // namespace

int main() {
  auto opened = painter::tui::Stage::Open(kCellWidth, 1);
  if (!opened) {
    printf("%s\n", opened.error().Describe("pipes").c_str());
    return 1;
  }
  painter::tui::Stage screen = std::move(*opened);

  PipeField field(screen.Size().Height, screen.Size().Width);
  field.render(screen.FrameBuffer());
  if (!screen.Present())
    return 1;

  auto next_frame = std::chrono::steady_clock::now() + kFrame;
  auto next_reset = std::chrono::steady_clock::now() + kResetPeriod;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_reset) {
      field.restart();
      field.render(screen.FrameBuffer());
      if (!screen.Present())
        break;
      do {
        next_reset += kResetPeriod;
      } while (next_reset <= now);
    }

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

    field.step();
    field.render(screen.FrameBuffer());
    if (!screen.Present())
      break;
    next_frame += kFrame;
    if (next_frame < std::chrono::steady_clock::now())
      next_frame = std::chrono::steady_clock::now() + kFrame;
  }

  return 0;
}
