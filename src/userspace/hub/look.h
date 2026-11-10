/* look.h — how the person set the hub: its settings (hub.settings), read
 * through box::settings as they hold in the Use Context, and read again as
 * soon as the file is saved or the context changes. */
#ifndef HUB_LOOK_H
#define HUB_LOOK_H

#include <cstdint>
#include <string>

#include "box/color.h"

namespace hub {

// Where the hub shows how many programs live behind: before the sign, on the
// line at the top of the screen that names them (windows.h), or nowhere.
enum class BehindShown { Prompt, Line, Nowhere };

struct Look {
    bool         Greeting;
    std::string  Sign;
    bool         Context;
    bool         Them;
    Color        SignColor;
    Color        ContextColor;
    Color        ThemColor;
    BehindShown  Behind;
    Color        BehindColor;
    Color        NewsColor;
    Color        RefusalColor;
    bool         Ended;
    bool         Finished;          // a program behind that finished is said
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
