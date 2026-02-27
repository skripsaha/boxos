# BoxLib - BoxOS User Space Library

Phase 1: Core Foundation

## Architecture

BoxLib implements 3-layer architecture for BoxOS user space programs:

```
┌─────────────────────────────────┐
│         Utils Layer             │  (Phase 3)
├─────────────────────────────────┤
│         Decks Layer             │  (Phase 2)
├─────────────────────────────────┤
│    Core Layer (THIS PHASE)      │
│  - Notify Page API              │
│  - Result Page API              │
│  - String/Memory utilities      │
└─────────────────────────────────┘
         ↓ int 0x80
    ┌────────────────┐
    │  BoxOS Kernel  │
    └────────────────┘
```

## Memory Cabin Model

User programs interact with kernel through fixed memory pages:

- `0x1000` - Notify Page (requests to kernel)
- `0x2000` - Result Page (responses from kernel)
- `0x3000` - Code Start Address

## Critical Constraints

**ENFORCED BY KERNEL:**
- Max prefixes in chain: 16 (not 64!)
- Event inline data: 192 bytes (not 3960!)
- Result ring buffer: 15 slots
- Result payload: 252 bytes per entry

## API Overview

### Notify API (`box/notify.h`)

Single-call wrapper:
```c
box_event_id_t box_notify(uint8_t deck_id, uint8_t opcode,
                          const void* data, size_t data_size);
```

Low-level control:
```c
box_notify_prepare();
box_notify_add_prefix(BOX_DECK_OPERATIONS, 0x01);
box_notify_write_data(&request, sizeof(request));
box_event_id_t event_id = box_notify_execute();
```

### Result API (`box/result.h`)

```c
box_result_entry_t result;

// Non-blocking
if (box_result_available()) {
    box_result_pop(&result);
}

// Blocking (with timeout)
if (box_result_wait(&result, 10000)) {
    // Process result.payload
}
```

### String/Memory Utilities (`box/string.h`)

```c
box_strlen(str);
box_strcpy(dest, src);
box_strcmp(s1, s2);
box_memcpy(dest, src, n);
box_memset(ptr, value, n);
box_memcmp(s1, s2, n);
```

## Build System

```bash
cd /Volumes/BOX/main/boxos/src/userspace/boxlib
make              # Build libbox.a
make clean        # Remove artifacts
```

Cross-platform support:
- macOS: Uses `x86_64-elf-gcc`
- Linux: Uses system `gcc`

## Usage in Programs

```c
#include <box/notify.h>
#include <box/result.h>

int main(void) {
    // Send request
    uint32_t value = 42;
    box_event_id_t event = box_notify(
        BOX_DECK_OPERATIONS,
        0x01,
        &value,
        sizeof(value)
    );

    // Wait for response
    box_result_entry_t result;
    if (box_result_wait(&result, 5000)) {
        // Handle result
    }

    return 0;
}
```

Compile:
```bash
x86_64-elf-gcc -nostdlib -ffreestanding -mno-red-zone \
    -I/Volumes/BOX/main/boxos/src/userspace/boxlib/include \
    -o program program.c \
    /Volumes/BOX/main/boxos/src/userspace/boxlib/libbox.a
```

## File Structure

```
boxlib/
├── include/box/
│   ├── types.h      # Core types and constants
│   ├── notify.h     # Notify Page API
│   ├── result.h     # Result Page API
│   └── string.h     # Utility functions
├── src/core/
│   ├── notify.c     # Notify implementation
│   ├── result.c     # Result ring buffer
│   └── string.c     # String/memory functions
├── Makefile
├── README.md
└── libbox.a         # Compiled library
```

## Next Phases

- **Phase 2:** Deck-specific wrappers (Operations, Storage, Hardware)
- **Phase 3:** High-level utilities (logging, error handling, helpers)

## Compatibility

Library structures match kernel definitions:
- `NotifyPage` layout (4KB page)
- `ResultRing` buffer (cache-line aligned)
- `ResultEntry` format (256 bytes aligned)

## Notes

All functions are freestanding (no libc dependencies).
Safe to use in minimal bootloader environments.
