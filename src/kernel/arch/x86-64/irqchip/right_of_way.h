/* right_of_way.h — what a line asks for instead of a vector number.
 *
 * On this architecture the number IS the priority. Intel SDM Vol 3A §11.8.3:
 * "The interrupt-priority class is the value of bits 7:4 of the interrupt
 * vector. The lowest interrupt-priority class is 1 and the highest is 15",
 * and within a class the larger low nibble wins. So a kernel that hands out
 * vectors is handing out right of way, whether it means to or not.
 *
 * This kernel used to hand them out by arithmetic: vector = 32 + key, where
 * the key was the ISA number, or the SCI number, or the GSI, depending on
 * which of three constructors built the line. That was never a requirement of
 * the hardware — Intel 82093AA §3.2.4 is explicit: "the notion of interrupt
 * priority is completely unrelated to the position of the physical interrupt
 * input signal on the APIC. Instead, software determines the vector (and
 * therefore the priority) for each input" — it was a decision, and one side
 * effect of it was that the system tick, at vector 32, sat in the lowest
 * class in the machine while every storage and USB interrupt outranked it.
 *
 * So a line does not ask for a number here. It says what happens if it is
 * late, and the pool turns that into a class. There are three answers because
 * there are three honest ones, and each is the consequence of lateness rather
 * than a label:
 *
 *   A_PERSON_WAITS  — nothing breaks and no work is lost; somebody notices.
 *                     Keyboards, the ACPI SCI, anything that reports rather
 *                     than transfers.
 *   WORK_IS_REDONE  — a transfer already in flight reaches its own deadline
 *                     and the device gives up, so the kernel does it again.
 *                     Disks, USB, the serial line.
 *   TIME_IS_LOST    — the kernel cannot get it back. The tick: every deadline
 *                     in this kernel hangs off it, and a tick that arrives
 *                     late is a schedule that was wrong while it was late.
 *
 * ‼ A row that fills up is a LOUD REFUSAL naming the row, never a quiet slide
 * into the next one. Sliding would make this the same "first free vector"
 * every other kernel uses, with a layer of words on top — and a line would
 * silently get a right of way its driver never asked for.
 *
 * WHY THIS IS NOT PREEMPTION. Every gate this kernel installs is an interrupt
 * gate (0x8E), which clears IF on entry, and nothing in the interrupt path
 * sets it again. Handlers therefore never nest. The class decides which of
 * several PENDING interrupts the LAPIC delivers first — not whether one
 * handler may interrupt another.
 */
#ifndef RIGHT_OF_WAY_H
#define RIGHT_OF_WAY_H

#include "ktypes.h"

typedef enum {
    RIGHT_OF_WAY_A_PERSON_WAITS = 0,
    RIGHT_OF_WAY_WORK_IS_REDONE,
    RIGHT_OF_WAY_TIME_IS_LOST,
    RIGHT_OF_WAY_ROWS
} RightOfWay;

/* Seeds the pool with everything the architecture and this kernel have
 * already spoken for, and says on the board how much room that leaves.
 * Called once, from idt_init, before any line can ask. */
void VectorPoolInit(void);

/* Hand this line its right of way, or refuse out loud.
 *
 * `who` is the name that appears in the refusal and in the line that says
 * what was taken — the driver or the line, in the words the log uses
 * elsewhere ("GSI 19", "xHCI 00:14.0").
 *
 * Returns the vector, or 0 on refusal. Zero is not a sentinel chosen for
 * convenience: vector 0 is #DE, no line can ever own it, and a caller that
 * forgets to check gets a number the IDT will never route to it. */
uint8_t VectorPoolTake(RightOfWay row, const char *who);

/* Take THIS vector, because the caller has no choice about which one.
 *
 * The 8259 is the reason this exists. ICW2 sets the base of a run of eight
 * consecutive vectors and the chip ignores the low three bits of what is
 * written there, so on the PIC backend "vector = 32 + IRQ" is a fact about
 * the silicon and not a convention this kernel may change. Returns false —
 * loudly — if the vector is already spoken for. */
bool VectorPoolClaim(uint8_t vector, const char *who);

/* Give it back. A vector that is never returned is a vector the next device
 * to arrive cannot have, and devices arrive and leave on this machine: a
 * flash drive is unplugged, a port gives up its seat, a controller is retired.
 *
 * ‼ The caller must have masked the line and stopped its source FIRST. This
 * only makes the number available again; it cannot stop an interrupt already
 * on its way, and the next owner of the number would be handed it. */
void VectorPoolGiveBack(uint8_t vector, const char *who);

/* Who holds this vector, or NULL. For the line that explains an interrupt
 * nobody expected. */
const char *VectorPoolHolder(uint8_t vector);

#endif /* RIGHT_OF_WAY_H */
