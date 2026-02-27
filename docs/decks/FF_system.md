# System Deck (0xFF)

**Role**: Security, process management, memory management, and tag manipulation

## Overview

The System Deck is the security and management hub of BoxOS. It handles:
- Process lifecycle (spawn, kill, info)
- Memory buffer allocation/deallocation
- Tag-based permission management
- Context management (future)
- Security bypass control (future)

All System Deck operations are protected by tag-based security checks enforced at the security gate.

## Opcodes

### Process Management

#### 0x01 - PROC_SPAWN
Spawns a new process from a flat binary.

**Arguments** (proc_spawn_event_t):
```c
uint64_t binary_phys_addr;  // Physical address of binary (page-aligned)
uint64_t binary_size;       // Size in bytes (max 16MB)
char tags[128];             // Comma-separated tags (e.g., "app,test")
```

**Response** (proc_spawn_response_t):
```c
uint32_t new_pid;          // PID of created process
uint32_t reserved;
```

**Security**: Requires `proc_spawn` or `system` tag

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Invalid address, size, or tags
- `SYSTEM_ERR_SIZE_LIMIT` - Binary exceeds 16MB
- `SYSTEM_ERR_PROCESS_LIMIT` - Max processes reached (256)
- `SYSTEM_ERR_CABIN_FAILED` - Cabin creation failed
- `SYSTEM_ERR_LOAD_FAILED` - Binary load failed

#### 0x02 - PROC_KILL
Terminates a running process (marks as TERMINATED).

**Arguments** (proc_kill_event_t):
```c
uint32_t target_pid;       // PID to terminate
uint32_t reserved;
```

**Response** (proc_kill_response_t):
```c
uint32_t killed_pid;       // Confirmed PID
uint32_t reserved;
```

**Security**: Requires `proc_kill` or `system` tag

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Invalid PID
- `SYSTEM_ERR_PROCESS_NOT_FOUND` - Process does not exist

#### 0x03 - PROC_INFO
Retrieves detailed information about a process.

**Arguments** (proc_info_event_t):
```c
uint32_t target_pid;       // PID to query (0 = self)
uint32_t reserved;
```

**Response** (proc_info_response_t):
```c
uint32_t pid;
uint32_t state;            // process_state_t
int32_t score;             // Scheduler score
uint64_t notify_page_phys;
uint64_t result_page_phys;
uint64_t code_start;
uint64_t code_size;
bool result_there;
char tags[64];             // Truncated tag string
```

**Security**: Allowed for all processes

**Error Codes**:
- `SYSTEM_ERR_PROCESS_NOT_FOUND` - Process does not exist

### Memory Management

#### 0x10 - BUF_ALLOC
Allocates a shared memory buffer.

**Arguments** (buf_alloc_event_t):
```c
uint64_t size;             // Requested size (max 16MB)
uint32_t flags;            // Reserved (set to 0)
```

**Response** (buf_alloc_response_t):
```c
uint64_t buffer_handle;    // Handle for future operations
uint64_t phys_addr;        // Physical address of buffer
uint64_t actual_size;      // Actual size (page-aligned)
uint32_t error_code;
```

**Security**: Requires `app`, `utility`, or `system` tag

**Limits**:
- Max buffer size: 16MB
- Max buffers per process: 16
- Global buffer limit: 64

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Zero size
- `SYSTEM_ERR_SIZE_LIMIT` - Size exceeds 16MB
- `SYSTEM_ERR_BUFFER_LIMIT` - Buffer limit reached
- `SYSTEM_ERR_NO_MEMORY` - PMM allocation failed

#### 0x11 - BUF_FREE
Frees a previously allocated buffer.

**Arguments** (buf_free_event_t):
```c
uint64_t buffer_handle;    // Handle from BUF_ALLOC
```

**Response** (buf_free_response_t):
```c
uint32_t error_code;
```

**Security**: Requires `app`, `utility`, or `system` tag
- Can only free buffers owned by calling process

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Zero handle or not owner
- `SYSTEM_ERR_BUFFER_NOT_FOUND` - Invalid handle

#### 0x12 - BUF_RESIZE
Resizes an existing buffer (STUB - not implemented).

**Arguments** (buf_resize_event_t):
```c
uint64_t buffer_handle;
uint64_t new_size;
```

**Response** (buf_resize_response_t):
```c
uint64_t buffer_handle;
uint64_t actual_size;
uint32_t error_code;      // Always SYSTEM_ERR_NOT_IMPLEMENTED
```

**Status**: Stub implementation (returns NOT_IMPLEMENTED)

### Tag Management

#### 0x20 - TAG_ADD
Adds a tag to a target process.

**Arguments** (tag_modify_event_t):
```c
uint32_t target_pid;       // Process to modify
char tag[64];              // Tag to add
```

**Response** (tag_modify_response_t):
```c
uint32_t error_code;
char message[128];         // Human-readable message
```

**Security**: Requires `system` tag

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Invalid PID or empty tag
- `SYSTEM_ERR_PROCESS_NOT_FOUND` - Process does not exist
- `SYSTEM_ERR_TAG_EXISTS` - Tag already present
- `SYSTEM_ERR_TAG_FULL` - Tag buffer full (64 bytes max)

#### 0x21 - TAG_REMOVE
Removes a tag from a target process.

**Arguments** (tag_modify_event_t):
```c
uint32_t target_pid;       // Process to modify
char tag[64];              // Tag to remove
```

**Response** (tag_modify_response_t):
```c
uint32_t error_code;
char message[128];         // Human-readable message
```

**Security**: Requires `system` tag

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Invalid PID or empty tag
- `SYSTEM_ERR_PROCESS_NOT_FOUND` - Process does not exist
- `SYSTEM_ERR_TAG_NOT_FOUND` - Tag not present

#### 0x22 - TAG_CHECK
Checks if a process has a specific tag.

**Arguments** (tag_check_event_t):
```c
uint32_t target_pid;       // Process to check
char tag[64];              // Tag to check
```

**Response** (tag_check_response_t):
```c
bool has_tag;              // true if tag exists
uint32_t error_code;
```

**Security**: Allowed for all processes (read-only)

**Error Codes**:
- `SYSTEM_ERR_INVALID_ARGS` - Invalid PID or empty tag
- `SYSTEM_ERR_PROCESS_NOT_FOUND` - Process does not exist

### Routing / IPC

#### 0x40 - ROUTE
Routes event to a specific process by PID. Changes `event->pid` to `event->route_target`.

**Arguments** (via routing header, not data):
- `route_target` in NotifyPage: target PID

**Behavior**:
1. Validates route_target exists and is not terminated
2. Checks target ResultPage has space
3. Saves original PID as `sender_pid`
4. Sets `route_flags = ROUTE_SOURCE_PROCESS`
5. Rewrites `event->pid = route_target`
6. Chain continues; Execution Deck delivers to new PID

**Security**: Requires `app`, `utility`, or `system` tag

**Error Codes**:
- `BOXOS_ERR_INVALID_ARGUMENT` - route_target is 0
- `BOXOS_ERR_ROUTE_SELF` - route_target equals sender
- `BOXOS_ERR_PROCESS_NOT_FOUND` - target does not exist or terminated
- `BOXOS_ERR_ROUTE_TARGET_FULL` - target ResultPage is full

#### 0x41 - ROUTE_TAG
Routes event to all processes matching a tag. Clones the event for each target.

**Arguments** (via routing header, not data):
- `route_tag` in NotifyPage: tag string (supports "key:..." wildcard)

**Behavior**:
1. Searches all processes for tag match (max 16 targets)
2. For each match: clones event, sets `pid = target`, pushes to EventRing with prefix [0x0000]
3. Original event continues its chain (PID unchanged, result returns to sender)

**Security**: Requires `app`, `utility`, or `system` tag

**Error Codes**:
- `BOXOS_ERR_INVALID_ARGUMENT` - route_tag is empty
- `BOXOS_ERR_ROUTE_NO_SUBSCRIBERS` - no processes match tag

#### 0x42 - LISTEN
Registers process as receiver of hardware IRQ data (keyboard, mouse, etc).

**Arguments** (in event->data):
- `data[0]`: source_type (0=KEYBOARD, 1=MOUSE, 2=NETWORK)
- `data[1]`: flags (LISTEN_FLAG_EXCLUSIVE=0x01)

**Security**: Requires `app`, `display`, or `system` tag

**Error Codes**:
- `BOXOS_ERR_LISTEN_TABLE_FULL` - listen table full (max 64 entries)
- `BOXOS_ERR_LISTEN_ALREADY` - already listening on this source

### Context Management (Future)

#### 0x04 - CTX_USE
Sets global context filter (STUB - requires TagFS).

**Status**: Not implemented (returns SYSTEM_ERR_NOT_IMPLEMENTED)

#### 0x05 - BYPASS_ON
Enables kernel bypass for device (STUB - security feature).

**Status**: Not implemented (returns SYSTEM_ERR_NOT_IMPLEMENTED)

## Error Codes

```c
#define SYSTEM_ERR_SUCCESS           0x0000
#define SYSTEM_ERR_INVALID_ARGS      0x0001
#define SYSTEM_ERR_NO_MEMORY         0x0002
#define SYSTEM_ERR_PROCESS_LIMIT     0x0003
#define SYSTEM_ERR_PROCESS_NOT_FOUND 0x0004
#define SYSTEM_ERR_LOAD_FAILED       0x0005
#define SYSTEM_ERR_CABIN_FAILED      0x0006
#define SYSTEM_ERR_SIZE_LIMIT        0x0007
#define SYSTEM_ERR_BUFFER_NOT_FOUND  0x0008
#define SYSTEM_ERR_BUFFER_LIMIT      0x0009
#define SYSTEM_ERR_TAG_EXISTS        0x000A
#define SYSTEM_ERR_TAG_NOT_FOUND     0x000B
#define SYSTEM_ERR_TAG_FULL          0x000C
#define SYSTEM_ERR_NOT_IMPLEMENTED   0x00FF
```

## Security Model

The System Deck implements fine-grained tag-based security:

1. **Process Operations** (PROC_SPAWN, PROC_KILL):
   - Require specific permission tags or `system` tag
   - PROC_INFO is read-only (allowed for all)

2. **Memory Operations** (BUF_ALLOC, BUF_FREE):
   - Require `app`, `utility`, or `system` tag
   - Ownership enforced (can only free own buffers)

3. **Tag Operations** (TAG_ADD, TAG_REMOVE):
   - Require `system` tag (high privilege)
   - TAG_CHECK is read-only (allowed for all)

4. **Context/Bypass Operations**:
   - Reserved for future implementation

## Implementation Status

### Phase 1: Security Foundation
Status: COMPLETE
- Tag-based permission system
- Security gate integration
- Test coverage: 8/8 tests pass

### Phase 2: Process Management
Status: COMPLETE
- PROC_SPAWN with flat binary loading
- PROC_KILL with state management
- PROC_INFO with self-query support
- Test coverage: 7/7 tests pass

### Phase 3: Memory Management
Status: COMPLETE
- BUF_ALLOC with PMM integration
- BUF_FREE with ownership checks
- BUF_RESIZE stub
- Buffer tracking table (64 entries)
- Test coverage: 8/8 tests pass

### Phase 4: Tag Management
Status: COMPLETE
- TAG_ADD with duplicate detection
- TAG_REMOVE with existence checks
- TAG_CHECK for read-only queries
- Test coverage: 8/8 tests pass

### Phase 5: Context & Advanced
Status: STUB
- CTX_USE (requires TagFS)
- BYPASS_ON (security feature)

## Files

Implementation:
- `src/kernel/core/guide/system_deck.h` - Operation definitions
- `src/kernel/core/guide/system_deck.c` - Handler dispatcher
- `src/kernel/core/guide/system_deck_process.h` - Data structures
- `src/kernel/core/guide/system_deck_process.c` - Operation handlers

Testing:
- `src/kernel/core/guide/system_test.h` - Test declarations
- `src/kernel/core/guide/system_test.c` - Comprehensive test suite

## Total Test Coverage

- Security tests: 8/8 pass
- Process management: 7/7 pass
- Tag management: 8/8 pass
- Buffer management: 8/8 pass
- **Total: 31/31 tests pass**
