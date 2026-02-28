/*
 * procb - BoxOS IPC Demo: Process B
 *
 * Sends the letter 'B' via IPC-based print(),
 * 5 times, yielding between each send.
 */
#include "box/io.h"
#include "box/system.h"

int main(void)
{
    for (int i = 0; i < 5; i++)
    {
        print("B");
        yield();
    }

    println("");
    exit(0);
}
