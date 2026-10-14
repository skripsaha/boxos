#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "box/clock.h"
#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/error.h"
#include "box/keyboard.h"
#include "box/system.h"
#include "box/vga.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr box::color kBackground = box::color::rgb(0x00, 0x00, 0x00);
constexpr box::color kPanel = box::color::rgb(0x07, 0x09, 0x0D);
constexpr box::color kPanelAlt = box::color::rgb(0x18, 0x2E, 0x4A);
constexpr box::color kBorder = box::color::rgb(0x82, 0x91, 0x9A);
constexpr box::color kText = box::color::rgb(0xF1, 0xF7, 0xFF);
constexpr box::color kMuted = box::color::rgb(0xA6, 0xC3, 0xE6);
constexpr box::color kCyan = box::color::rgb(0x00, 0xE5, 0xFF);
constexpr box::color kGreen = box::color::rgb(0x5C, 0xFF, 0x8C);
constexpr box::color kYellow = box::color::rgb(0xFF, 0xD3, 0x4E);
constexpr box::color kRed = box::color::rgb(0xFF, 0x5C, 0x6C);
constexpr box::color kSelection = box::color::rgb(0x00, 0xE5, 0xFF);
constexpr box::color kBrightGreen = box::color::rgb(0x59, 0xDB, 0x0D);

struct ProcessRow {
  std::uint32_t pid = 0;
  std::uint32_t generation = 0;
  std::uint32_t state = 0;
  std::uint64_t cpu_us = 0;
  unsigned cpu_percent = 0;
  std::string tags;
};

struct MachineSnapshot {
  system_info_t info{};
  system_resources_t resources{};
  bool info_ok = false;
  bool resources_ok = false;
  unsigned cpu_percent = 0;
  unsigned ram_percent = 0;
  unsigned disk_percent = 0;
};

TextCell cell(char ch, box::color fg, box::color bg = kBackground) {
  TextCell result{};
  result.fg = fg.raw();
  result.bg = bg.raw();
  result.ch = ch;
  return result;
}

std::string bytes(std::uint64_t value) {
  constexpr std::uint64_t kKiB = 1ULL << 10;
  constexpr std::uint64_t kMiB = 1ULL << 20;
  constexpr std::uint64_t kGiB = 1ULL << 30;
  const char *unit = "B";
  std::uint64_t base = 1;
  if (value >= kGiB) {
    unit = "gb";
    base = kGiB;
  } else if (value >= kMiB) {
    unit = "mb";
    base = kMiB;
  } else if (value >= kKiB) {
    unit = "kb";
    base = kKiB;
  }

  char text[32];
  if (base == 1) {
    std::snprintf(text, sizeof(text), "%llu %s",
                  static_cast<unsigned long long>(value), unit);
  } else {
    const unsigned whole = static_cast<unsigned>(value / base);
    const unsigned tenth = static_cast<unsigned>((value % base) * 10 / base);
    std::snprintf(text, sizeof(text), "%u.%u %s", whole, tenth, unit);
  }
  return text;
}

std::string uptime(std::uint64_t nanoseconds) {
  std::uint64_t seconds = nanoseconds / 1000000000ULL;
  const std::uint64_t days = seconds / 86400ULL;
  seconds %= 86400ULL;
  const unsigned hours = static_cast<unsigned>(seconds / 3600ULL);
  seconds %= 3600ULL;
  const unsigned minutes = static_cast<unsigned>(seconds / 60ULL);
  const unsigned secs = static_cast<unsigned>(seconds % 60ULL);

  char text[48];
  if (days != 0) {
    std::snprintf(text, sizeof(text), "%llud %02u:%02u:%02u",
                  static_cast<unsigned long long>(days), hours, minutes, secs);
  } else {
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u", hours, minutes, secs);
  }
  return text;
}

std::size_t bounded_length(const char *text, std::size_t limit) {
  std::size_t length = 0;
  while (length < limit && text[length] != '\0')
    ++length;
  return length;
}

const char *state_name(std::uint32_t state) {
  switch (state) {
  case 0:
    return "new";
  case 1:
    return "live";
  case 2:
    return "wait";
  case 3:
    return "stop";
  case 4:
    return "done";
  case 5:
    return "crash";
  default:
    return "?";
  }
}

bool same_process(const ProcessRow &left, const ProcessRow &right) {
  return left.pid == right.pid && left.generation == right.generation;
}

ProcessRow *find_process(std::vector<ProcessRow> &list, std::uint32_t pid,
                         std::uint32_t generation) {
  for (ProcessRow &process : list) {
    if (process.pid == pid && process.generation == generation)
      return &process;
  }
  return nullptr;
}

void collect_tag(const char *tag, std::vector<ProcessRow> &list) {
  ProcMate *mates = nullptr;
  std::uint32_t total = 0;
  const int count = proc_crew(tag, &mates, &total);
  if (count < 0 || !mates)
    return;

  for (int i = 0; i < count; ++i) {
    if (find_process(list, mates[i].pid, mates[i].generation) != nullptr)
      continue;
    ProcessRow process;
    process.pid = mates[i].pid;
    process.generation = mates[i].generation;
    process.state = mates[i].state;
    process.cpu_us = mates[i].cpu_us;
    process.tags = mates[i].tags ? mates[i].tags : "";
    list.push_back(std::move(process));
  }
  free(mates);
}

unsigned percent(std::uint64_t part, std::uint64_t whole) {
  if (whole == 0)
    return 0;
  if (part >= whole)
    return 100;
  return static_cast<unsigned>((part * 100ULL) / whole);
}

std::uint8_t blend_channel(std::uint8_t from, std::uint8_t to,
                           unsigned position, unsigned span) {
  const int delta = static_cast<int>(to) - static_cast<int>(from);
  return static_cast<std::uint8_t>(static_cast<int>(from) +
                                   delta * static_cast<int>(position) /
                                       static_cast<int>(span));
}

box::color blend_graph_color(box::color from, box::color to, unsigned position,
                             unsigned span) {
  return box::color::rgb(blend_channel(from.r(), to.r(), position, span),
                         blend_channel(from.g(), to.g(), position, span),
                         blend_channel(from.b(), to.b(), position, span));
}

box::color graph_color(unsigned load) {
  load = load > 100 ? 100 : load;
  constexpr box::color cyan = box::color::rgb(0x35, 0xD9, 0xFF);
  constexpr box::color green = box::color::rgb(0x38, 0xFF, 0x70);
  constexpr box::color yellow = box::color::rgb(0xFF, 0xE3, 0x4A);
  constexpr box::color orange = box::color::rgb(0xFF, 0x9A, 0x2F);
  constexpr box::color red = box::color::rgb(0xFF, 0x30, 0x48);

  if (load < 25)
    return blend_graph_color(cyan, green, load, 25);
  if (load < 50)
    return blend_graph_color(green, yellow, load - 25, 25);
  if (load < 75)
    return blend_graph_color(yellow, orange, load - 50, 25);
  return blend_graph_color(orange, red, load - 75, 25);
}

class TaskCenter {
public:
  TaskCenter(std::uint8_t rows, std::uint8_t cols)
      : rows_(rows), cols_(cols), left_width_(cols / 2),
        history_(graph_width(), 0),
        frame_(static_cast<std::size_t>(rows) * cols,
               cell(' ', kText, kBackground)) {}

  bool refresh() {
    const auto now = Clock::now();
    const std::uint64_t now_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
    MachineSnapshot next_machine;
    read_machine(next_machine);
    std::vector<ProcessRow> next_processes;
    next_processes.reserve(processes_.capacity() > 8 ? processes_.capacity()
                                                     : 8);
    collect_tag("app", next_processes);
    collect_tag("utility", next_processes);
    update_process_load(next_processes, now_us);
    std::sort(next_processes.begin(), next_processes.end(),
              [](const ProcessRow &left, const ProcessRow &right) {
                if (left.cpu_percent != right.cpu_percent)
                  return left.cpu_percent > right.cpu_percent;
                return left.pid < right.pid;
              });
    preserve_selection(next_processes);
    processes_ = std::move(next_processes);
    machine_ = next_machine;
    update_history(machine_.cpu_percent);
    last_sample_us_ = now_us;
    render();
    return true;
  }

  bool handle(const box::key &key) {
    if (key.ch() == KEY_ESCAPE)
      return true;
    if (key.scancode() == KEY_UP) {
      move_selection(-1);
    } else if (key.scancode() == KEY_DOWN) {
      move_selection(1);
    } else if (key.ch() == KEY_BACKSPACE || key.scancode() == KEY_DELETE) {
      terminate_selected();
    }
    return false;
  }

  void clear() {
    box::vga::clear(kBackground);
    box::vga::move_cursor(0, 0);
  }

private:
  std::uint8_t rows_;
  std::uint8_t cols_;
  std::uint8_t left_width_;
  std::vector<ProcessRow> processes_;
  std::vector<ProcessRow> previous_processes_;
  std::vector<unsigned char> history_;
  std::vector<TextCell> frame_;
  MachineSnapshot machine_{};
  std::uint64_t last_sample_us_ = 0;
  std::uint32_t selected_pid_ = 0;
  std::uint32_t selected_generation_ = 0;
  std::size_t selected_ = 0;
  unsigned char smoothed_load_ = 0;
  unsigned smoothing_phase_ = 0;
  std::string status_;

  std::uint8_t graph_width() const {
    return left_width_ > 2 ? static_cast<std::uint8_t>(left_width_ - 2) : 1;
  }

  // The disk's use as a share — or that the kernel does not count this
  // volume's free ground (a format-v2 book: box/system.h, volume_free_counted).
  std::string disk_used() const {
    if (machine_.resources_ok && machine_.resources.volume_present &&
        !machine_.resources.volume_free_counted)
      return "free not counted";
    return std::to_string(machine_.disk_percent) + "%";
  }

  void read_machine(MachineSnapshot &snapshot) {
    snapshot.info_ok = sysinfo(&snapshot.info) == 0;
    snapshot.resources_ok = system_resources(&snapshot.resources) == 0;
    if (snapshot.info_ok)
      snapshot.ram_percent =
          percent(snapshot.info.used_memory, snapshot.info.total_memory);
    if (snapshot.resources_ok && snapshot.resources.volume_present &&
        snapshot.resources.volume_free_counted)
      snapshot.disk_percent = percent(snapshot.resources.volume_used_bytes,
                                      snapshot.resources.volume_total_bytes);

    if (snapshot.resources_ok && last_resources_valid_) {
      const std::uint64_t busy =
          snapshot.resources.cpu_busy_tsc >= last_resources_.cpu_busy_tsc
              ? snapshot.resources.cpu_busy_tsc - last_resources_.cpu_busy_tsc
              : 0;
      const std::uint64_t total =
          snapshot.resources.cpu_total_tsc >= last_resources_.cpu_total_tsc
              ? snapshot.resources.cpu_total_tsc - last_resources_.cpu_total_tsc
              : 0;
      snapshot.cpu_percent = percent(busy, total);
    }
    if (snapshot.resources_ok) {
      last_resources_ = snapshot.resources;
      last_resources_valid_ = true;
    }
  }

  void update_process_load(std::vector<ProcessRow> &current,
                           std::uint64_t now_us) {
    const std::uint64_t elapsed =
        last_sample_us_ != 0 && now_us > last_sample_us_
            ? now_us - last_sample_us_
            : 0;
    for (ProcessRow &process : current) {
      for (const ProcessRow &old : previous_processes_) {
        if (!same_process(process, old))
          continue;
        if (elapsed != 0 && process.cpu_us >= old.cpu_us) {
          const std::uint64_t delta = process.cpu_us - old.cpu_us;
          const std::uint64_t load = (delta * 100ULL) / elapsed;
          process.cpu_percent = static_cast<unsigned>(load > 999 ? 999 : load);
        }
        break;
      }
    }
    previous_processes_ = current;
  }

  void preserve_selection(const std::vector<ProcessRow> &next) {
    if (selected_pid_ != 0) {
      for (std::size_t i = 0; i < next.size(); ++i) {
        if (next[i].pid == selected_pid_ &&
            next[i].generation == selected_generation_) {
          selected_ = i;
          return;
        }
      }
    }
    if (next.empty()) {
      selected_ = 0;
      selected_pid_ = 0;
      selected_generation_ = 0;
    } else {
      if (selected_ >= next.size())
        selected_ = next.size() - 1;
      selected_pid_ = next[selected_].pid;
      selected_generation_ = next[selected_].generation;
    }
  }

  void move_selection(int delta) {
    if (processes_.empty())
      return;
    const std::size_t count = processes_.size();
    if (delta < 0)
      selected_ = selected_ == 0 ? count - 1 : selected_ - 1;
    else
      selected_ = selected_ + 1 >= count ? 0 : selected_ + 1;
    selected_pid_ = processes_[selected_].pid;
    selected_generation_ = processes_[selected_].generation;
    render();
  }

  void terminate_selected() {
    if (processes_.empty()) {
      status_ = "no process selected";
      render();
      return;
    }
    const ProcessRow &process = processes_[selected_];
    const int rc = proc_finish(process.pid, process.generation);
    if (rc == 0) {
      status_ = "terminated pid " + std::to_string(process.pid);
    } else {
      status_ = "cannot terminate pid " + std::to_string(process.pid) + " (" +
                std::to_string(rc) + ")";
    }
    render();
  }

  void update_history(unsigned load) {
    if (history_.empty())
      return;
    load = load > 100 ? 100 : load;
    const int target = static_cast<int>(load);
    const int current = static_cast<int>(smoothed_load_);
    const int delta = target - current;
    if (delta != 0) {
      const unsigned response = 28u + ((smoothing_phase_ * 17u) % 33u);
      const unsigned distance =
          static_cast<unsigned>(delta < 0 ? -delta : delta);
      unsigned step = distance * response / 100u;
      if (step == 0)
        step = 1;
      smoothed_load_ = static_cast<unsigned char>(
          delta > 0 ? current + static_cast<int>(step)
                    : current - static_cast<int>(step));
      if (delta > 0 && smoothed_load_ > load)
        smoothed_load_ = static_cast<unsigned char>(load);
      if (delta < 0 && smoothed_load_ < load)
        smoothed_load_ = static_cast<unsigned char>(load);
    }
    ++smoothing_phase_;
    if (history_.size() > 1)
      std::copy(history_.begin() + 1, history_.end(), history_.begin());
    history_.back() = smoothed_load_;
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
      const std::uint16_t x =
          static_cast<std::uint16_t>(col) + static_cast<std::uint16_t>(i);
      if (x >= cols_)
        break;
      put(row, static_cast<std::uint8_t>(x), value[i], fg, bg);
    }
  }

  void box(std::uint8_t x, std::uint8_t y, std::uint8_t width,
           std::uint8_t height, const char *title) {
    if (width < 2 || height < 2)
      return;
    for (std::uint8_t row = static_cast<std::uint8_t>(y + 1);
         row < y + height - 1; ++row) {
      for (std::uint8_t col = static_cast<std::uint8_t>(x + 1);
           col < x + width - 1; ++col)
        put(row, col, ' ', kText, kPanel);
    }
    for (std::uint8_t col = x; col < x + width; ++col) {
      put(y, col, '-', kBorder, kPanel);
      put(static_cast<std::uint8_t>(y + height - 1), col, '-', kBorder, kPanel);
    }
    for (std::uint8_t row = y; row < y + height; ++row) {
      put(row, x, '|', kBorder, kPanel);
      put(row, static_cast<std::uint8_t>(x + width - 1), '|', kBorder, kPanel);
    }
    put(y, x, '.', kBorder, kPanel);
    put(y, static_cast<std::uint8_t>(x + width - 1), '.', kBorder, kPanel);
    put(static_cast<std::uint8_t>(y + height - 1), x, '"', kBorder, kPanel);
    put(static_cast<std::uint8_t>(y + height - 1),
        static_cast<std::uint8_t>(x + width - 1), '"', kBorder, kPanel);
    text(y, static_cast<std::uint8_t>(x + 2), title, kCyan,
         static_cast<std::uint8_t>(width - 4), kPanel);
  }

  void bar(std::uint8_t row, std::uint8_t col, std::uint8_t width,
           unsigned value, box::color color) {
    const unsigned filled =
        static_cast<unsigned>(width) * (value > 100 ? 100 : value) / 100;
    for (std::uint8_t i = 0; i < width; ++i)
      put(row, static_cast<std::uint8_t>(col + i), ' ',
          i < filled ? color : kPanelAlt, i < filled ? color : kPanelAlt);
  }

  void render_graph(std::uint8_t height) {
    const std::uint8_t top = 1;
    const std::uint8_t bottom = static_cast<std::uint8_t>(height - 2);
    const std::uint8_t graph_height =
        static_cast<std::uint8_t>(bottom - top + 1);
    const std::uint8_t center =
        static_cast<std::uint8_t>(top + graph_height / 2);
    const unsigned radius_limit = graph_height / 2;
    for (std::size_t i = 0; i < history_.size(); ++i) {
      const unsigned value = history_[i];
      const unsigned radius = value * radius_limit / 100;
      const box::color color = graph_color(value);
      const std::uint8_t x = static_cast<std::uint8_t>(i + 1);
      for (int offset = -static_cast<int>(radius);
           offset <= static_cast<int>(radius); ++offset) {
        const std::uint8_t y =
            static_cast<std::uint8_t>(static_cast<int>(center) + offset);
        put(y, x, ':', color, kPanel);
      }
    }
    std::string title = "Load: " + std::to_string(machine_.cpu_percent) + "%";
    text(1, 2, title, kText, static_cast<std::uint8_t>(left_width_ - 4),
         kPanel);
  }

  void render_stats(std::uint8_t top, std::uint8_t height) {
    const std::uint8_t content = static_cast<std::uint8_t>(left_width_ - 4);
    text(static_cast<std::uint8_t>(top + 1), 2,
         "Current state", kCyan, content, kPanel);
    if (!machine_.info_ok) {
      text(static_cast<std::uint8_t>(top + 3), 2, "system info unavailable",
           kRed, content, kPanel);
      return;
    }

    if (height < 11) {
      text(static_cast<std::uint8_t>(top + 2), 2,
           "RAM " + std::to_string(machine_.ram_percent) + "%  Disk " +
               disk_used(),
           kText, content, kPanel);
      return;
    }

    text(static_cast<std::uint8_t>(top + 3), 2,
         "Memory: " + bytes(machine_.info.used_memory) + " / " +
             bytes(machine_.info.total_memory) + "  " +
             std::to_string(machine_.ram_percent) + "%",
         kText, content, kPanel);
    bar(static_cast<std::uint8_t>(top + 4), 2, content, machine_.ram_percent,
        kBrightGreen);

    if (machine_.resources_ok && machine_.resources.volume_present) {
      std::string name(machine_.resources.volume_name,
                       bounded_length(machine_.resources.volume_name, 16));
      text(static_cast<std::uint8_t>(top + 6), 2,
           "Disk: " + (name.empty() ? std::string("unnamed") : name) + "  " +
               disk_used(),
           kText, content, kPanel);
      bar(static_cast<std::uint8_t>(top + 7), 2, content, machine_.disk_percent,
          kBrightGreen);
      text(static_cast<std::uint8_t>(top + 8), 2,
           (machine_.resources.volume_free_counted
                ? bytes(machine_.resources.volume_free_bytes) + " free / "
                : std::string("free not counted / ")) +
               bytes(machine_.resources.volume_total_bytes),
           kMuted, content, kPanel);
    } else {
      text(static_cast<std::uint8_t>(top + 6), 2, "Disk unavailable", kMuted,
           content, kPanel);
    }

    text(static_cast<std::uint8_t>(top + 9), 2,
         "Uptime: " + uptime(machine_.info.uptime_ns) + "  Tasks: " +
             std::to_string(machine_.info.process_count),
         kMuted, content, kPanel);
  }

  void render_processes(std::uint8_t x, std::uint8_t width) {
    text(1, static_cast<std::uint8_t>(x + 2), "ID     Load State Tags", kText,
         static_cast<std::uint8_t>(width - 4), kPanel);
    const std::uint8_t first_row = 2;
    const std::uint8_t last_row = static_cast<std::uint8_t>(rows_ - 3);
    const std::size_t visible =
        last_row >= first_row ? last_row - first_row + 1 : 0;
    std::size_t first = 0;
    if (visible != 0 && selected_ >= visible)
      first = selected_ - visible + 1;

    for (std::size_t line = 0;
         line < visible && first + line < processes_.size(); ++line) {
      const ProcessRow &process = processes_[first + line];
      const bool selected = first + line == selected_;
      const box::color fg = selected ? kBackground : kText;
      const box::color bg = selected ? kSelection : kPanel;
      const std::uint8_t row = static_cast<std::uint8_t>(first_row + line);
      for (std::uint8_t col = x + 1; col < x + width - 1; ++col)
        put(row, col, ' ', fg, bg);

      char line_text[256];
      std::snprintf(line_text, sizeof(line_text), "%-6u %3u%% %-5s %s",
                    static_cast<unsigned>(process.pid), process.cpu_percent,
                    state_name(process.state), process.tags.c_str());
      text(row, static_cast<std::uint8_t>(x + 2), line_text, fg,
           static_cast<std::uint8_t>(width - 4), bg);
    }
    if (processes_.empty())
      text(first_row, static_cast<std::uint8_t>(x + 2),
           "no executable processes", kMuted,
           static_cast<std::uint8_t>(width - 4));
  }

  void render_footer() {
    std::string footer =
        "[Arrows] select  [Backspace/Delete] terminate  [Esc] quit";
    if (!status_.empty())
      footer += "  " + status_;
    text(static_cast<std::uint8_t>(rows_ - 1), 1, footer, kMuted,
         static_cast<std::uint8_t>(cols_ - 2));
  }

  void render() {
    std::fill(frame_.begin(), frame_.end(), cell(' ', kText, kBackground));
    if (rows_ < 14 || cols_ < 60) {
      text(1, 2, "taskcenter needs at least 60x14 console cells", kRed,
           static_cast<std::uint8_t>(cols_ - 4));
      (void)vga_paint(0, 0, rows_, cols_, frame_.data());
      return;
    }

    const std::uint8_t graph_height = static_cast<std::uint8_t>(rows_ / 2);
    const std::uint8_t stats_top = graph_height;
    const std::uint8_t stats_height =
        static_cast<std::uint8_t>(rows_ - stats_top - 1);
    const std::uint8_t right_x = static_cast<std::uint8_t>(left_width_ + 1);
    const std::uint8_t right_width = static_cast<std::uint8_t>(cols_ - right_x);
    box(0, 0, left_width_, graph_height, "Processor Load Graph");
    box(0, stats_top, left_width_, stats_height, "System Information");
    box(right_x, 0, right_width, static_cast<std::uint8_t>(rows_ - 1),
        "Tasks List");
    render_graph(graph_height);
    render_stats(stats_top, stats_height);
    render_processes(right_x, right_width);
    render_footer();
    (void)vga_paint(0, 0, rows_, cols_, frame_.data());
  }

  system_resources_t last_resources_{};
  bool last_resources_valid_ = false;
};

} // namespace

int main() {
  const vga_dimensions_t dimensions = box::vga::size();
  if (dimensions.rows == 0 || dimensions.cols == 0) {
    std::printf("taskcenter: screen is unavailable\n");
    return 1;
  }

  TaskCenter center(dimensions.rows, dimensions.cols);
  center.refresh();
  box::key_stream keyboard;
  auto next_refresh = Clock::now();
  for (;;) {
    if (const auto key = keyboard.wait(100); key && center.handle(*key))
      break;
    const auto now = Clock::now();
    if (now >= next_refresh) {
      center.refresh();
      next_refresh = now + std::chrono::milliseconds(250);
    }
  }
  center.clear();
  return 0;
}
