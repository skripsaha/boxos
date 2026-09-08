/*
 * handset — who holds the handset hears the keys.
 *
 * The oracle of the console's ear: says [HANDSET] ask, reads one line the
 * way any program does (readline over the strand's lane tag), says what it
 * got and leaves. The harness types the line — before the ask (type-ahead
 * banked by the daemon) or after it — and then types for the shell, which
 * must hear again once this program is gone.
 */

#include "box/print.h"

int main(void)
{
    char line[256];

    printf("[HANDSET] ask\n");
    int n = readline(line, sizeof(line));
    if (n < 0) {
        printf("[HANDSET] FAILED: readline said %d\n", n);
        return 1;
    }
    printf("[HANDSET] got: %s\n", line);
    return 0;
}
