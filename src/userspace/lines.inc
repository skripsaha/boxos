; lines.inc: a line of text said through printf, its format written where it
; is used: `line "%s is planet %d", r13, rbx`. Up to three 64-bit values
; follow the format, in registers or as numbers. A small library apart from
; the programs that use it: it wears asmlib, a program reaches it with
; `%include "lines.inc" asmlib everywhere`, and it finds its own part,
; lines_core.inc, beside itself.

%include "lines_core.inc"

; line "FORMAT" [, a [, b [, c]]]: printf takes no vector registers here, so
; al is 0. rdi, rsi, rdx, rcx, rax and the registers printf may change are
; not kept; rsp is to be aligned to 16 where the line stands.
%macro line 1-4
    lines_text %%format, %1
    lea rdi, [%%format]
%if %0 >= 2
    mov rsi, %2
%endif
%if %0 >= 3
    mov rdx, %3
%endif
%if %0 >= 4
    mov rcx, %4
%endif
    xor eax, eax
    call printf
%endmacro
