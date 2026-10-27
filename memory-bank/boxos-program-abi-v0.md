# BoxOS Program ABI v0

Status: design draft. No implementation is implied by this document yet.

This note defines the proposed contract between a native BoxOS program and the
existing kernel/userspace runtime. It is intentionally separate from the
command grammar and from the future executable-file format.

## Goal

A program produced inside BoxOS must have one stable way to:

- enter userspace;
- inspect its Cabin-owned runtime environment;
- submit a BoxOS operation;
- wait for the matching answer;
- communicate through BoxOS I/O primitives;
- terminate cleanly.

The ABI must expose BoxOS semantics rather than reproduce a Linux register-
numbered syscall ABI.

## Current implementation facts

These facts were read from the live source tree and are the authority for the
first design pass.

- The userspace entry is `_start` in `src/userspace/boxlib/boxlib_start.asm`.
- The current linker script places user images at `0xC000` and uses `_start` as
  the ELF entry point (`src/userspace/boxlib/user.ld`).
- The kernel accepts x86-64 little-endian `ET_EXEC` ELF images with `PT_LOAD`
  segments and maps their permissions with W^X in
  `src/kernel/core/memory/vmm/vmm.c`.
- Flat binaries are also accepted by the same loader. The first ELF-based
  assembler path therefore does not require a kernel loader change.
- CabinInfo is mapped at `0x1000`; the main PocketRing header is at `0x2000`,
  the main ResultRing header at `0x3000`, the TouchRing header at `0x5000`, and
  user code begins at `0xC000` (`src/include/cabin_layout.h`).
- Program arguments are not passed as `argc/argv`. The spawner writes the
  command line into CabinInfo as Luggage before the first instruction
  (`src/userspace/boxlib/include/box/luggage.h`, `src/userspace/boxlib/src/luggage.c`).
- `notify` is a semantic doorbell. The userspace macro emits the x86-64
  `syscall` instruction with `RDI = 0`; `yield` emits the same instruction with
  `RDI = GATE_YIELD` (`src/userspace/boxlib/include/box/core/notify.h`).
- `notify_entry.asm` is the common SYSCALL entry path. It builds an interrupt
  frame and calls the same `syscall_handler` used by the INT 0x80 fallback
  (`src/kernel/arch/x86-64/notify/notify_entry.asm`).
- Ordinary request data is carried by PocketRing/Pocket, Manifest, Crate and
  user buffers. It is not carried in the Linux-style syscall argument
  registers (`src/include/boxos_pocket.h`, `src/include/boxos_manifest.h`,
  `src/include/boxos_crate.h`).
- A Manifest of at most 88 bytes is enclosed in the Pocket. Larger Manifests
  are passed by user address and must remain alive until the kernel finishes
  reading them. Crate descriptors and their payloads are always address-based.
- Guide stages the Manifest and Crate descriptors, runs
  `ManifestExecuteOnce` or a compiled `Manifest`, commits output Crate sizes,
  and delivers a Result (`src/kernel/core/guide/guide.c`).
- On SMP, the App Core submits the process to a K-Core queue. The K-Core calls
  `guide_process_one` and serves one Pocket per turn
  (`src/kernel/arch/x86-64/idt/idt.c`, `src/kernel/arch/x86-64/amp/kcore.c`).
- Synchronous userspace waits match a Result by a 24-bit submit cookie rather
  than assuming that the next Result belongs to the caller
  (`src/userspace/boxlib/src/core/manifest.c`, `src/userspace/boxlib/src/core/result.c`).
- `exit()` runs runtime teardown and sends `SYSTEM_OP_PROC_KILL` through the
  normal Manifest/notify path. A successful self-exit does not return
  (`src/userspace/boxlib/src/system.c`).
- `Current` is a named I/O abstraction over screen, keyboard, log, files and
  Brook streams (`src/userspace/boxlib/include/box/current.h`).
- `print`/`printf` use the existing display-daemon Brook lane or the VGA
  fallback; they are not a second output ABI (`src/userspace/boxlib/src/print.c`,
  `src/userspace/boxlib/src/vga.c`).
- The current archive also contains `.tbss` in `string.o` for the thread-local
  `strtok` cursor. Existing C++ images initialize the local-exec TLS layout
  explicitly; the kernel loader does not synthesize it from `PT_TLS`. A native
  runtime that exposes thread-local operations must preserve that exact
  contract or reject the operation with a named error.

## Entity ownership and lifetime

| Entity | Owner | Lifetime | Purpose |
|---|---|---|---|
| CabinInfo | kernel writes, program reads | process Cabin lifetime | PID, heap/stack layout, Luggage |
| Pocket | userspace producer / kernel consumer | until PocketRing head advances | request envelope |
| Raw Manifest | program memory or Pocket enclosure | until kernel read completes | operation sequence |
| Compiled Manifest | kernel, owned by process | until release or process destruction | cached validated dispatch |
| Crate descriptor | program memory, staged by Guide | until request completion | buffer description |
| Crate payload | program memory | until handler completion | operation data |
| Result | kernel producer / userspace consumer | until ResultRing head advances | completion metadata |
| Current handle | program | until release | named I/O endpoint |
| Brook claim | Cabin/strand claim | until release or peer departure | framed stream |
| Touch claim | Cabin/strand claim | until release or process cleanup | semantic event subscription |
| Process | kernel scheduler | until exit/reap | execution and identity |
| Cabin | process/strand references | until last strand is gone | address space and shared runtime |
| Strand | scheduler | until strand exit/reap | one execution context inside a Cabin |

## Proposed ABI layers

The assembler and future language should expose two levels.

### High-level BoxOS operations

These are the normal program-facing forms:

```asm
box.say "Hello from BoxOS"
box.exit 0
```

They are compiled into runtime helpers that use the existing BoxOS path.

### Raw expert operations

These are available when a system program needs direct control:

```asm
box.manifest ...
box.notify
box.wait
```

Raw operations must still obey the shared Pocket/Manifest/Crate/Result ABI.
They are not an alternate syscall table.

## Minimal proposed primitive set

The first ABI draft contains five semantic primitives:

```text
box.notify       submit the prepared Pocket
box.yield        give the current core away
box.wait         wait for the Result carrying the current cookie
box.exit code    terminate the current process through System Deck
box.say text     write text through the existing BoxOS output path
```

`box.notify` is a language-level name for the existing `syscall` doorbell with
`RDI = 0`. `box.yield` uses `RDI = GATE_YIELD` and does not create a Pocket.

`box.wait` must not be a blind ResultRing poll. It must preserve cookie
matching and use the existing wait/Turn In behavior where appropriate.

`box.exit` must not invent a second process-termination path. It lowers to the
existing System Deck operation.

`box.say` must reuse the existing `Current`/print path. A direct Hardware Deck
VGA operation may be useful for a bootstrap diagnostic, but it must not become
the final standard output contract.

### Output decision

Decision: **B — `box.say` writes to `Current("screen")`**.

The program-facing meaning is the named screen Current. The existing display
daemon, per-strand Brook console lanes, ordering, and direct VGA fallback remain
implementation details of that path. The embedded runtime must not create a
second VGA-only output API.

### Dependency audit finding

The current `Current("screen")` path is architecturally the right contract but
is not a small standalone function. Its live implementation reaches the print
state and batching machinery, Brook, IPC, Manifest/Result submission, memory,
Touch and display-daemon fallback code. A first runtime cannot safely pretend
that copying only `current.c` is a minimal solution.

The next runtime task is therefore to define a deliberate **runtime capsule**:
the smallest reusable implementation closure that preserves the Current screen
contract. It may be packaged as an embedded machine-code/runtime payload in the
first ELF and later become the ordinary BoxOS runtime archive. It must not
become a second output path with different semantics.

## Entry contract

A native program starts at `_start` with:

- a valid user stack aligned for the System V x86-64 calling convention;
- a mapped CabinInfo page at `0x1000`;
- the main PocketRing and ResultRing headers mapped at `0x2000` and `0x3000`;
- the TouchRing header at `0x5000`;
- code loaded according to the ELF entry point;
- a process identity derived from the executable file's tags;
- Luggage already present before the first instruction.

The initial ABI does not require `argc/argv`. A future language may expose a
higher-level argument view over Luggage.

## Request contract

A raw request has this conceptual shape:

```text
program memory:
    Manifest bytes
    Crate[] descriptors
    Crate payload buffers

PocketRing:
    Pocket { manifest, crates, target, cookie }

notify:
    syscall with RDI = 0
```

The program must not reuse or free address-based Manifest/Crate memory until
the matching Result has arrived, unless the data was copied into an enclosed
Pocket or a runtime-owned async submission object.

## Completion contract

A successful or failed synchronous request receives exactly one matching Result
for its cookie. Output Crate descriptors are committed before the answer is
made visible to userspace.

An asynchronous operation may return control to the Guide with a parked
handler. The handler owns the staged Crates until the underlying event completes
and then pushes the Result. The ABI must make this lifetime explicit to any
future assembler runtime.

## Output contract

The final output abstraction is a named Current, with `screen` as the normal
console destination. `print` and `printf` remain convenience functions over
that path.

The ABI must not make a numeric file descriptor or a Linux `stdout` register a
fundamental concept. A future launcher may bind different Currents without
changing the program's source.

## First implementation boundary

The first native assembler should produce a standalone `ET_EXEC` ELF and a
small self-contained runtime for the minimal primitive set. It should not begin
by implementing a complete linker or by exposing every BoxLib symbol.

### Runtime packaging decision

Decision: **D — staged runtime packaging, with a real internal link stage**.

The dependency audit showed that embedding a copied runtime inside the assembler
would duplicate the existing BoxLib output path. The first quality toolchain
therefore keeps the user-facing command simple while using a proper internal
object/link step:

```text
program.asm
    → ELF64 ET_REL object in memory
    → runtime archive of ELF64 ET_REL modules
    → BoxOS linker
    → one ET_EXEC ELF
```

The intermediate object does not become a normal TagFS artifact unless a future
expert mode explicitly asks for it. The runtime archive and linker are real
toolchain components, not generated code copied into each application.

The same ABI remains valid when the bootstrap ELF object/archive path is later
replaced by a native BoxOS object or executable format.

The first useful vertical slice is:

```text
program.asm
  → assembler inside BoxOS
  → ET_EXEC ELF
  → TagFS artifact with explicit output tags
  → shell launch
  → box.say
  → box.exit
```

The raw Manifest/Crate surface can be added to the same assembler after the
minimal runtime path is proven.

## Invariants

- `notify` is only the doorbell; operation identity lives in Manifest op kinds.
- No Linux syscall-number ABI is exposed to BoxOS programs.
- Every synchronous answer is matched by cookie, never ring order alone.
- Address-based request memory remains alive until its answer or explicit async
  ownership transfer.
- `box.exit` uses the existing System Deck lifecycle.
- Output uses Current/BoxOS print semantics, not a parallel VGA-only API.
- The assembler does not change the kernel or TagFS in the first vertical slice.
- The runtime is not duplicated between assembler-generated programs.
- The first implementation is complete within its declared scope; no fake
  operations, placeholder handlers or silent fallback semantics are accepted.
- A compatibility path must preserve the same BoxOS ABI and lifetime rules,
  rather than creating a second hidden programming model.

## Open decisions before implementation

1. The exact raw syntax for Manifest and Crate construction.
2. The exact first runtime capsule needed to implement `box.say` through
   `Current("screen")` without introducing a second output path.
3. How a standalone assembler program receives and parses Luggage.
4. Whether a generated Nameplate is part of the first ELF output.
5. How runtime helpers are represented in the future native executable format.

## Planned phases

1. **ABI design** — finish entry, notify, wait, exit, output and lifetime
   contracts without changing the kernel.
2. **Standalone assembler** — assemble one source file into a valid `ET_EXEC`
   ELF with code/data segments and a deterministic entry point.
3. **Runtime and link stage** — map the dependency closure needed by
   `box.notify`, `box.yield`, `box.wait`, `box.exit` and `box.say`; package it
   as real ELF64 ET_REL runtime modules and link it without duplicating code.
   The first linker must also preserve `.tdata`/`.tbss`, the measured
   relocation set (including `R_X86_64_TPOFF32`) and the existing Nameplate
   contract; unsupported TLS use is a hard diagnostic, never a silent drop.
4. **Native artifact flow** — resolve the source through the current context,
   preserve all source tags, add explicitly requested output tags, and create
   or update the ELF artifact through existing BoxOS storage APIs.
5. **Shell launch verification** — launch the generated artifact by its name
   and tags, verify output and exit handling, and exercise ambiguity/error
   paths.
6. **Native format stage** — only after the ELF bootstrap path, runtime ABI and
   linker behavior are proven, design the native BoxOS object/executable format
   around measured needs instead of guesses.

## Quality bar

Every implementation phase must be complete within its stated boundary:

- no stubs presented as working features;
- no parallel fake runtime or alternate output path;
- no silent truncation, ignored errors or guessed lifetimes;
- all ownership and asynchronous completion rules are explicit;
- changed behavior is verified on the actual BoxOS path before the next phase;
- TagFS remains outside the first toolchain implementation boundary.
