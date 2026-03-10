#ifndef BOXOS_DECKS_H
#define BOXOS_DECKS_H

#define DECK_EXECUTION     0x00
#define DECK_OPERATIONS    0x01
#define DECK_STORAGE       0x02
#define DECK_HARDWARE      0x03
#define DECK_NETWORK       0x04
#define DECK_SYSTEM        0xFF

#define DECK_PREFIX(deck_id, opcode) ((uint16_t)(((uint8_t)(deck_id) << 8) | ((uint8_t)(opcode))))
#define DECK_ID_FROM_PREFIX(prefix)  (((prefix) >> 8) & 0xFF)
#define OPCODE_FROM_PREFIX(prefix)   ((prefix) & 0xFF)

#define PREFIX_TERMINATOR  0x0000

#endif // BOXOS_DECKS_H
