#ifndef TODO_H
#define TODO_H

/* todo — what there is to do, each thing with the Use Context it was said in.
 *
 * todo_list.cpp is the list and its file; todo_desk.h the screen of it, laid
 * out by todo_view.cpp and driven by todo.cpp, which is the Form and the keys
 * too. The list lives on the volume in todo.list (keep.h), a line to a thing:
 * "x" done or "-" not, a "*" after it for a starred one, a tab, the Use
 * Context's tags, a tab, the thing as it was said. A thing is the person's own
 * words, so a tab or a line end in them is kept as a blank. A first line
 * "everywhere" says the list is kept flat (TodoList::Everywhere). */

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TodoItem {
    bool        Done = false;
    bool        Star = false;   /* to be done first */
    std::string Context;        /* comma-joined tags, "" for none */
    std::string Text;
};

class TodoList {
public:
    std::optional<std::string> Load();
    std::optional<std::string> Save() const;

    void Add(std::string_view Text, std::string_view Context);

    /* What belongs to `Context`: a thing that shares a tag with it, and a
     * thing said in no context, which belongs to every one; with no context
     * said, everything. */
    static bool Belongs(const TodoItem &Item, std::string_view Context);

    /* What is shown for `Context`, as places in Items: the starred still to
     * do, the rest still to do, then what is done, each in the list's order. */
    std::vector<std::size_t> View(std::string_view Context) const;

    /* The list flat: every thing shown whatever the Use Context, and a new one
     * said in none. Kept with the list, so it stays the way it was left. */
    bool Everywhere = false;

    std::vector<TodoItem> Items;
};

/* Where a thing stands in View: 0 starred, 1 to do, 2 done. */
int TodoRank(const TodoItem &Item);

/* The tags of a context one by one, as they are said: "code,cpp" is code, cpp. */
std::vector<std::string_view> TodoTags(std::string_view Context);

/* The things at `Places` as text: "[ ] * thing  (code cpp)", a line to a thing. */
std::string TodoText(const TodoList &List, const std::vector<std::size_t> &Places);

#endif
