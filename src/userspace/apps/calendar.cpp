#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "box/clock.h"
#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"
#include "box/vga.h"

namespace {

using Date = std::chrono::year_month_day;
using DayPoint = std::chrono::sys_days;

constexpr box::color kBackground = box::color::rgb(0x08, 0x09, 0x0D);
constexpr box::color kPanel = box::color::rgb(0x12, 0x14, 0x1B);
constexpr box::color kWhite = box::color::rgb(0xF6, 0xF2, 0xE9);
constexpr box::color kMuted = box::color::rgb(0xA8, 0xA5, 0xA0);
constexpr box::color kRed = box::color::rgb(0xF7, 0x17, 0x17);
constexpr box::color kDarkRed = box::color::rgb(0x73, 0x11, 0x11);
constexpr box::color kSelectedText = box::color::rgb(0x18, 0x0B, 0x0E);

constexpr const char *kMonths[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};

constexpr const char *kWeekdayShort[] = {"MON", "TUE", "WED", "THU",
                                         "FRI", "SAT", "SUN"};

constexpr const char *kDigits[10][7] = {
    {"01110", "11011", "11011", "11011", "11011", "11011", "01110"},
    {"00110", "01110", "00110", "00110", "00110", "00110", "11111"},
    {"01110", "11011", "00011", "00110", "01100", "11000", "11111"},
    {"01110", "11011", "00011", "00110", "00011", "11011", "01110"},
    {"00011", "00111", "01111", "11011", "11111", "00011", "00011"},
    {"11111", "11000", "11110", "00011", "00011", "11011", "01110"},
    {"00111", "01100", "11000", "11110", "11011", "11011", "01110"},
    {"11111", "00011", "00110", "01100", "01100", "01100", "01100"},
    {"01110", "11011", "11011", "01110", "11011", "11011", "01110"},
    {"01110", "11011", "11011", "01111", "00011", "00110", "11100"},
};

const char *block_glyph_row(char letter, unsigned row) {
  static constexpr const char *m[] = {"101", "111", "101", "101", "101"};
  static constexpr const char *o[] = {"111", "101", "101", "101", "111"};
  static constexpr const char *n[] = {"110", "101", "101", "101", "101"};
  static constexpr const char *t[] = {"111", "010", "010", "010", "010"};
  static constexpr const char *u[] = {"101", "101", "101", "101", "111"};
  static constexpr const char *e[] = {"111", "100", "110", "100", "111"};
  static constexpr const char *w[] = {"101", "101", "101", "111", "101"};
  static constexpr const char *d[] = {"110", "101", "101", "101", "110"};
  static constexpr const char *h[] = {"101", "101", "111", "101", "101"};
  static constexpr const char *f[] = {"111", "100", "110", "100", "100"};
  static constexpr const char *r[] = {"110", "101", "110", "101", "101"};
  static constexpr const char *i[] = {"111", "010", "010", "010", "111"};
  static constexpr const char *s[] = {"111", "100", "111", "001", "111"};
  static constexpr const char *a[] = {"010", "101", "111", "101", "101"};

  const char *const *glyph = nullptr;
  switch (letter) {
  case 'M':
    glyph = m;
    break;
  case 'O':
    glyph = o;
    break;
  case 'N':
    glyph = n;
    break;
  case 'T':
    glyph = t;
    break;
  case 'U':
    glyph = u;
    break;
  case 'E':
    glyph = e;
    break;
  case 'W':
    glyph = w;
    break;
  case 'D':
    glyph = d;
    break;
  case 'H':
    glyph = h;
    break;
  case 'F':
    glyph = f;
    break;
  case 'R':
    glyph = r;
    break;
  case 'I':
    glyph = i;
    break;
  case 'S':
    glyph = s;
    break;
  case 'A':
    glyph = a;
    break;
  default:
    return "000";
  }
  return glyph[row < 5 ? row : 0];
}

TextCell cell(char ch, box::color fg, box::color bg) {
  TextCell result{};
  result.fg = fg.raw();
  result.bg = bg.raw();
  result.ch = ch;
  return result;
}

Date date_from_boxtime(const BoxTime &time) {
  return Date{std::chrono::year{static_cast<int>(time.year)},
              std::chrono::month{time.month}, std::chrono::day{time.day}};
}

unsigned days_in_month(Date date) {
  const auto last = std::chrono::year_month_day_last{
      date.year(), std::chrono::month_day_last{date.month()}};
  return static_cast<unsigned>(last.day());
}

bool same_date(Date left, Date right) {
  return left.year() == right.year() && left.month() == right.month() &&
         left.day() == right.day();
}

std::string date_text(Date date) {
  char text[32];
  std::snprintf(
      text, sizeof(text), "%04d-%02u-%02u", static_cast<int>(date.year()),
      static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
  return text;
}

class Calendar {
public:
  Calendar(std::uint8_t rows, std::uint8_t cols)
      : rows_(rows), cols_(cols), left_width_(cols / 2),
        frame_(static_cast<std::size_t>(rows) * cols,
               cell(' ', kWhite, kBackground)) {
    read_today();
  }

  bool handle(const box::key &key) {
    if (key.ch() == KEY_ESCAPE)
      return false;
    if (!time_ok_)
      return true;

    switch (key.scancode()) {
    case KEY_LEFT:
      shift_days(-1);
      break;
    case KEY_RIGHT:
      shift_days(1);
      break;
    case KEY_UP:
      shift_days(-7);
      break;
    case KEY_DOWN:
      shift_days(7);
      break;
    default:
      switch (key.ch()) {
      case 'a':
      case 'A':
        shift_days(-1);
        break;
      case 'd':
      case 'D':
        shift_days(1);
        break;
      case 'w':
      case 'W':
        shift_days(-7);
        break;
      case 's':
      case 'S':
        shift_days(7);
        break;
      case 'q':
      case 'Q':
        shift_months(-1);
        break;
      case 'e':
      case 'E':
        shift_months(1);
        break;
      case 't':
      case 'T':
        selected_ = today_;
        break;
      default:
        return true;
      }
      break;
    }
    render();
    return true;
  }

  void render() {
    read_today();
    std::fill(frame_.begin(), frame_.end(), cell(' ', kWhite, kBackground));
    if (rows_ < 20 || cols_ < 70) {
      text(1, 2, "calendar needs at least 70x20 console cells", kRed,
           static_cast<std::uint8_t>(cols_ > 4 ? cols_ - 4 : 1));
      paint();
      return;
    }

    draw_left();
    draw_right();
    draw_footer();
    paint();
  }

  void clear() {
    box::vga::clear(kBackground);
    box::vga::move_cursor(0, 0);
  }

private:
  std::uint8_t rows_;
  std::uint8_t cols_;
  std::uint8_t left_width_;
  std::vector<TextCell> frame_;
  Date today_{};
  Date selected_{};
  bool time_ok_ = false;

  void read_today() {
    BoxTime now{};
    if (clock_boxtime_local(&now) != OK) {
      time_ok_ = false;
      return;
    }

    const Date current = date_from_boxtime(now);
    if (!time_ok_ || same_date(today_, selected_))
      selected_ = current;
    today_ = current;
    time_ok_ = true;
  }

  void shift_days(int delta) {
    selected_ = Date{DayPoint{selected_} + std::chrono::days{delta}};
  }

  void shift_months(int delta) {
    auto month = selected_.year() / selected_.month();
    month += std::chrono::months{delta};
    const unsigned day = std::min(static_cast<unsigned>(selected_.day()),
                                  days_in_month(month / std::chrono::day{1}));
    selected_ = month / std::chrono::day{day};
  }

  void put(std::uint8_t row, std::uint8_t col, char ch, box::color fg,
           box::color bg = kBackground) {
    if (row < rows_ && col < cols_)
      frame_[static_cast<std::size_t>(row) * cols_ + col] = cell(ch, fg, bg);
  }

  void text(std::uint8_t row, std::uint8_t col, const std::string &value,
            box::color fg, std::uint8_t max_width = 255,
            box::color bg = kBackground) {
    for (std::size_t i = 0; i < value.size() && i < max_width; ++i) {
      const std::uint16_t x = static_cast<std::uint16_t>(col) + i;
      if (x >= cols_)
        break;
      put(row, static_cast<std::uint8_t>(x), value[i], fg, bg);
    }
  }

  void centered(std::uint8_t row, std::uint8_t first, std::uint8_t width,
                const std::string &value, box::color fg,
                box::color bg = kBackground) {
    const unsigned length = static_cast<unsigned>(value.size());
    const unsigned offset = length < width ? (width - length) / 2 : 0;
    text(row, static_cast<std::uint8_t>(first + offset), value, fg,
         static_cast<std::uint8_t>(width - offset), bg);
  }

  void draw_big_digit(unsigned digit_value, std::uint8_t row, std::uint8_t col,
                      unsigned scale_x, unsigned scale_y, box::color color) {
    if (digit_value > 9)
      return;
    for (unsigned source_row = 0; source_row < 7; ++source_row) {
      for (unsigned source_col = 0; source_col < 5; ++source_col) {
        if (kDigits[digit_value][source_row][source_col] != '1')
          continue;
        for (unsigned y = 0; y < scale_y; ++y) {
          for (unsigned x = 0; x < scale_x; ++x)
            put(static_cast<std::uint8_t>(row + source_row * scale_y + y),
                static_cast<std::uint8_t>(col + source_col * scale_x + x), ' ',
                color, color);
        }
      }
    }
  }

  void draw_big_day(std::uint8_t row, unsigned scale_y) {
    const unsigned day = static_cast<unsigned>(selected_.day());
    const unsigned scale_x = left_width_ >= 35 ? 3 : 2;
    const unsigned digit_width = 5 * scale_x;
    const unsigned gap = scale_x;
    const unsigned total_width = digit_width * 2 + gap;
    const std::uint8_t origin =
        static_cast<std::uint8_t>((left_width_ - total_width) / 2);
    draw_big_digit(day / 10, row, origin, scale_x, scale_y, kWhite);
    draw_big_digit(day % 10, row,
                   static_cast<std::uint8_t>(origin + digit_width + gap),
                   scale_x, scale_y, kWhite);
  }

  void draw_block_word(const char *word, std::uint8_t row, box::color color) {
    constexpr unsigned scale_x = 2;
    constexpr unsigned scale_y = 1;
    constexpr unsigned glyph_width = 3 * scale_x;
    constexpr unsigned gap = scale_x;
    const unsigned length = std::strlen(word);
    const unsigned total_width =
        length * glyph_width + (length > 0 ? (length - 1) * gap : 0);
    const std::uint8_t origin = static_cast<std::uint8_t>(
        total_width < left_width_ ? (left_width_ - total_width) / 2 : 1);

    for (unsigned index = 0; index < length; ++index) {
      const std::uint8_t col =
          static_cast<std::uint8_t>(origin + index * (glyph_width + gap));
      for (unsigned source_row = 0; source_row < 5; ++source_row) {
        const char *pattern = block_glyph_row(word[index], source_row);
        for (unsigned source_col = 0; source_col < 3; ++source_col) {
          if (pattern[source_col] != '1')
            continue;
          for (unsigned y = 0; y < scale_y; ++y) {
            for (unsigned x = 0; x < scale_x; ++x)
              put(static_cast<std::uint8_t>(row + source_row * scale_y + y),
                  static_cast<std::uint8_t>(col + source_col * scale_x + x),
                  ' ', color, color);
          }
        }
      }
    }
  }

  void draw_left() {
    if (!time_ok_) {
      centered(rows_ / 2, 1, static_cast<std::uint8_t>(left_width_ - 2),
               "BoxTime unavailable", kRed, kPanel);
      return;
    }

    const unsigned available = rows_ > 2 ? rows_ - 2 : 0;
    constexpr unsigned fixed_height = 8;
    const unsigned scale_y = available >= 7 * 2 + fixed_height + 2 ? 2 : 1;
    const unsigned date_height = 7 * scale_y;
    const unsigned content_height = date_height + fixed_height;
    const unsigned top =
        1 + (available > content_height ? (available - content_height) / 2 : 0);
    draw_big_day(static_cast<std::uint8_t>(top), scale_y);

    const unsigned month = static_cast<unsigned>(selected_.month());
    std::string month_line = std::string(kMonths[month - 1]) + " " +
                             std::to_string(static_cast<int>(selected_.year()));
    const unsigned month_row = top + date_height + 1;
    centered(static_cast<std::uint8_t>(month_row), 1,
             static_cast<std::uint8_t>(left_width_ - 2), month_line, kWhite,
             kPanel);

    const unsigned weekday =
        std::chrono::weekday{DayPoint{selected_}}.iso_encoding() - 1;
    draw_block_word(kWeekdayShort[weekday],
                    static_cast<std::uint8_t>(month_row + 2), kRed);
  }

  void draw_calendar_day(std::uint8_t row, std::uint8_t col, unsigned day,
                         bool selected, bool today) {
    const box::color fg = selected ? kSelectedText : (today ? kRed : kWhite);
    const box::color bg = selected ? kRed : (today ? kDarkRed : kPanel);
    put(row, col, ' ', fg, bg);
    put(row, static_cast<std::uint8_t>(col + 1), ' ', fg, bg);

    char value[3];
    std::snprintf(value, sizeof(value), "%2u", day);
    text(row, col, value, fg, 2, bg);
  }

  void draw_right() {
    const std::uint8_t first = static_cast<std::uint8_t>(left_width_ + 2);
    const std::uint8_t width = static_cast<std::uint8_t>(cols_ - first);

    const unsigned month = static_cast<unsigned>(selected_.month());
    std::string title = std::string(kMonths[month - 1]) + " " +
                        std::to_string(static_cast<int>(selected_.year()));
    const std::uint8_t grid_width = 7 * 3 - 1;
    const std::uint8_t grid_left =
        static_cast<std::uint8_t>(first + (width - grid_width) / 2);
    const Date first_day{selected_.year(), selected_.month(),
                         std::chrono::day{1}};
    const unsigned first_column =
        std::chrono::weekday{DayPoint{first_day}}.iso_encoding() - 1;
    const unsigned total_days = days_in_month(first_day);
    const unsigned grid_rows = (first_column + total_days + 6) / 7;
    const unsigned content_height = 8 + grid_rows;
    const unsigned available = rows_ > 2 ? rows_ - 2 : 0;
    const unsigned top =
        1 + (available > content_height ? (available - content_height) / 2 : 0);
    centered(static_cast<std::uint8_t>(top), first, width, title, kWhite,
             kPanel);
    centered(static_cast<std::uint8_t>(top + 2), grid_left, grid_width,
             "Mo Tu We Th Fr Sa Su", kMuted, kPanel);

    for (unsigned day = 1; day <= total_days; ++day) {
      const unsigned slot = first_column + day - 1;
      const std::uint8_t row = static_cast<std::uint8_t>(top + 4 + slot / 7);
      const std::uint8_t col =
          static_cast<std::uint8_t>(grid_left + (slot % 7) * 3);
      const Date date{selected_.year(), selected_.month(),
                      std::chrono::day{day}};
      draw_calendar_day(row, col, day, same_date(date, selected_),
                        same_date(date, today_));
    }

    centered(static_cast<std::uint8_t>(top + 5 + grid_rows), first, width,
             "Selected", kMuted, kPanel);
    centered(static_cast<std::uint8_t>(top + 6 + grid_rows), first, width,
             date_text(selected_), kWhite, kPanel);
  }

  void draw_footer() {
    text(static_cast<std::uint8_t>(rows_ - 1), 1,
         "Arrows day/week  Q/E month  T today  Esc exit", kMuted,
         static_cast<std::uint8_t>(cols_ - 2));
  }

  void paint() { (void)vga_paint(0, 0, rows_, cols_, frame_.data()); }
};

} // namespace

int main() {
  const vga_dimensions_t dimensions = box::vga::size();
  if (dimensions.rows == 0 || dimensions.cols == 0) {
    std::printf("calendar: screen is unavailable\n");
    return 1;
  }

  Calendar calendar(dimensions.rows, dimensions.cols);
  calendar.render();
  box::key_stream keyboard;
  for (;;) {
    const auto key = keyboard.wait(1000);
    if (key && !calendar.handle(*key))
      break;
    calendar.render();
  }
  calendar.clear();
  return 0;
}
