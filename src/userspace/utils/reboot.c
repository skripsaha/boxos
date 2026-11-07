#include "box/print.h"
#include "box/system.h"
#include "box/form.h"

BOX_FORM("");

int main(void) {

    println("Rebooting system...");
    reboot();

    exit(0);
    return 0;
}
