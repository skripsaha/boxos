/* todo_list.cpp — the list and its file (todo.h). */

#include "keep.h"
#include "todo.h"

namespace {

const Keep File{"todo.list", "todo,list"};

/* The person's words on one line of the file: no tab, no line end in them. */
std::string Flat(std::string_view Text)
{
    std::string Line(Text);
    for (char &Ch : Line)
        if (Ch == '\t' || Ch == '\n' || Ch == '\r') Ch = ' ';
    return Line;
}

/* Whether the comma-joined `Tags` hold `Tag`. */
bool Holds(std::string_view Tags, std::string_view Tag)
{
    while (!Tags.empty()) {
        std::size_t      Comma = Tags.find(',');
        std::string_view One   = Tags.substr(0, Comma);
        if (One == Tag) return true;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return false;
}

}

std::optional<std::string> TodoList::Load()
{
    Items.clear();
    Everywhere = false;
    std::string Text;
    if (std::optional<std::string> Refusal = KeepRead(File, Text)) return Refusal;

    std::string_view Rest = Text;
    while (!Rest.empty()) {
        std::size_t      End  = Rest.find('\n');
        std::string_view Line = Rest.substr(0, End);
        Rest.remove_prefix(End == std::string_view::npos ? Rest.size() : End + 1);

        if (Line == "everywhere") {
            Everywhere = true;
            continue;
        }
        std::size_t First = Line.find('\t');
        std::size_t Second = First == std::string_view::npos ? First : Line.find('\t', First + 1);
        if (Second == std::string_view::npos) continue;
        TodoItem Item;
        Item.Done    = Line.substr(0, First) == "x";
        Item.Context = std::string(Line.substr(First + 1, Second - First - 1));
        Item.Text    = std::string(Line.substr(Second + 1));
        if (!Item.Text.empty()) Items.push_back(std::move(Item));
    }
    return std::nullopt;
}

std::optional<std::string> TodoList::Save() const
{
    std::string Text = Everywhere ? "everywhere\n" : "";
    for (const TodoItem &Item : Items) {
        Text += Item.Done ? "x\t" : "-\t";
        Text += Flat(Item.Context);
        Text += '\t';
        Text += Flat(Item.Text);
        Text += '\n';
    }
    return KeepWrite(File, Text);
}

void TodoList::Add(std::string_view Text, std::string_view Context)
{
    std::size_t Begin = Text.find_first_not_of(' ');
    if (Begin == std::string_view::npos) return;
    std::size_t End = Text.find_last_not_of(' ');
    Items.push_back({false, std::string(Context), Flat(Text.substr(Begin, End - Begin + 1))});
}

bool TodoList::Belongs(const TodoItem &Item, std::string_view Context)
{
    if (Context.empty() || Item.Context.empty()) return true;
    std::string_view Tags = Item.Context;
    while (!Tags.empty()) {
        std::size_t Comma = Tags.find(',');
        if (Holds(Context, Tags.substr(0, Comma))) return true;
        if (Comma == std::string_view::npos) break;
        Tags.remove_prefix(Comma + 1);
    }
    return false;
}

std::string TodoText(const std::vector<TodoItem> &Items)
{
    std::string Text;
    for (const TodoItem &Item : Items) {
        Text += Item.Done ? "[x] " : "[ ] ";
        Text += Item.Text;
        if (!Item.Context.empty()) Text += "  (" + Item.Context + ")";
        Text += '\n';
    }
    return Text;
}
