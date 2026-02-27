#ifndef BOXOS_DECKS_H
#define BOXOS_DECKS_H

// ============================================================================
// BOXOS DECK IDS AND OPCODES
// ============================================================================

// Deck IDs
#define DECK_EXECUTION     0x00
#define DECK_OPERATIONS    0x01
#define DECK_STORAGE       0x02
#define DECK_HARDWARE      0x03
#define DECK_SYSTEM        0xFF

// Prefix construction helpers
#define DECK_PREFIX(deck_id, opcode) ((uint16_t)(((uint8_t)(deck_id) << 8) | ((uint8_t)(opcode))))
#define DECK_ID_FROM_PREFIX(prefix)  (((prefix) >> 8) & 0xFF)
#define OPCODE_FROM_PREFIX(prefix)   ((prefix) & 0xFF)

// Prefix terminator
#define PREFIX_TERMINATOR  0x0000

#endif // BOXOS_DECKS_H
