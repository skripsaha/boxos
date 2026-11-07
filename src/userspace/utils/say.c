#include "box/print.h"
#include "box/form.h"
#include "box/system.h"

BOX_FORM("takes text \"what to say\"\n"
         "gives text\n");

int main(void) {
    const char *text = form_text(0);

    if (!text) {
        exit(1);
        return 1;
    }

    println(text);

    exit(0);
    return 0;
}
