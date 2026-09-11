#include "box/print.h"
#include "box/ipc.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/system.h"

int main(void)
{
    CabinInfo *ci = cabin_info();
    uint32_t spawner = ci->spawner_pid;

    for (int i = 0; i < 5; i++)
    {
        print("A");

        if (spawner)
        {
            send(spawner, "A", 1);
        }

        yield();
    }
    print("\n");

    exit(0);
}