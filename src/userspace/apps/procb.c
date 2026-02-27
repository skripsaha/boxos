/*
 * procb - BoxOS IPC Demo: Process B
 *
 * Sends the letter 'B' to the shell process via IPC broadcast,
 * 5 times, yielding between each send to allow interleaving.
 */
#include "box/ipc.h"
#include "box/system.h"

int main(void) {
    const char msg[1] = {'B'};

    for (int i = 0; i < 5; i++) {
        broadcast("shell", msg, 1);
        yield();
    }

    exit(0);
    while (1) {}
    return 0;
}
