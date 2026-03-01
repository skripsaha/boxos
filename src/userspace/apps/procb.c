#include "box/io.h"
#include "box/ipc.h"
#include "box/notify.h"
#include "box/system.h"

int main(void)
{
    notify_page_t* np = notify_page();
    uint32_t parent = np->parent_pid;

    for (int i = 0; i < 5; i++)
    {
        print("B");
        if (parent != 0) {
            send(parent, "B", 1);
        }
        yield();
    }

    println("");
    exit(0);
}
