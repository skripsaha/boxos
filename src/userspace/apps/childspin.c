#include "box/system.h"

/* An eternal child: yields forever so a supervisor can drive deterministic
 * kill / wait-timeout / multi-child demux tests against a process that never
 * exits on its own. */
int main(void)
{
    for (;;)
        yield();
}
