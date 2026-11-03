#ifndef TODO_H
#define TODO_H

/* todo — what there is to do, each thing with the Use Context it was said in.
 *
 * todo_list.cpp is the list and its file; todo.cpp the Form, the glass and
 * the keys. The list lives on the volume in todo.list (keep.h), a line to a
 * thing: "x" done or "-" not, a tab, the Use Context's tags, a tab, the thing
 * as it was said. A thing is the person's own words, so a tab or a line end in
 * them is kept as a blank. A first line "everywhere" says the list is kept
 * flat (TodoList::Everywhere). */

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TodoItem {
    bool        Done = false;
    std::string Context;    /* comma-joined tags, "" for none */
    std::string Text;
};

class TodoList {
public:
    std::optional<std::string> Load();
    std::optional<std::string> Save() const;

    void Add(std::string_view Text, std::string_view Context);

    /* What belongs to `Context`: a thing said in no context belongs to every
     * one, and with no context said everything belongs. */
    static bool Belongs(const TodoItem &Item, std::string_view Context);

    /* The list flat: every thing shown whatever the Use Context, and a new one
     * said in none. Kept with the list, so it stays the way it was left. */
    bool Everywhere = false;

    std::vector<TodoItem> Items;
};

/* The words of a list as text: "[ ] thing  (context)", a line to a thing. */
std::string TodoText(const std::vector<TodoItem> &Items);

#endif
