/* todo_list.cpp — the list and its file (todo.h). */

#include <algorithm>

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

}

std::vector<std::string_view> TodoTags(std::string_view Context)
{
    std::vector<std::string_view> Tags;
    while (!Context.empty()) {
        std::size_t      Comma = Context.find(',');
        std::string_view One   = Context.substr(0, Comma);
        std::size_t      Begin = One.find_first_not_of(' '), End = One.find_last_not_of(' ');
        if (Begin != std::string_view::npos) Tags.push_back(One.substr(Begin, End - Begin + 1));
        if (Comma == std::string_view::npos) break;
        Context.remove_prefix(Comma + 1);
    }
    return Tags;
}

int TodoRank(const TodoItem &Item)
{
    return Item.Done ? 2 : Item.Star ? 0 : 1;
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
        std::size_t First  = Line.find('\t');
        std::size_t Second = First == std::string_view::npos ? First : Line.find('\t', First + 1);
        if (Second == std::string_view::npos) continue;
        std::string_view State = Line.substr(0, First);
        TodoItem         Item;
        Item.Done    = State.starts_with('x');
        Item.Star    = State.find('*') != std::string_view::npos;
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
        Text += Item.Done ? 'x' : '-';
        if (Item.Star) Text += '*';
        Text += '\t';
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
    Items.push_back({false, false, std::string(Context), Flat(Text.substr(Begin, End - Begin + 1))});
}

bool TodoList::Belongs(const TodoItem &Item, std::string_view Context)
{
    if (Context.empty() || Item.Context.empty()) return true;
    std::vector<std::string_view> Here = TodoTags(Context);
    for (std::string_view Tag : TodoTags(Item.Context))
        if (std::find(Here.begin(), Here.end(), Tag) != Here.end()) return true;
    return false;
}

std::vector<std::size_t> TodoList::View(std::string_view Context) const
{
    std::vector<std::size_t> Places;
    for (int Rank = 0; Rank < 3; Rank++)
        for (std::size_t At = 0; At < Items.size(); At++)
            if (TodoRank(Items[At]) == Rank && Belongs(Items[At], Context)) Places.push_back(At);
    return Places;
}

std::string TodoText(const TodoList &List, const std::vector<std::size_t> &Places)
{
    std::string Text;
    for (std::size_t At : Places) {
        const TodoItem &Item = List.Items[At];
        Text += Item.Done ? "[x] " : "[ ] ";
        if (Item.Star) Text += "* ";
        Text += Item.Text;
        std::vector<std::string_view> Tags = TodoTags(Item.Context);
        for (std::size_t Tag = 0; Tag < Tags.size(); Tag++) {
            Text += Tag == 0 ? "  (" : " ";
            Text += Tags[Tag];
        }
        if (!Tags.empty()) Text += ')';
        Text += '\n';
    }
    return Text;
}
