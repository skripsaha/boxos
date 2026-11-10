/* look.h — how the person set the hub: its settings (hub.settings), read
 * through box::settings as they hold in the Use Context, and read again as
 * soon as the file is saved or the context changes. */
#ifndef HUB_LOOK_H
#define HUB_LOOK_H

#include <cstdint>
#include <string>

#include "box/color.h"
#include "box/types.h"
#include "key_chord.h"

namespace hub {

// Where the hub shows how many programs live behind: before the sign, on the
// line at the top of the screen that names them (windows.h), or nowhere.
enum class BehindShown { Prompt, Line, Nowhere };

// Which of them the key next brings first (circle.h): the one gone behind
// last, the one gone behind first, or the one on the screen last.
enum class BehindOrder { NewestFirst, OldestFirst, LastShown };

// The keys the hub acts on over every program, in the order the display is
// told them and tells them back (box/display.h DISP_CMD_KEYS).
enum HubKey : std::uint8_t { KeyStop, KeyLetGo, KeyNext, KeyCount };

struct Look {
    bool         Greeting;
    Color        Text;              // the screen's own, told to the display (box/display.h DISP_CMD_PAGE)
    Color        Page;
    std::string  Sign;
    bool         Context;
    bool         Them;
    Color        SignColor;
    Color        ContextColor;
    Color        ThemColor;
    BehindShown  Behind;
    Color        BehindColor;
    BehindOrder  Order;
    bool         BehindContextFirst;   // those gone behind in this Use Context come first
    KeyChord     Keys[KeyCount];
    Color        NewsColor;
    Color        RefusalColor;
    bool         Ended;
    std::int64_t LongCommand;       // seconds; 0: never said
    bool         Whisper;
    bool         Needs;
    bool         DidYouMean;
    bool         History;
    bool         RecallHereFirst;   // Up brings back this Use Context's lines first
    bool         ChooseInWindow;    // which one is meant is asked in a window, not a list
};

Look ReadLook();

}  // namespace hub

#endif
