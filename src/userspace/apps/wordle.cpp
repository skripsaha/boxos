#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "box/cxx/console.h"
#include "box/cxx/keyboard.h"
#include "box/keyboard.h"

namespace {

constexpr std::uint8_t kWordLength = 5;
constexpr std::uint8_t kGuessCount = 6;
constexpr std::uint8_t kTileWidth = 4;
constexpr std::uint8_t kTileHeight = 2;
constexpr std::uint8_t kTileGap = 1;
constexpr std::uint8_t kBoardTop = 2;
constexpr std::uint8_t kKeyboardTop =
    kBoardTop + kGuessCount * kTileHeight + (kGuessCount - 1) * kTileGap + 1;

constexpr box::color kBackground = box::color::rgb(0x08, 0x0D, 0x14);
constexpr box::color kTileEmpty = box::color::rgb(0x1A, 0x25, 0x31);
constexpr box::color kTileCurrent = box::color::rgb(0x20, 0x3B, 0x4C);
constexpr box::color kText = box::color::rgb(0xF2, 0xF7, 0xFF);
constexpr box::color kAccent = box::color::rgb(0xCC, 0xD9, 0xDB);
constexpr box::color kCorrect = box::color::rgb(0x07, 0xD5, 0x00);
constexpr box::color kPresent = box::color::rgb(0xE4, 0xB6, 0x24);
constexpr box::color kAbsent = box::color::rgb(0x48, 0x55, 0x64);

/* A compact built-in answer dictionary. Every entry is a lower-case word of
 * exactly five ASCII letters, so it can be checked without a locale or libc. */
constexpr const char *kWords[] = {
    "about", "above", "abuse", "actor", "acute", "adapt", "admit", "adopt",
    "adult", "after", "again", "agent", "agree", "ahead", "alarm", "album",
    "alert", "alike", "alive", "allow", "alone", "along", "alter", "among",
    "anger", "angle", "angry", "apart", "apple", "apply", "arena", "argue",
    "arise", "array", "aside", "asset", "audio", "audit", "avoid", "award",
    "aware", "awful", "bacon", "badge", "badly", "baker", "bases", "basic",
    "basin", "basis", "beach", "began", "begin", "being", "below", "bench",
    "berry", "birth", "black", "blame", "blank", "blast", "blaze", "bleed",
    "blend", "bless", "blind", "block", "blood", "bloom", "blown", "board",
    "boast", "bonus", "booth", "bound", "brain", "brake", "brand", "brave",
    "bread", "break", "breed", "bride", "brief", "bring", "broad", "broke",
    "brook", "brown", "brush", "build", "built", "bunch", "burst", "buyer",
    "cabin", "cable", "cache", "candy", "cargo", "carry", "catch", "cause",
    "cease", "chain", "chair", "chalk", "chaos", "charm", "chart", "chase",
    "cheap", "check", "cheek", "cheer", "chess", "chest", "chief", "child",
    "china", "chirp", "choke", "chose", "civil", "claim", "class", "clean",
    "clear", "clerk", "click", "climb", "cling", "clock", "close", "cloth",
    "cloud", "coach", "coast", "color", "could", "count", "court", "cover",
    "craft", "crash", "crazy", "cream", "creek", "crisp", "cross", "crowd",
    "crown", "crude", "cruel", "crush", "curve", "cycle", "daily", "dairy",
    "dance", "dealt", "death", "debut", "delay", "depth", "devil", "diary",
    "dirty", "doubt", "dozen", "draft", "drain", "drama", "drank", "dream",
    "dress", "dried", "drift", "drink", "drive", "drove", "eager", "early",
    "earth", "eight", "elite", "empty", "enemy", "enjoy", "enter", "entry",
    "equal", "error", "event", "every", "exact", "extra", "faith", "false",
    "fault", "favor", "feast", "fence", "ferry", "fifth", "fifty", "fight",
    "final", "first", "flame", "flash", "fleet", "flesh", "float", "floor",
    "flour", "fluid", "focus", "force", "forge", "forth", "forty", "forum",
    "found", "frame", "frank", "fraud", "fresh", "front", "fruit", "fully",
    "funny", "giant", "given", "glade", "glass", "globe", "glory", "glove",
    "going", "grace", "grade", "grand", "grant", "graph", "grasp", "grass",
    "grave", "great", "green", "grief", "grill", "grind", "grove", "grown",
    "guard", "guess", "guest", "guide", "habit", "happy", "harsh", "haste",
    "heart", "heavy", "hello", "hence", "hobby", "horse", "hotel", "house",
    "human", "humor", "ideal", "image", "imply", "index", "inner", "input",
    "issue", "ivory", "jelly", "joint", "judge", "juice", "jumpy", "kneel",
    "knife", "knock", "known", "label", "labor", "large", "laser", "later",
    "laugh", "layer", "learn", "lease", "least", "leave", "legal", "lemon",
    "level", "light", "limit", "local", "lodge", "logic", "loose", "lower",
    "loyal", "lucky", "lunch", "magic", "major", "maker", "march", "match",
    "maybe", "mayor", "medal", "media", "mercy", "merge", "merit", "metal",
    "meter", "might", "minor", "model", "money", "month", "moral", "motor",
    "mount", "mouse", "mouth", "movie", "music", "naive", "nerve", "never",
    "newer", "night", "noise", "north", "novel", "nurse", "ocean", "offer",
    "often", "olive", "order", "other", "ought", "outer", "owner", "paint",
    "panel", "paper", "party", "paste", "pause", "peace", "peach", "phase",
    "phone", "photo", "piano", "piece", "pilot", "pinch", "pitch", "place",
    "plain", "plane", "plant", "plate", "plaza", "plead", "pluck", "point",
    "pound", "power", "press", "price", "pride", "prime", "print", "prior",
    "prize", "proof", "proud", "prove", "queen", "quick", "quiet", "quite",
    "radio", "raise", "rally", "range", "rapid", "ratio", "reach", "react",
    "ready", "realm", "refer", "relax", "reply", "right", "rival", "river",
    "rough", "round", "route", "royal", "rugby", "rural", "sadly", "safer",
    "saint", "salad", "sauce", "scale", "scene", "scope", "score", "sense",
    "serve", "seven", "shade", "shake", "shall", "shape", "share", "sharp",
    "sheep", "sheet", "shelf", "shell", "shift", "shine", "shirt", "shock",
    "shoot", "short", "sight", "since", "skill", "skull", "sleep", "slice",
    "slide", "slope", "small", "smart", "smile", "smoke", "solid", "solve",
    "sorry", "sound", "south", "space", "spare", "speak", "speed", "spell",
    "spend", "spice", "spite", "sport", "staff", "stage", "stair", "stake",
    "stand", "start", "state", "steam", "steel", "stick", "still", "stock",
    "stone", "stood", "store", "storm", "story", "strip", "stuck", "study",
    "style", "sugar", "suite", "sunny", "super", "sweet", "swing", "table",
    "taste", "teach", "teeth", "thank", "their", "theme", "there", "these",
    "thick", "thing", "think", "third", "those", "three", "throw", "tight",
    "timer", "title", "today", "topic", "total", "touch", "tough", "tower",
    "trace", "track", "trade", "train", "treat", "trend", "trial", "tribe",
    "trick", "truck", "truly", "trust", "truth", "twice", "under", "union",
    "unite", "until", "upper", "urban", "usual", "value", "video", "visit",
    "vital", "voice", "waste", "watch", "water", "wheel", "where", "which",
    "while", "white", "whole", "whose", "woman", "world", "worry", "worse",
    "worst", "worth", "would", "write", "wrong", "wrote", "young", "youth"};

enum class TileState : std::uint8_t {
  empty,
  current,
  absent,
  present,
  correct
};

TextCell make_cell(char ch, box::color fg, box::color bg) {
  TextCell cell{};
  cell.fg = fg.raw();
  cell.bg = bg.raw();
  cell.ch = ch;
  return cell;
}

class WordleGame {
public:
  WordleGame(std::uint8_t rows, std::uint8_t cols)
      : rows_(rows), cols_(cols), frame_(static_cast<std::size_t>(rows) * cols),
        rng_(seed()) {
    reset();
  }

  void reset() {
    row_ = 0;
    length_ = 0;
    finished_ = false;
    won_ = false;
    message_ = "Type a five-letter word";
    answer_ = kWords[random_below(word_count())];
    for (std::uint8_t r = 0; r < kGuessCount; ++r) {
      for (std::uint8_t c = 0; c < kWordLength; ++c) {
        guesses_[r][c] = 0;
        states_[r][c] = TileState::empty;
      }
    }
    for (std::uint8_t i = 0; i < 26; ++i)
      keyboard_[i] = TileState::empty;
  }

  bool handle_key(const box::key &key) {
    if (key.ch() == KEY_ESCAPE)
      return false;
    if (finished_) {
      if (key.ch() == KEY_ENTER || key.ch() == ' ')
        reset();
      return true;
    }

    char ch = key.ch();
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch - 'A' + 'a');
    if (ch >= 'a' && ch <= 'z') {
      if (length_ < kWordLength) {
        guesses_[row_][length_++] = ch;
        message_ = "Press Enter to check";
      }
      return true;
    }
    if (ch == KEY_BACKSPACE) {
      if (length_ != 0)
        guesses_[row_][--length_] = 0;
      message_ = "Type a five-letter word";
      return true;
    }
    if (ch == KEY_ENTER)
      submit();
    return true;
  }

  bool render() {
    for (TextCell &cell : frame_)
      cell = make_cell(' ', kText, kBackground);

    draw_centered(0, "W O R D L E", kAccent);
    const std::uint8_t board_width =
        kWordLength * kTileWidth + (kWordLength - 1) * kTileGap;
    const std::uint8_t board_left =
        cols_ > board_width
            ? static_cast<std::uint8_t>((cols_ - board_width) / 2)
            : 0;

    for (std::uint8_t r = 0; r < kGuessCount; ++r) {
      for (std::uint8_t c = 0; c < kWordLength; ++c) {
        const std::uint8_t x =
            static_cast<std::uint8_t>(board_left + c * (kTileWidth + kTileGap));
        const std::uint8_t y =
            static_cast<std::uint8_t>(kBoardTop + r * (kTileHeight + kTileGap));
        TileState state = states_[r][c];
        char letter = guesses_[r][c];
        if (r == row_ && !finished_ && state == TileState::empty && letter != 0)
          state = TileState::current;
        draw_tile(x, y, letter, state);
      }
    }

    draw_keyboard();
    if (finished_) {
      const char *result =
          won_ ? "Solved!  ENTER: new word" : "Word:      ENTER: new word";
      if (!won_)
        draw_centered(kKeyboardTop + 3, answer_, kText);
      draw_centered(kKeyboardTop + 4, result, won_ ? kCorrect : kPresent);
    } else {
      draw_centered(kKeyboardTop + 4, message_, kText);
    }
    return vga_paint(0, 0, rows_, cols_, frame_.data()) == 0;
  }

private:
  std::uint8_t rows_;
  std::uint8_t cols_;
  std::vector<TextCell> frame_;
  const char *answer_ = nullptr;
  char guesses_[kGuessCount][kWordLength]{};
  TileState states_[kGuessCount][kWordLength]{};
  TileState keyboard_[26]{};
  std::uint8_t row_ = 0;
  std::uint8_t length_ = 0;
  std::uint32_t rng_;
  bool finished_ = false;
  bool won_ = false;
  std::string_view message_ = "";

  static constexpr std::size_t word_count() {
    return sizeof(kWords) / sizeof(kWords[0]);
  }

  static std::uint32_t seed() {
    return static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  std::uint32_t random() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
  }

  std::uint32_t random_below(std::size_t limit) {
    return limit == 0 ? 0 : random() % static_cast<std::uint32_t>(limit);
  }

  std::size_t index(std::uint8_t x, std::uint8_t y) const {
    return static_cast<std::size_t>(y) * cols_ + x;
  }

  void draw_tile(std::uint8_t x, std::uint8_t y, char letter, TileState state) {
    box::color bg = kTileEmpty;
    box::color fg = kText;
    switch (state) {
    case TileState::current:
      bg = kTileCurrent;
      break;
    case TileState::absent:
      bg = kAbsent;
      break;
    case TileState::present:
      bg = kPresent;
      break;
    case TileState::correct:
      bg = kCorrect;
      break;
    case TileState::empty:
      break;
    }

    for (std::uint8_t dy = 0; dy < kTileHeight; ++dy) {
      for (std::uint8_t dx = 0; dx < kTileWidth; ++dx)
        frame_[index(static_cast<std::uint8_t>(x + dx),
                     static_cast<std::uint8_t>(y + dy))] =
            make_cell(' ', fg, bg);
    }
    if (letter != 0) {
      const std::uint8_t letter_x =
          static_cast<std::uint8_t>(x + kTileWidth / 2);
      frame_[index(letter_x, y)] = make_cell(letter, kText, bg);
    }
  }

  void draw_keyboard() {
    draw_keyboard_row(kKeyboardTop, "QWERTYUIOP", 10);
    draw_keyboard_row(static_cast<std::uint8_t>(kKeyboardTop + 1), "ASDFGHJKL",
                      9);
    draw_keyboard_row(static_cast<std::uint8_t>(kKeyboardTop + 2), "ZXCVBNM",
                      7);
  }

  void draw_keyboard_row(std::uint8_t row, const char *keys,
                         std::uint8_t count) {
    constexpr std::uint8_t key_width = 3;
    constexpr std::uint8_t key_gap = 1;
    const std::uint8_t total = count * (key_width + key_gap) - key_gap;
    const std::uint8_t left =
        cols_ > total ? static_cast<std::uint8_t>((cols_ - total) / 2) : 0;
    for (std::uint8_t i = 0; i < count; ++i) {
      const char key = keys[i];
      const std::uint8_t x =
          static_cast<std::uint8_t>(left + i * (key_width + key_gap));
      const TileState state = keyboard_[static_cast<std::uint8_t>(key - 'A')];
      box::color bg = kTileEmpty;
      if (state == TileState::absent)
        bg = kAbsent;
      else if (state == TileState::present)
        bg = kPresent;
      else if (state == TileState::correct)
        bg = kCorrect;
      for (std::uint8_t dx = 0; dx < key_width; ++dx)
        frame_[index(static_cast<std::uint8_t>(x + dx), row)] =
            make_cell(' ', kText, bg);
      frame_[index(static_cast<std::uint8_t>(x + 1), row)] =
          make_cell(key, kText, bg);
    }
  }

  void draw_centered(std::uint8_t row, std::string_view text,
                     box::color color) {
    if (row >= rows_)
      return;
    const std::size_t count = text.size() < cols_ ? text.size() : cols_;
    const std::uint8_t left =
        static_cast<std::uint8_t>(count < cols_ ? (cols_ - count) / 2 : 0);
    for (std::size_t i = 0; i < count; ++i)
      frame_[index(static_cast<std::uint8_t>(left + i), row)] =
          make_cell(text[i], color, kBackground);
  }

  bool is_word(const char *guess) const {
    for (const char *word : kWords) {
      bool equal = true;
      for (std::uint8_t i = 0; i < kWordLength; ++i)
        if (word[i] != guess[i])
          equal = false;
      if (equal)
        return true;
    }
    return false;
  }

  void submit() {
    if (length_ != kWordLength) {
      message_ = "Need five letters";
      return;
    }
    if (!is_word(guesses_[row_])) {
      message_ = "Not in the word list";
      return;
    }

    bool used[kWordLength]{};
    for (std::uint8_t i = 0; i < kWordLength; ++i) {
      if (guesses_[row_][i] == answer_[i]) {
        states_[row_][i] = TileState::correct;
        used[i] = true;
        update_keyboard(guesses_[row_][i], TileState::correct);
      }
    }
    for (std::uint8_t i = 0; i < kWordLength; ++i) {
      if (states_[row_][i] == TileState::correct)
        continue;
      states_[row_][i] = TileState::absent;
      for (std::uint8_t j = 0; j < kWordLength; ++j) {
        if (!used[j] && guesses_[row_][i] == answer_[j]) {
          states_[row_][i] = TileState::present;
          used[j] = true;
          break;
        }
      }
      update_keyboard(guesses_[row_][i], states_[row_][i]);
    }

    bool correct = true;
    for (std::uint8_t i = 0; i < kWordLength; ++i)
      if (states_[row_][i] != TileState::correct)
        correct = false;
    if (correct) {
      won_ = true;
      finished_ = true;
      message_ = "Solved";
      return;
    }
    if (row_ + 1 == kGuessCount) {
      finished_ = true;
      message_ = "Game over";
      return;
    }
    ++row_;
    length_ = 0;
    message_ = "Next guess";
  }

  void update_keyboard(char letter, TileState state) {
    const std::uint8_t index = static_cast<std::uint8_t>(letter - 'a');
    TileState &old = keyboard_[index];
    if (state == TileState::correct ||
        (state == TileState::present && old != TileState::correct) ||
        (state == TileState::absent && old == TileState::empty))
      old = state;
  }
};

bool is_exit_key(const box::key &key) {
  return key.ch() == KEY_ESCAPE ||
         (key.ctrl() && (key.ch() == 'c' || key.ch() == 'C'));
}

} // namespace

int main() {
  const auto dimensions = box::vga::size();
  if (dimensions.rows < kKeyboardTop + 5 || dimensions.cols < 32) {
    printf("wordle: screen is too small\n");
    return 1;
  }

  WordleGame game(dimensions.rows, dimensions.cols);
  if (!game.render())
    return 1;

  box::key_stream keyboard;
  for (;;) {
    const auto key = keyboard.wait();
    if (!key)
      continue;
    if (is_exit_key(*key))
      break;
    if (!game.handle_key(*key))
      break;
    if (!game.render())
      break;
  }

  box::vga::clear(kBackground);
  box::vga::move_cursor(0, 0);
  return 0;
}
