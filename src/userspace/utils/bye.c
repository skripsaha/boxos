#include "box/print.h"
#include "box/system.h"

int main(void) {

    println("Shutting down system...");
    shutdown();

    exit(0);
    return 0;
}