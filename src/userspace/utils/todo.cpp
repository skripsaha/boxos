/* todo — what there is to do.
 *
 *   todo                   the list, on the screen: what belongs to the Use Context
 *   todo buy milk          buy milk is added, in the Use Context it is said in
 *   todo -> plans.txt      the list as it is shown, as text into plans.txt
 *
 * On the screen:
 *   up, down               choose          shift+up, shift+down   move it
 *   space                  done, or not done again
 *   a                      add             enter     change its words
 *   s                      star it: to be done first
 *   d                      take it away    tab       everywhere, or here again
 *   q, ESC                 leave
 *
 * A thing is said in a Use Context and belongs to it: with `use code` said,
 * the list is what there is to do for code — a thing said in a context that
 * shares a tag with it — and what was said in no context belongs everywhere.
 * Tab makes the list flat — everywhere: every thing on it shown, and a new one
 * said in no context — and back; it stays the way it was left. The list is
 * kept on the volume (todo.h) and every change is settled there before the
 * next key is read. */

#include <algorithm>
#include <format>
#include <new>
#include <string>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "keep.h"
#include "todo_desk.h"

BOX_FORM("takes optional text \"a thing to do; with none, the list on the screen\"\n");

namespace {

using Color = painter::core::Color;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;

constexpr int CellsPerSpark = 60;   /* the cheer is as thick on a big screen as on a small one */

const Color SparkInks[] = {Color::Rgb(0x60, 0xA0, 0xF0), Color::Rgb(0x70, 0xD0, 0x80), Color::Rgb(0x60, 0xD0, 0xD0),
                           Color::Rgb(0xD0, 0x80, 0xE0), Color::Rgb(0xF0, 0x80, 0x80), Color::Rgb(0xE0, 0xD0, 0x60)};

}

void TodoDesk::Keep()
{
    if (std::optional<std::string> Refusal = List.Save()) Note = "not kept: " + *Refusal;
}

/* The chosen thing swapped with the one shown before or after it, when that
 * one stands where it does: both starred, both to do, or both done. */
void TodoDesk::Move(int Way)
{
    std::vector<std::size_t> Places = List.View(Here());
    if (Places.empty()) return;
    std::size_t Other = Way < 0 ? Selected - 1 : Selected + 1;
    if ((Way < 0 && Selected == 0) || Other >= Places.size()) return;
    TodoItem &Mine = List.Items[Places[Selected]], &Theirs = List.Items[Places[Other]];
    if (TodoRank(Mine) != TodoRank(Theirs)) return;
    std::swap(Mine, Theirs);
    Selected = Other;
    Flashed  = static_cast<std::size_t>(-1);
    Keep();
}

/* Confetti over the list: sparks at random places, born over the first two
 * thirds of the cheer. */
void TodoDesk::Cheer()
{
    Seed    = cpu_rdtsc() | 1;
    CheerAt = clock_uptime_us();
    Confetti.clear();
    auto Roll = [&]() {
        Seed ^= Seed << 13;
        Seed ^= Seed >> 7;
        Seed ^= Seed << 17;
        return Seed;
    };
    const painter::core::Size Dimensions = Stage->Size();
    for (int Spark = Dimensions.Height * Dimensions.Width / CellsPerSpark; Spark > 0; Spark--) {
        Confetti.push_back({static_cast<int>(2 + Roll() % static_cast<std::uint64_t>(std::max(1, Dimensions.Height - 3))),
                            static_cast<int>(Roll() % static_cast<std::uint64_t>(Dimensions.Width)),
                            SparkInks[Roll() % (sizeof(SparkInks) / sizeof(SparkInks[0]))],
                            Roll() % (TodoCheerUs * 2 / 3)});
    }
}

/* Done, or not done again. The thing marked done glows where it goes; the
 * last one done here sets off the cheer. */
void TodoDesk::Toggle(std::size_t At)
{
    TodoItem &Item = List.Items[At];
    Item.Done = !Item.Done;
    Keep();
    if (!Item.Done) return;
    Flashed = At;
    FlashAt = clock_uptime_us();
    std::vector<std::size_t> Places = List.View(Here());
    if (std::all_of(Places.begin(), Places.end(), [&](std::size_t Place) { return List.Items[Place].Done; })) Cheer();
}

/* A key while words are typed: true once they are done with. */
bool TodoDesk::Type(const KeyEvent &Key)
{
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:   if (Caret > 0) Caret--; break;
        case painter::tui::KeyCodes::Right:  if (Caret < Line.size()) Caret++; break;
        case painter::tui::KeyCodes::Home:   Caret = 0; break;
        case painter::tui::KeyCodes::End:    Caret = Line.size(); break;
        case painter::tui::KeyCodes::Delete: if (Caret < Line.size()) Line.erase(Caret, 1); break;
        default: break;
        }
        return false;
    }
    char Ch = Key.Ascii;
    if (Ch == painter::tui::KeyCodes::Escape) return true;
    if (Ch == '\n' || Ch == '\r') {
        std::vector<std::size_t> Places = List.View(Here());
        if (Now == TodoMode::Adding) {
            std::size_t Before = List.Items.size();
            List.Add(Line, Here());
            if (List.Items.size() > Before) {
                Keep();
                Places   = List.View(Here());
                Selected = static_cast<std::size_t>(std::find(Places.begin(), Places.end(), Before) - Places.begin());
            }
        } else if (Selected < Places.size() && Line.find_first_not_of(' ') != std::string::npos) {
            List.Items[Places[Selected]].Text = Line;
            Keep();
        }
        return true;
    }
    if (Ch == '\b' || Ch == 0x7F) {
        if (Caret > 0) Line.erase(--Caret, 1);
    } else if (static_cast<unsigned char>(Ch) >= 0x20 && static_cast<unsigned char>(Ch) < 0x7F) {
        Line.insert(Caret++, 1, Ch);
    }
    return false;
}

/* A key on the list itself, but the one that leaves. */
void TodoDesk::Browse(const KeyEvent &Key, const std::vector<std::size_t> &Places)
{
    bool Some = Selected < Places.size();
    if (Key.IsExtended()) {
        bool Shift = Key.IsShift();
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Up:     if (Shift) Move(-1); else if (Selected > 0) Selected--; break;
        case painter::tui::KeyCodes::Down:   if (Shift) Move(1); else if (Selected + 1 < Places.size()) Selected++; break;
        case painter::tui::KeyCodes::Home:   Selected = 0; break;
        case painter::tui::KeyCodes::End:    Selected = Places.empty() ? 0 : Places.size() - 1; break;
        case painter::tui::KeyCodes::Delete:
            if (Some) {
                List.Items.erase(List.Items.begin() + static_cast<std::ptrdiff_t>(Places[Selected]));
                Keep();
            }
            break;
        default: break;
        }
        return;
    }
    char Ch = Key.Ascii;
    if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    if (Ch == '\t') {
        List.Everywhere = !List.Everywhere;
        Selected        = 0;
        Keep();
    } else if (Ch == 'a') {
        Now   = TodoMode::Adding;
        Line.clear();
        Caret = 0;
    } else if (!Some) {
        return;
    } else if (Ch == ' ') {
        Toggle(Places[Selected]);
    } else if (Ch == 's') {
        List.Items[Places[Selected]].Star = !List.Items[Places[Selected]].Star;
        Keep();
    } else if (Ch == '\n' || Ch == '\r') {
        Now   = TodoMode::Changing;
        Line  = List.Items[Places[Selected]].Text;
        Caret = Line.size();
    } else if (Ch == 'd') {
        List.Items.erase(List.Items.begin() + static_cast<std::ptrdiff_t>(Places[Selected]));
        Keep();
    }
}

bool TodoDesk::OnKey(const KeyEvent &Key)
{
    Note.clear();
    if (Now != TodoMode::Browse) {
        if (Type(Key)) Now = TodoMode::Browse;
        return true;
    }
    if (!Key.IsExtended() && (Key.Ascii == 'q' || Key.Ascii == 'Q' || Key.Ascii == painter::tui::KeyCodes::Escape)) return false;
    std::size_t Before = List.Items.size();
    Browse(Key, List.View(Here()));
    if (List.Items.size() < Before) Flashed = static_cast<std::size_t>(-1);
    return true;
}

std::uint32_t TodoDesk::Settle() const
{
    std::uint64_t Time   = clock_uptime_us();
    bool          Moving = (Flashed != static_cast<std::size_t>(-1) && Time - FlashAt < TodoFlashUs) ||
                           (CheerAt != 0 && Time - CheerAt < TodoCheerUs);
    return Moving ? 33 : 0;
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        TodoDesk Todo;
        Todo.Context = KeepContext();
        if (std::optional<std::string> Refusal = Todo.List.Load()) {
            Say("todo: the list was not read: " + *Refusal);
            return 1;
        }

        if (Count > 0) {
            const char *Text = form_text(0);
            Todo.List.Add(Text ? Text : "", Todo.Here());
            if (std::optional<std::string> Refusal = Todo.List.Save()) {
                Say("todo: not kept: " + *Refusal);
                return 1;
            }
            const TodoItem &Added = Todo.List.Items.back();
            Say(Added.Context.empty() ? std::format("todo: {}", Added.Text)
                                      : std::format("todo: {}  (for {})", Added.Text, Added.Context));
            return 0;
        }

        /* Under an arrow the list is text for the file, as the screen would show it. */
        if (form_screen_file() != 0) {
            std::string Text = TodoText(Todo.List, Todo.List.View(Todo.Here()));
            print_bytes(Text.data(), Text.size());
            return 0;
        }

        std::expected<Stage, StageError> Opened = Stage::Open(40, 8);
        if (!Opened) {
            Say(Opened.error().Describe("todo"));
            return 1;
        }
        Todo.Stage = std::move(*Opened);
        bool Painted = true;
        for (;;) {
            Todo.Draw();
            if (!(Painted = static_cast<bool>(Todo.Stage->Present()))) break;
            const painter::tui::Event Event = Todo.Stage->Wait(Todo.Settle());
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind == EventKind::Key && !Todo.OnKey(Event.Key)) break;
        }
        (void)Todo.Stage->Close();
        std::vector<std::size_t> Places = Todo.List.View(Todo.Here());
        std::size_t Left = static_cast<std::size_t>(
            std::count_if(Places.begin(), Places.end(), [&](std::size_t At) { return !Todo.List.Items[At].Done; }));
        Say(std::format("[TODO] {} to do{}", Left, Todo.Here().empty() ? "" : " here"));
        if (!Todo.Note.empty()) Say("todo: " + Todo.Note);
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for the list.");
        return 1;
    }
}
