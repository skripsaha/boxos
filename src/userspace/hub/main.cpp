/* main.cpp — the hub: where the line typed, the programs on the volume and
 * the Use Context meet.
 *
 * Started by the system it begins on a clear screen with its one line of
 * greeting; started from another hub it keeps what is on the screen. */

#include "box/core/cabin.h"
#include "box/print.h"

#include "hub.h"
#include "say.h"
#include "session.h"

int main()
{
    hub::Hub Self;
    Self.Shown = hub::ReadLook();
    Self.Started.Begin();
    Self.Lines.Begin();

    if (cabin_info()->spawner_pid == 0) clear();
    if (Self.Shown.Greeting) hub::Say("BoxOS Hub — help shows what there is\n");

    if (!hub::Converse(Self)) {
        hub::Say("hub: no keyboard can be heard here, so nothing can be typed; the hub ends\n");
        return 1;
    }
    return 0;
}
