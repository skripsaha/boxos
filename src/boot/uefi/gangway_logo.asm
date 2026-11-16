; gangway_logo.asm — the logo the panel stands beside (gangway_panel_look.c):
; duppo-word, still, dark and light, as tools/scene.py packs them into the
; build's scenes — the very bytes the volume carries, kept in the loader
; itself, so the panel shows it when the volume does not read, which is when
; the panel is wanted most.
;
; LOGO_DARK and LOGO_LIGHT name the two files (the Makefile passes them).

[BITS 64]

section .rodata

global GangwayLogoDark
global GangwayLogoDarkEnd
global GangwayLogoLight
global GangwayLogoLightEnd

align 16
GangwayLogoDark:
    incbin LOGO_DARK
GangwayLogoDarkEnd:

align 16
GangwayLogoLight:
    incbin LOGO_LIGHT
GangwayLogoLightEnd:
