#include <chrono>
#include <cstdint>
#include <thread>

#include "box/clock.h"
#include "box/cxx/console.h"

namespace {

constexpr std::uint8_t kClockWidth = 8;
constexpr box::color kForeground = box::color::rgb(0xFF, 0x2F, 0x97);
constexpr box::color kBackground = box::color::rgb(0x08, 0x10, 0x18);

char digit(std::int64_t value) { return static_cast<char>('0' + value); }

void paint_time() {
  BoxTime now{};
  if (clock_boxtime_local(&now) != OK)
    return;

  const auto hours = now.hour;
  const auto minutes = now.minute;
  const auto seconds = now.second;
  const char text[kClockWidth] = {digit(hours / 10),   digit(hours % 10),   ':',
                                  digit(minutes / 10), digit(minutes % 10), ':',
                                  digit(seconds / 10), digit(seconds % 10)};

  TextCell cells[kClockWidth];
  for (std::uint8_t i = 0; i < kClockWidth; ++i) {
    cells[i].fg = kForeground.raw();
    cells[i].bg = kBackground.raw();
    cells[i].ch = text[i];
    cells[i].pad[0] = 0;
    cells[i].pad[1] = 0;
    cells[i].pad[2] = 0;
  }
  const auto dimensions = box::vga::size();
  if (dimensions.rows == 0 || dimensions.cols < kClockWidth)
    return;
  (void)vga_paint(0, static_cast<std::uint8_t>(dimensions.cols - kClockWidth),
                  1, kClockWidth, cells);
}

} // namespace

int main() {
  auto next = std::chrono::steady_clock::now();
  for (;;) {
    paint_time();
    next += std::chrono::milliseconds(1);
    std::this_thread::sleep_until(next);
  }
}
