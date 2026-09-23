#ifndef PCI_CLASS_WORDS_H
#define PCI_CLASS_WORDS_H

#include "ktypes.h"

/* What a function IS, in words, from the three bytes that say so.
 *
 * A class triple is the only thing on a PCI function that describes it
 * without a vendor database: PCI Local Bus Specification Rev. 3.0 §6.2.1
 * puts the Programming Interface at configuration offset 09h, the Sub-Class
 * at 0Ah and the Base Class at 0Bh, and says of them: "The Class Code
 * register is a read-only register that is used to identify the generic
 * function of the device and, in some cases, a specific register-level
 * programming interface." The names of the codes are assigned by the PCI
 * Code and ID Assignment Specification.
 *
 * Printing the triple as hex leaves the reading to whoever is standing in
 * front of the board with a specification open; printing it as words does
 * not. `01.06.01` and `a SATA controller speaking AHCI 1.0` are the same
 * three bytes, and only one of them can be read off a photograph of a
 * screen.
 *
 * Always writes something. A code this table has no word for still gets its
 * numbers — inside a sentence that says they are numbers because nothing
 * named them, so an unnamed device is never mistaken for a named one. */
void PciClassInWords(uint8_t cls, uint8_t sub, uint8_t prog,
                     char *out, size_t out_len);

#endif
