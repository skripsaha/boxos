#include "box/print.h"
#include "box/system.h"

int main(void) {

    println("Rebooting system...");
    reboot();

    exit(0);
    return 0;
}
