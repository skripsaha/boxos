#include "commands.h"
#include "box/io.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/string.h"

int cmd_ipc_test(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    println("=== BoxOS IPC + Multitasking Demo ===");
    println("Launching proca and procb...");

    int pid_a = proc_exec("proca");
    if (pid_a < 0) {
        println("Error: could not launch proca (not in TagFS?)");
        return -1;
    }

    int pid_b = proc_exec("procb");
    if (pid_b < 0) {
        println("Error: could not launch procb");
        return -1;
    }

    printf("proca -> PID %d\n", pid_a);
    printf("procb -> PID %d\n", pid_b);
    println("Receiving IPC messages:");
    println("------------------------");

    int received = 0;

    /* Each process sends 5 messages -> expect up to 10 total.
     * receive_wait(500) times out after ~500 ms of silence,
     * which means both processes have finished sending.            */
    while (received < 20) {
        result_entry_t entry;
        bool got = receive_wait(&entry, 500);
        if (!got) {
            break;  /* timeout: no more messages */
        }

        /* Null-terminate and print the payload */
        char buf[RESULT_PAYLOAD_SIZE + 1];
        uint16_t len = entry.size;
        if (len > RESULT_PAYLOAD_SIZE) {
            len = RESULT_PAYLOAD_SIZE;
        }
        memcpy(buf, entry.payload, len);
        buf[len] = '\0';

        printf("[PID %u] -> \"%s\"\n", entry.sender_pid, buf);
        received++;
    }

    println("------------------------");
    printf("Total received: %d messages\n", received);
    println("=== Demo complete ===");
    return 0;
}
