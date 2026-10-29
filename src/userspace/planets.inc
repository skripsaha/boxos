; planets.inc: the planets in their order from the Sun, for planets.asm.

PLANETS equ 8

section .rodata
planet_names:
    dq mercury, venus, earth, mars, jupiter, saturn, uranus, neptune

mercury db "Mercury", 0
venus   db "Venus", 0
earth   db "Earth", 0
mars    db "Mars", 0
jupiter db "Jupiter", 0
saturn  db "Saturn", 0
uranus  db "Uranus", 0
neptune db "Neptune", 0
