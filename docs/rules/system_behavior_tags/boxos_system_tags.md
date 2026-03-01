# BoxOS System Tags Specification

## Overview

All system behavior in BoxOS is controlled through tags. There are no separate permission systems, no ACLs, no capability lists. Tags are the single unified mechanism for privileges, identity, state, and context.

Tags come in two storage formats with identical internal representation:

- **Label tags**: single word (`app`, `hidden`, `protected`). Stored as key with empty value.
- **Key-value tags**: `key:value` (`user:toaster`, `project:mars`). Used for parameterized properties.

## Privilege Tags

Control what a process can do. Checked by Security Gate on every operation.

| Tag       | Type  | Applies to   | Description                                                                                                                        |
| --------- | ----- | ------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| `app`     | label | file         | Standard application. Access: Operations Deck, Storage Deck, basic I/O. Marks file as executable.                                  |
| `utility` | label | file         | System utility. Access: everything `app` can + System Deck (process management, tag management, memory). Marks file as executable. |
| `system`  | label | file/process | OS component. Full access including Hardware Deck. Passes through Security Gate (auditable). Marks file as executable.             |
| `god`     | label | process      | Superuser. Security Gate skips all checks. Granted at login with password.                                                         |
| `bypass`  | label | process only | Runtime permission for direct hardware access. Not a file tag. Granted by System Deck upon request. Revoked when no longer needed. |
| `network` | label | file/process | Enables Network Deck access. Without this tag, `app` cannot send or receive network data.                                          |

### Privilege Hierarchy

```
god        — bypasses Security Gate entirely
system     — full access, passes through Security Gate
utility    — system management, no direct hardware
app        — standard operations, storage, I/O
(no tag)   — data file, cannot execute
```

### Executability Rule

A file can only be spawned as a process if it has `app`, `utility`, or `system` tag. No tag = data, PROC_SPAWN refused.

## Identity Tags

Control ownership and visibility between users.

| Tag           | Type      | Applies to   | Description                                                                                                      |
| ------------- | --------- | ------------ | ---------------------------------------------------------------------------------------------------------------- |
| `user:<name>` | key:value | file/process | Owner or authorized user. Multiple `user:` tags = shared access. Inherited by child processes and created files. |

### Login Mechanics

- System boot → login prompt → password check → `use user:<name>` set as default context
- All processes inherit `user:<name>` from login session
- Switching user (`use user:alex`) requires password for target account
- Files without any `user:` tag are accessible only by `system`/`god`

### Sharing

To share a file: `tag report.txt user:alex`. Both `user:toaster` and `user:alex` now see the file. To revoke: `untag report.txt user:alex`.

## State Tags

Control file lifecycle and visibility. Checked by Storage Deck and query engine.

| Tag         | Type  | Applies to | Description                                                                                          |
| ----------- | ----- | ---------- | ---------------------------------------------------------------------------------------------------- |
| `trashed`   | label | file       | In trash. Hidden from normal queries. `erase trashed` deletes permanently.                           |
| `hidden`    | label | file       | Hidden from normal file listings. Visible with explicit `files hidden` query.                        |
| `readonly`  | label | file       | Write operations denied. Only `utility`/`system`/`god` can remove this tag.                          |
| `protected` | label | file       | Cannot be deleted. Only `god` can remove this tag or delete the file.                                |
| `temp`      | label | file       | Short-lived. System may delete at process exit, logout, or resource pressure.                        |
| `public`    | label | file       | Visible to all users. Ignores `user:` filter in queries.                                             |
| `autostart` | label | file       | Launched automatically at system boot. Kernel runs TAG_QUERY for `autostart` and spawns all results. |
| `encrypted` | label | file       | File contents encrypted on disk. Storage Deck requests key on access. **FUTURE — not implemented.**  |

## Process Tags

Control process behavior at runtime. Checked by Scheduler and Security Gate.

| Tag          | Type  | Applies to | Description                                                              |
| ------------ | ----- | ---------- | ------------------------------------------------------------------------ |
| `background` | label | process    | Not killed on user logout. Scheduler treats normally otherwise.          |
| `stopped`    | label | process    | Frozen. Scheduler skips (0 score). Remove tag to resume.                 |
| `unsociable` | label | process    | Refuses incoming IPC from other processes. **FUTURE — not implemented.** |

## Context Tags

Free-form user-created tags. Influence `use` command and Scheduler (+20 priority bonus).

| Tag              | Type      | Applies to   | Description                             |
| ---------------- | --------- | ------------ | --------------------------------------- |
| `project:<name>` | key:value | file/process | Project grouping                        |
| `task:<name>`    | key:value | file/process | Task grouping                           |
| `type:<value>`   | key:value | file         | Content type (photo, document, code...) |
| `status:<value>` | key:value | file         | Status (draft, active, done...)         |
| Any `key:value`  | key:value | file/process | User freedom. Any key, any value.       |

### Context Mechanics

- `use project:mars` → all queries filtered, matching processes get +20 score
- `use` (no args) → reset context
- Context inherited by child processes
- Multiple context tags supported: `use project:mars status:active`

### Category Query

`type:*` syntax returns all values for a key. Example: `files type:*` shows all content types in use.

## Security Gate Logic

```
on every Deck operation(process, deck_id, opcode):

    if process has "god":
        ALLOW (skip all checks)

    if process has "stopped":
        DENY

    match deck_id:
        Hardware Deck:
            require "system" or "bypass" → else DENY
        Network Deck:
            require "network" → else DENY
        System Deck:
            require "utility" or "system" → else DENY
        Storage Deck:
            if write operation and file has "readonly" → DENY
            if delete operation and file has "protected" → DENY
            require "app" or higher → else DENY
        Operations Deck:
            require "app" or higher → else DENY

    ALLOW
```

## Tag Modification Rules

| Action                                         | Required privilege                   |
| ---------------------------------------------- | ------------------------------------ |
| Add/remove context tags (project:, type:, ...) | any user                             |
| Add/remove `trashed`, `hidden`                 | owner (`user:` match)                |
| Add/remove `readonly`                          | `utility` / `system` / `god`         |
| Add/remove `protected`                         | `god` only                           |
| Add/remove `app`, `utility`, `system`          | `system` / `god` + user confirmation |
| Add/remove `user:` on own files                | owner                                |
| Add/remove `user:` on others' files            | `god` only                           |
| Grant `bypass` to process                      | `system` deck runtime decision       |
| Grant `god`                                    | login with password only             |
