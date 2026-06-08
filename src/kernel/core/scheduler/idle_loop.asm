[BITS 64]
section .text

global idle_loop
extern cpu_idle
; idle_loop's address is taken by the scheduler (`idle->context.rip =
; (uint64_t)idle_loop`). The very first scheduling of the idle process
; lands here via task_restore_context's terminal RET (shadow-stack-
; tracked), not an indirect CALL/JMP, so IBT is not strictly required.
; ENDBR64 is added defensively so a future indirect-call path through
; idle_loop stays IBT-safe; NOP without CR4.CET=1.
idle_loop:
    endbr64
.loop:
    call cpu_idle    ; one idle wait: MWAIT-C1 if available, else STI;HLT
                     ; (cpu_idle does its own STI; returns after an interrupt)
    jmp .loop        ; repeat forever
