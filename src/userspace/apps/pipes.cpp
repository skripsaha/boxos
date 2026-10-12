#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"

namespace {

constexpr std::chrono::milliseconds kFrame{45};
constexpr std::chrono::seconds kResetPeriod{30};
constexpr std::uint8_t kWalkerCount = 5;
constexpr std::uint8_t kCellWidth = 2;
constexpr box::color kBackground = box::color::rgb(0x00, 0x00, 0x00);

enum class Direction : std::uint8_t { up, right, down, left };

struct Cell {
  std::uint8_t links = 0;
  std::uint8_t color = 0;
};

struct Walker {
  std::uint8_t x = 0;
  std::uint8_t y = 0;
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

box::color pipe_color(std::uint8_t index) {
  switch (index % 6) {
  case 0:
    return box::color::rgb(0x00, 0xFF, 0xFF);
  case 1:
    return box::color::rgb(0xFF, 0x00, 0xA8);
  case 2:
    return box::color::rgb(0xFF, 0xD0, 0x00);
  case 3:
    return box::color::rgb(0xB0, 0xFF, 0x20);
  case 4:
    return box::color::rgb(0x90, 0x00, 0xFF);
  default:
    return box::color::rgb(0xFF, 0xA0, 0x00);
  }
}

TextCell make_cell(char ch, box::color fg, box::color bg = kBackground) {
  TextCell cell{};
  cell.fg = fg.raw();
  cell.bg = bg.raw();
  cell.ch = ch;
  return cell;
}

TextCell make_block(box::color color) {
  return make_cell(' ', color, color);
}

class PipeField {
public:
  PipeField(std::uint8_t rows, std::uint8_t screen_cols)
      : rows_(rows), screen_cols_(screen_cols),
        cols_(static_cast<std::uint8_t>(screen_cols / kCellWidth)),
        cells_(static_cast<std::size_t>(rows) * cols_),
        frame_(static_cast<std::size_t>(rows) * screen_cols),
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

  bool render() {
    for (TextCell &cell : frame_)
      cell = make_cell(' ', kBackground);

    for (std::uint8_t y = 0; y < rows_; ++y) {
      for (std::uint8_t x = 0; x < cols_; ++x) {
        const Cell &cell = cells_[index(x, y)];
        if (cell.links == 0)
          continue;
        const box::color color = pipe_color(cell.color);
        const std::uint8_t screen_x =
            static_cast<std::uint8_t>(x * kCellWidth);
        for (std::uint8_t offset = 0; offset < kCellWidth; ++offset)
          frame_[screen_index(static_cast<std::uint8_t>(screen_x + offset), y)] =
              make_block(color);
      }
    }

    return vga_paint(0, 0, rows_, screen_cols_, frame_.data()) == 0;
  }

  void restart() { reset(); }

private:
  std::uint8_t rows_;
  std::uint8_t screen_cols_;
  std::uint8_t cols_;
  std::vector<Cell> cells_;
  std::vector<TextCell> frame_;
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

  std::size_t index(std::uint8_t x, std::uint8_t y) const {
    return static_cast<std::size_t>(y) * cols_ + x;
  }

  std::size_t screen_index(std::uint8_t x, std::uint8_t y) const {
    return static_cast<std::size_t>(y) * screen_cols_ + x;
  }

  bool inside(std::int16_t x, std::int16_t y) const {
    return x >= 0 && y >= 0 && x < cols_ && y < rows_;
  }

  bool empty(std::int16_t x, std::int16_t y) const {
    return inside(x, y) && cells_[index(static_cast<std::uint8_t>(x),
                                        static_cast<std::uint8_t>(y))]
                              .links == 0;
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

      walker.x = static_cast<std::uint8_t>(candidate % cols_);
      walker.y = static_cast<std::uint8_t>(candidate / cols_);
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

    const std::int16_t next_x = static_cast<std::int16_t>(walker.x) + dx(next);
    const std::int16_t next_y = static_cast<std::int16_t>(walker.y) + dy(next);
    Cell &current = cells_[index(walker.x, walker.y)];
    Cell &destination =
        cells_[index(static_cast<std::uint8_t>(next_x),
                     static_cast<std::uint8_t>(next_y))];
    current.links = static_cast<std::uint8_t>(current.links | bit(next));
    destination.links =
        static_cast<std::uint8_t>(destination.links | bit(opposite(next)));
    destination.color = walker.color;
    walker.x = static_cast<std::uint8_t>(next_x);
    walker.y = static_cast<std::uint8_t>(next_y);
    walker.direction = next;
  }

  bool can_move(const Walker &walker, Direction direction) const {
    const std::int16_t next_x = static_cast<std::int16_t>(walker.x) + dx(direction);
    const std::int16_t next_y = static_cast<std::int16_t>(walker.y) + dy(direction);
    return empty(next_x, next_y);
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

  static std::int16_t dx(Direction direction) {
    return direction == Direction::right ? 1 : direction == Direction::left ? -1 : 0;
  }

  static std::int16_t dy(Direction direction) {
    return direction == Direction::down ? 1 : direction == Direction::up ? -1 : 0;
  }

};

bool is_exit_key(const box::key &key) {
  return key.ch() == KEY_ESCAPE || key.ch() == 'q' || key.ch() == 'Q';
}

} // namespace

int main() {
  const auto dimensions = box::vga::size();
  if (dimensions.rows == 0 || dimensions.cols < kCellWidth) {
    printf("pipes: screen is unavailable\n");
    return 1;
  }

  PipeField field(dimensions.rows, dimensions.cols);
  if (!field.render())
    return 1;

  box::key_stream keyboard;
  auto next_frame = std::chrono::steady_clock::now() + kFrame;
  auto next_reset = std::chrono::steady_clock::now() + kResetPeriod;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_reset) {
      field.restart();
      if (!field.render())
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
      if (const auto key = keyboard.wait(timeout); key && is_exit_key(*key))
        break;
      continue;
    }

    field.step();
    if (!field.render())
      break;
    next_frame += kFrame;
    if (next_frame < std::chrono::steady_clock::now())
      next_frame = std::chrono::steady_clock::now() + kFrame;
  }

  box::vga::clear(kBackground);
  box::vga::move_cursor(0, 0);
  return 0;
}
