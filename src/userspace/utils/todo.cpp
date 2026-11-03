/* todo — what there is to do.
 *
 *   todo                   the list, on the screen: what belongs to the Use Context
 *   todo buy milk          buy milk is added, in the Use Context it is said in
 *   todo -> plans.txt      the list as text into plans.txt
 *
 * On the screen:
 *   up, down               choose          shift+up, shift+down   move it
 *   space                  done, or not done again
 *   a                      add             enter     change its words
 *   d                      take it away    tab       everywhere, or here again
 *   q, ESC                 leave
 *
 * A thing is said in a Use Context and belongs to it: with `use code` said,
 * the list is what there is to do for code, and what was said in no context
 * belongs everywhere. Tab makes the list flat — everywhere: every thing on it
 * shown, and a new one said in no context — and back; it stays the way it was
 * left. The list is kept on the volume (todo.h) and every change is settled
 * there before the next key is read. */

#include <algorithm>
#include <format>
#include <new>
#include <string>

#include "box/form.h"
#include "box/memory.h"
#include "box/print.h"

#include "keep.h"
#include "stage.h"
#include "todo.h"

BOX_FORM("takes optional text \"a thing to do; with none, the list on the screen\"\n");

namespace {

const Color Paper    = COLOR_RGB(0x12, 0x12, 0x12);
const Color BarPaper = COLOR_RGB(0x3A, 0x3A, 0x3A);
const Color Chosen   = COLOR_RGB(0x2E, 0x2E, 0x2E);
const Color TitleInk = COLOR_AMBER;
const Color TextInk  = COLOR_RGB(0xEE, 0xEE, 0xEE);
const Color DoneInk  = COLOR_RGB(0x6E, 0x6E, 0x6E);
const Color TickInk  = COLOR_RGB(0x60, 0xE0, 0x70);
const Color HintInk  = COLOR_RGB(0x8C, 0x8C, 0x8C);
const Color KeyInk   = COLOR_AMBER;
const Color BadInk   = COLOR_RGB(0xFF, 0x70, 0x70);
const Color TagInk   = COLOR_RGB(0x60, 0xA0, 0xE0);

enum class Mode { Browse, Adding, Changing };

class Desk {
public:
    Stage       Glass;
    TodoList    List;
    std::string Context;   /* the Use Context */
    std::string Note;      /* said on the bar until the next key */

    /* The context things are shown and said in: none when the list is flat. */
    std::string_view Here() const { return List.Everywhere ? std::string_view{} : std::string_view{Context}; }

    bool OnKey(const kb_event_t &Key);
    void Draw();

private:
    std::vector<std::size_t> Shown() const;
    void Keep();
    void Move(int Way);
    bool Type(const kb_event_t &Key);
    void Bar();

    std::size_t Selected = 0;      /* among what is shown */
    std::size_t Scroll   = 0;
    Mode        Now      = Mode::Browse;
    std::string Line;
    std::size_t Caret    = 0;
};

/* What is shown, as places in the list: what is still to do first, what is
 * done after it, each in the list's own order. */
std::vector<std::size_t> Desk::Shown() const
{
    std::vector<std::size_t> Places;
    for (int Done = 0; Done < 2; Done++)
        for (std::size_t At = 0; At < List.Items.size(); At++)
            if (List.Items[At].Done == (Done == 1) && TodoList::Belongs(List.Items[At], Here()))
                Places.push_back(At);
    return Places;
}

void Desk::Keep()
{
    if (std::optional<std::string> Refusal = List.Save()) Note = "not kept: " + *Refusal;
}

/* The chosen thing swapped with the one shown before or after it, when that
 * one is as done as it is. */
void Desk::Move(int Way)
{
    std::vector<std::size_t> Places = Shown();
    if (Places.empty()) return;
    std::size_t Other = Way < 0 ? Selected - 1 : Selected + 1;
    if ((Way < 0 && Selected == 0) || Other >= Places.size()) return;
    TodoItem &Here = List.Items[Places[Selected]], &There = List.Items[Places[Other]];
    if (Here.Done != There.Done) return;
    std::swap(Here, There);
    Selected = Other;
    Keep();
}

/* A key while words are typed: true once they are done with. */
bool Desk::Type(const kb_event_t &Key)
{
    if (Key.mods & KB_MOD_EXTENDED) {
        switch (Key.scancode) {
        case KEY_LEFT:   if (Caret > 0) Caret--; break;
        case KEY_RIGHT:  if (Caret < Line.size()) Caret++; break;
        case KEY_HOME:   Caret = 0; break;
        case KEY_END:    Caret = Line.size(); break;
        case KEY_DELETE: if (Caret < Line.size()) Line.erase(Caret, 1); break;
        default: break;
        }
        return false;
    }
    char Ch = Key.ascii;
    if (Ch == KEY_ESCAPE) return true;
    if (Ch == '\n' || Ch == '\r') {
        std::vector<std::size_t> Places = Shown();
        if (Now == Mode::Adding) {
            std::size_t Before = List.Items.size();
            List.Add(Line, Here());
            if (List.Items.size() > Before) {
                Keep();
                Places = Shown();
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

/* False to leave. */
bool Desk::OnKey(const kb_event_t &Key)
{
    Note.clear();
    if (Now != Mode::Browse) {
        if (Type(Key)) Now = Mode::Browse;
        return true;
    }
    std::vector<std::size_t> Places = Shown();
    if (Key.mods & KB_MOD_EXTENDED) {
        bool Shift = (Key.mods & KB_MOD_SHIFT) != 0;
        if (Key.scancode == KEY_UP) {
            if (Shift) Move(-1);
            else if (Selected > 0) Selected--;
        } else if (Key.scancode == KEY_DOWN) {
            if (Shift) Move(1);
            else if (Selected + 1 < Places.size()) Selected++;
        } else if (Key.scancode == KEY_HOME) {
            Selected = 0;
        } else if (Key.scancode == KEY_END) {
            Selected = Places.empty() ? 0 : Places.size() - 1;
        } else if (Key.scancode == KEY_DELETE && Selected < Places.size()) {
            List.Items.erase(List.Items.begin() + static_cast<std::ptrdiff_t>(Places[Selected]));
            Keep();
        }
        return true;
    }
    char Ch = Key.ascii;
    if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    switch (Ch) {
    case 'q':
    case KEY_ESCAPE: return false;
    case '\t':
        List.Everywhere = !List.Everywhere;
        Selected        = 0;
        Keep();
        break;
    case ' ':
        if (Selected < Places.size()) {
            TodoItem &Item = List.Items[Places[Selected]];
            Item.Done = !Item.Done;
            Keep();
        }
        break;
    case 'a':
        Now   = Mode::Adding;
        Line.clear();
        Caret = 0;
        break;
    case '\n':
    case '\r':
        if (Selected < Places.size()) {
            Now   = Mode::Changing;
            Line  = List.Items[Places[Selected]].Text;
            Caret = Line.size();
        }
        break;
    case 'd':
        if (Selected < Places.size()) {
            List.Items.erase(List.Items.begin() + static_cast<std::ptrdiff_t>(Places[Selected]));
            Keep();
        }
        break;
    default: break;
    }
    return true;
}

void Desk::Bar()
{
    int Row = Glass.Rows - 1;
    Glass.Fill(Row, 0, Glass.Cols, ' ', TextInk, BarPaper);
    if (Now != Mode::Browse) {
        int Col = Glass.Write(Row, 1, Now == Mode::Adding ? "to do: " : "change: ", KeyInk, BarPaper);
        int Room = Glass.Cols - Col - 1;
        std::size_t From = Caret > static_cast<std::size_t>(Room) ? Caret - static_cast<std::size_t>(Room) : 0;
        Glass.Write(Row, Col, std::string_view(Line).substr(From, static_cast<std::size_t>(Room)), TextInk, BarPaper);
        Glass.Put(Row, Col + static_cast<int>(Caret - From), Caret < Line.size() ? Line[Caret] : ' ', BarPaper, TextInk);
        return;
    }
    if (!Note.empty()) {
        Glass.Write(Row, 1, Note, BadInk, BarPaper);
        return;
    }
    int Col = 1;
    for (auto [Keys, What] : {std::pair{"\x18\x19", "choose"}, {"space", "done"}, {"a", "add"}, {"enter", "change"},
                              {"d", "away"}, {"shift+\x18\x19", "move"},
                              {"tab", List.Everywhere ? "here" : "everywhere"}, {"q", "leave"}}) {
        int Wide = static_cast<int>(std::string_view(Keys).size() + 1 + std::string_view(What).size() + 2);
        if (Col + Wide > Glass.Cols) break;
        Col = Glass.Write(Row, Col, Keys, KeyInk, BarPaper);
        Col = Glass.Write(Row, Col + 1, What, HintInk, BarPaper) + 2;
    }
}

void Desk::Draw()
{
    Glass.Clear(Paper);
    std::vector<std::size_t> Places = Shown();
    if (!Places.empty()) Selected = std::min(Selected, Places.size() - 1);

    std::size_t Left = 0, Done = 0;
    for (std::size_t At : Places) (List.Items[At].Done ? Done : Left)++;
    int Col = Glass.Write(0, 1, "todo", TitleInk, Paper);
    Glass.Write(0, Col + 2, List.Everywhere ? "everywhere" : Context.empty() ? "everything" : "for " + Context, TagInk,
                Paper);
    std::string Counts = std::format("{} to do, {} done", Left, Done);
    Glass.Write(0, Glass.Cols - 1 - static_cast<int>(Counts.size()), Counts, HintInk, Paper);

    int First = 2, Room = Glass.Rows - 1 - First;
    if (Places.empty()) {
        Glass.Centre(Glass.Rows / 2, "nothing to do here; a adds a thing", HintInk, Paper);
    } else {
        if (Selected < Scroll) Scroll = Selected;
        if (Selected >= Scroll + static_cast<std::size_t>(Room)) Scroll = Selected - static_cast<std::size_t>(Room) + 1;
    }
    for (int Row = 0; Row < Room && Scroll + static_cast<std::size_t>(Row) < Places.size(); Row++) {
        std::size_t     Index  = Scroll + static_cast<std::size_t>(Row);
        const TodoItem &Item   = List.Items[Places[Index]];
        bool            Here   = Index == Selected;
        Color           Back   = Here ? Chosen : Paper;
        int             Y      = First + Row;
        Glass.Fill(Y, 0, Glass.Cols, ' ', TextInk, Back);
        if (Here) Glass.Put(Y, 1, Glyph::Pointer, TitleInk, Back);
        Glass.Write(Y, 3, "[ ]", HintInk, Back);
        if (Item.Done) Glass.Put(Y, 4, Glyph::Tick, TickInk, Back);

        int Tag  = Item.Context.empty() ? 0 : static_cast<int>(Item.Context.size()) + 2;
        int Wide = Glass.Cols - 8 - Tag;
        std::string Text = Item.Text;
        if (static_cast<int>(Text.size()) > Wide) Text = Text.substr(0, static_cast<std::size_t>(std::max(0, Wide - 3))) + "...";
        Glass.Write(Y, 7, Text, Item.Done ? DoneInk : TextInk, Back);
        if (Tag) Glass.Write(Y, Glass.Cols - Tag, Item.Context, Item.Done ? DoneInk : TagInk, Back);
    }
    Bar();
}

}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        Desk Todo;
        Todo.Context = KeepContext();
        if (std::optional<std::string> Refusal = Todo.List.Load()) {
            StageSay("todo: the list was not read: " + *Refusal);
            return 1;
        }

        if (Count > 0) {
            const char *Text = form_text(0);
            Todo.List.Add(Text ? Text : "", Todo.Here());
            if (std::optional<std::string> Refusal = Todo.List.Save()) {
                StageSay("todo: not kept: " + *Refusal);
                return 1;
            }
            const TodoItem &Added = Todo.List.Items.back();
            StageSay(Added.Context.empty() ? std::format("todo: {}", Added.Text)
                                           : std::format("todo: {}  (for {})", Added.Text, Added.Context));
            return 0;
        }

        /* Under an arrow the list is text for the file, not a screen. */
        if (form_screen_file() != 0) {
            std::string Text = TodoText(Todo.List.Items);
            print_bytes(Text.data(), Text.size());
            return 0;
        }

        if (std::optional<std::string> Refusal = Todo.Glass.Take("todo", 40, 8)) {
            Todo.Glass.Give();
            StageSay(*Refusal);
            return 1;
        }
        bool Painted = true;
        for (;;) {
            Todo.Draw();
            if (!(Painted = Todo.Glass.Show())) break;
            kb_event_t Key;
            StageHeard Heard = Todo.Glass.Listen(Key, 0);
            if (Heard == StageHeard::Gone) break;
            if (Heard == StageHeard::Key && !Todo.OnKey(Key)) break;
        }
        Todo.Glass.Give();
        std::size_t Left = 0;
        for (const TodoItem &Item : Todo.List.Items) Left += !Item.Done;
        StageSay(std::format("[TODO] {} to do", Left));
        if (!Todo.Note.empty()) StageSay("todo: " + Todo.Note);
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        StageSay("There is not enough memory for the list.");
        return 1;
    }
}
