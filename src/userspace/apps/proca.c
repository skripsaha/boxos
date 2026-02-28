#include "box/io.h"
#include "box/system.h"

int main(void)
{
    for (int i = 0; i < 5; i++)
    {
        print("A");
        yield();
    }

    exit(0);
    while (1)
    {
    }
    return 0;
}
