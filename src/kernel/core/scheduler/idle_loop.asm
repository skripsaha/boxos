[BITS 64]
section .text

global idle_loop
extern cpu_idle
idle_loop:
    endbr64
.loop:
    call cpu_idle
    jmp .loop