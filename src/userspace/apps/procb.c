#include "box/io.h"
#include "box/ipc.h"
#include "box/notify.h"
#include "box/system.h"

int main(void)
{
    notify_page_t *np = notify_page();
    uint64_t spawner = np->spawner_pid;
    // printf("Spawner PID: %d\n", spawner);

    for (int i = 0; i < 5; i++)
    {
        // Simple output to display
        print("B");

        // Case with the spawner PID. When ipc_test utility need this for example.
        if (spawner)
        {
            send(spawner, "B", 1);
        }

        yield();
    }
    print("\n");

    exit(0);
}
