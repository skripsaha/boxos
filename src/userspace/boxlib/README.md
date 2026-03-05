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

- `0x1000 - 0x2FFF` - Notify Page (8KB, requests to kernel)
- `0x3000 - 0xBFFF` - Result Page (36KB, responses from kernel)
- `0xC000` - Code Start Address (entry point)

## Critical Constraints

**ENFORCED BY KERNEL:**
- Max prefixes in chain: 16
- Event inline data: 256 bytes
- Result ring buffer: 128 slots
- Result payload: 244 bytes per entry

## API Overview

### Notify API (`box/notify.h`)

Low-level control:
```c
notify_prepare();
notify_add_prefix(0x01, 0x01);  // deck_id, opcode
notify_write_data(&request, sizeof(request));
event_id_t event_id = notify();
```

### Result API (`box/result.h`)

```c
result_entry_t result;

// Non-blocking check
if (result_available()) {
    result_pop(&result);
}

// Blocking (with timeout via RDTSC)
if (result_wait(&result, 10000)) {
    // Process result.payload (244 bytes)
}
```

### String/Memory Utilities (`box/string.h`)

```c
strlen(str);
strcpy(dest, src);
strcmp(s1, s2);
memcpy(dest, src, n);
memset(ptr, value, n);
memcmp(s1, s2, n);
```

## Build System

```bash
cd src/userspace/boxlib
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
    notify_prepare();
    notify_add_prefix(DECK_OPERATIONS, 0x01);
    notify_write_data(&value, sizeof(value));
    event_id_t event = notify();

    // Wait for response
    result_entry_t result;
    if (result_wait(&result, 5000)) {
        // Handle result.payload
    }

    return 0;
}
```

## File Structure

```
boxlib/
├── include/box/
│   ├── defs.h       # Platform definitions
│   ├── types.h      # Core types and constants
│   ├── notify.h     # Notify Page API
│   ├── result.h     # Result Page API
│   ├── ipc.h        # IPC API (send, broadcast, listen, receive)
│   └── string.h     # Utility functions
├── src/core/
│   ├── notify.c     # Notify implementation (INT 0x80)
│   ├── result.c     # Result ring buffer
│   ├── ipc.c        # IPC implementation
│   ├── yield.c      # Cooperative yield
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
- `notify_page_t` layout (8KB, 2 pages)
- `result_ring_t` buffer (cache-line aligned head/tail)
- `result_entry_t` format (256 bytes: source + error_code + sender_pid + 244 bytes payload)

## Notes

All functions are freestanding (no libc dependencies).
Safe to use in minimal bootloader environments.
