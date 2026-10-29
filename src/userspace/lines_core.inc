; lines_core.inc: the part of lines.inc that keeps a line's text. The format
; goes to .rodata under a label of the caller's, a newline and a NUL after
; it, and the code goes on in .text. lines.inc finds this file beside itself
; wherever it was found; it is read once however often it is included.

%ifndef LINES_CORE
%define LINES_CORE

extern printf

; lines_text LABEL, "FORMAT"
%macro lines_text 2
section .rodata
%1: db %2, 10, 0
section .text
%endmacro

%endif
