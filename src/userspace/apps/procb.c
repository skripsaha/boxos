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
        print("B");

        if (spawner)
        {
            send(spawner, "B", 1);
        }

        yield();
    }
    print("\n");

    exit(0);
}