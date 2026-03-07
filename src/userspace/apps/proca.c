#include "box/io.h"
#include "box/ipc.h"
#include "box/notify.h"
#include "box/system.h"

int main(void)
{
    CabinInfo *ci = cabin_info();
    uint32_t spawner = ci->spawner_pid;
    // printf("Spawner PID: %d\n", spawner);

    for (int i = 0; i < 5; i++)
    {
        // Simple output to display
        print("A");

        // Case with the spawner PID. When ipc_test utility need this for example.
        if (spawner)
        {
            send(spawner, "A", 1);
        }

        yield();
    }
    print("\n");

    exit(0);
}
