/* look.h — how the person set the hub: its settings (hub.settings), read
 * through box::settings, and read again as soon as the file is saved. */
#ifndef HUB_LOOK_H
#define HUB_LOOK_H

#include <cstdint>
#include <string>

#include "box/color.h"

namespace hub {

struct Look {
    bool         Greeting;
    std::string  Sign;
    bool         Context;
    bool         Them;
    Color        SignColor;
    Color        ContextColor;
    Color        ThemColor;
    Color        NewsColor;
    Color        RefusalColor;
    bool         Ended;
    std::int64_t LongCommand;       // seconds; 0: never said
    bool         Whisper;
    bool         Needs;
    bool         DidYouMean;
    bool         History;
    bool         RecallHereFirst;   // Up brings back this Use Context's lines first
};

Look ReadLook();

}  // namespace hub

#endif
