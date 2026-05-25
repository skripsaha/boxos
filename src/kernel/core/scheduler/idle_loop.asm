[BITS 64]
section .text

global idle_loop
extern cpu_idle
idle_loop:
.loop:
    call cpu_idle    ; one idle wait: MWAIT-C1 if available, else STI;HLT
                     ; (cpu_idle does its own STI; returns after an interrupt)
    jmp .loop        ; repeat forever
