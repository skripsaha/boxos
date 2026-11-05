#ifndef TODO_DESK_H
#define TODO_DESK_H

/* The list on the glass (todo.h): the keys in todo.cpp, the picture in
 * todo_view.cpp. A thing marked done flashes; the last one done here sets off
 * a little cheer. Neither asks for a frame once it is over: the desk waits on
 * the keys alone then (Settle). */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/stage.h"
#include "todo.h"

constexpr std::uint64_t TodoFlashUs = 450000;    /* a thing marked done glows this long */
constexpr std::uint64_t TodoCheerUs = 1800000;   /* and the cheer lasts this long */

enum class TodoMode { Browse, Adding, Changing };

struct TodoSpark {
    int           Row, Col;
    painter::core::Color Ink;
    std::uint64_t Born;   /* microseconds into the cheer */
};

class TodoDesk {
public:
    std::optional<painter::tui::Stage> Glass;
    TodoList                         List;
    std::string Context;   /* the Use Context */
    std::string Note;      /* said on the bar until the next key */

    /* The context things are shown and said in: none when the list is flat. */
    std::string_view Here() const { return List.Everywhere ? std::string_view{} : std::string_view{Context}; }

    /* False to leave. */
    bool OnKey(const painter::tui::KeyEvent &Key);
    void Draw();
    /* How long the keys may be waited for before the picture changes by
     * itself, in milliseconds; 0 when it will not. */
    std::uint32_t Settle() const;

private:
    void Keep();
    void Move(int Way);
    void Toggle(std::size_t At);
    bool Type(const painter::tui::KeyEvent &Key);
    void Browse(const painter::tui::KeyEvent &Key, const std::vector<std::size_t> &Places);
    void Cheer();

    void Head(const std::vector<std::size_t> &Places);
    void Thing(int Row, const TodoItem &Item, std::size_t At, bool Chosen);
    void Empty();
    void Sparks();
    void Bar();

    std::size_t            Selected = 0;   /* among what is shown */
    std::size_t            Scroll   = 0;
    TodoMode               Now      = TodoMode::Browse;
    std::string            Line;
    std::size_t            Caret    = 0;
    std::size_t            Flashed  = static_cast<std::size_t>(-1);   /* the place just marked done */
    std::uint64_t          FlashAt  = 0;
    std::uint64_t          CheerAt  = 0;
    std::vector<TodoSpark> Confetti;
    std::uint64_t          Seed     = 1;
};

#endif
