#include "box/print.h"
#include "box/system.h"
#include "box/form.h"

BOX_FORM("");

int main(void) {

    println("Shutting down system...");
    shutdown();

    exit(0);
    return 0;
}
