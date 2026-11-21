/* main.cpp — the hub: where the line typed, the programs on the volume and
 * the Use Context meet.
 *
 * Started by the system it begins on a clear screen with its one line of
 * greeting; started from another hub, or raised again by the system after it
 * fell (its life says rise), it keeps what is on the screen — the kernel's
 * word of the fall among it. */

#include "box/core/cabin.h"
#include "box/life.h"
#include "box/print.h"
#include "boxos_mascot.h"
#include "boxos_symbols.h"

#include "hub.h"
#include "programs.h"
#include "say.h"
#include "session.h"

BOX_LIFE("task\nrise");

int main()
{
    hub::Hub Self;
    Self.Shown = hub::ReadLook();
    if (!Self.Started.Begin(Self.Shown)) return 1;
    Self.Lines.Begin();

    if (cabin_info()->spawner_pid == 0 && cabin_info()->raised == 0) clear();
    if (Self.Shown.Greeting) {
        hub::SayIn(BOXOS_DUPPO_PINK, BOXOS_SYMBOL_DUPPO_TEXT);
        hub::Say(" BoxOS Hub — help shows what there is\n");
    }

    // The commands found before the first line, so its first key is not
    // what reads the volume to find them.
    hub::programs::EachCommand([](file_id_t, std::string_view) {});
    if (!hub::Converse(Self)) {
        hub::Say("hub: no keyboard can be heard here, so nothing can be typed; the hub ends\n");
        return 1;
    }
    return 0;
}
