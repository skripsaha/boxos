#include "../include/box/proc.h"
#include "../include/box/notify.h"
#include "../include/box/chain.h"

void proc_exit(int exit_code) {
    (void)exit_code;
    proc_kill(0);
    notify();
    while (1) {
        __asm__ volatile("pause");
    }
}
