#include "box/io.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    println("=== BoxOS IPC + Multitasking Demo ===");
    println("Launching proca and procb...");
    io_flush();

    int pid_a = proc_exec("proca");
    if (pid_a < 0) {
        println("Error: could not launch proca (not in TagFS?)");
        exit(1);
        return 1;
    }

    int pid_b = proc_exec("procb");
    if (pid_b < 0) {
        println("Error: could not launch procb");
        exit(1);
        return 1;
    }

    printf("proca -> PID %d\n", pid_a);
    printf("procb -> PID %d\n", pid_b);
    println("Receiving IPC messages:");
    println("------------------------");
    io_flush();

    int received = 0;
    int exits = 0;

    while (received < 20) {
        Result entry;
        bool got = receive_wait(&entry, 500);
        if (!got) {
            break;
        }

        // Skip exit notifications from child processes
        if (entry.data_length >= 1 && entry.data_addr != 0 &&
            *(uint8_t*)(uintptr_t)entry.data_addr == 0xFE) {
            exits++;
            if (exits >= 2) break;
            continue;
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

    println("------------------------");
    printf("Total received: %d messages\n", received);
    println("=== Demo complete ===");
    exit(0);
    return 0;
}
