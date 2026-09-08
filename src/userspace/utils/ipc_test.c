#include "box/print.h"
#include "box/ipc.h"
#include "box/touch.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {

    println("=== BoxOS IPC + Multitasking Demo ===");
    println("Launching proca and procb...");
    io_flush();

    /* Claim process:died BEFORE spawning so neither child can exit before we
     * are watching. Its death (clean OR crash) drives loop termination — a
     * separate ring from the args/message IPC below — and each death is matched
     * by canonical (pid, generation), so a recycled pid can never be mistaken. */
    TouchTag pdied = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    bool watching = (pdied != TOUCH_TAG_INVALID &&
                     touch_claim(pdied, TOUCH_REST, 0, 0) == OK);

    uint32_t gen_a = 0, gen_b = 0;
    int pid_a = proc_exec_gen("proca", NULL, &gen_a);
    if (pid_a < 0) {
        println("Error: could not launch proca (not in TagFS?)");
        if (watching) touch_release(pdied);
        exit(1);
        return 1;
    }

    int pid_b = proc_exec_gen("procb", NULL, &gen_b);
    if (pid_b < 0) {
        println("Error: could not launch procb");
        if (watching) touch_release(pdied);
        exit(1);
        return 1;
    }

    printf("proca -> PID %d\n", pid_a);
    printf("procb -> PID %d\n", pid_b);
    println("Receiving IPC messages:");
    println("------------------------");
    io_flush();

    bool dead_a = false, dead_b = false;
    int  received = 0;

    /* Event-driven termination: stop once BOTH children's deaths are collected.
     * received < 20 is a hard backstop. Messages are drained/printed meanwhile. */
    while (received < 20 && !(dead_a && dead_b)) {
        if (watching) {
            Touch t;
            while (touch_try_pop_tag(pdied, &t)) {
                if (t.payload_len < sizeof(TouchProcessDied)) continue;
                TouchProcessDied d;
                memcpy(&d, t.payload, sizeof d);
                if (d.pid == (uint32_t)pid_a && (gen_a == 0 || d.generation == gen_a))
                    dead_a = true;
                else if (d.pid == (uint32_t)pid_b && (gen_b == 0 || d.generation == gen_b))
                    dead_b = true;
            }
            if (dead_a && dead_b) break;
        }

        Result entry;
        if (!receive_wait(&entry, 500)) {
            if (watching) continue;   /* idle slice — the deaths terminate us */
            break;                    /* no claim: idle is the only stop signal */
        }

        char buf[257];
        uint32_t len = entry.data_length;
        if (len > 256) {
            len = 256;
        }
        if (entry.data_addr != 0 && len > 0) {
            memcpy(buf, (void*)(uintptr_t)entry.data_addr, len);
        }
        buf[len] = '\0';

        printf("[PID %u] -> \"%s\"\n", entry.sender_pid, buf);
        received++;
    }

    if (watching) touch_release(pdied);

    println("------------------------");
    printf("Total received: %d messages\n", received);
    println("=== Demo complete ===");
    exit(0);
    return 0;
}
