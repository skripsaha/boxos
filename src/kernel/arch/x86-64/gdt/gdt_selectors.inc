; gdt_selectors.inc - GDT selector constants for NASM
; Must match gdt.h definitions exactly
;
; Selector format: [Index:13][TI:1][RPL:2]
; RPL: 0=Ring0 (kernel), 3=Ring3 (user)

%ifndef GDT_SELECTORS_INC
%define GDT_SELECTORS_INC

%define GDT_KERNEL_CODE  0x08       ; Index 1, RPL=0
%define GDT_KERNEL_DATA  0x10       ; Index 2, RPL=0
%define GDT_USER_CODE    0x1B       ; Index 3, RPL=3 (0x18 | 3)
%define GDT_USER_DATA    0x23       ; Index 4, RPL=3 (0x20 | 3)
%define GDT_TSS          0x28       ; Index 5

%endif
