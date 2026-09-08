#include "box/print.h"
#include "box/luggage.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 2) {
        println("Usage: say <text...>");
        exit(1);
        return 1;
    }

    /* The rest of the line as typed — spacing and quotes included — which is
     * the luggage's tail from the first word after the name. */
    println(luggage_tail(1));

    exit(0);
    return 0;
}
