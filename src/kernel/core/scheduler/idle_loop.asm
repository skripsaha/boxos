[BITS 64]
section .text

global idle_loop
idle_loop:
.loop:
    sti          ; Enable interrupts
    hlt          ; Halt until interrupt (power-saving)
    jmp .loop    ; Repeat forever
