# BoxOS Runtime Capsule Audit v0

Status: design evidence. Read-only audit; no implementation changes.

## Finding

The existing `libbox.a` is already an archive of ELF relocatable objects. The
runtime capsule should reuse this object/module model rather than copy source
or generate a second implementation of BoxOS I/O.

The dependency closure is not uniform:

- `box.notify` is comparatively small. Its direct object path uses the
  PocketRing producer, `strand_rings()` and memory primitives, with `yield`
  sharing the same notify door.
- `box.wait` is not a simple polling helper. It depends on ResultRing sequence
  handling, cookie filtering, stash routing, Turn In/step-aside behavior and
  the CPU wait path.
- `box.exit` runs runtime teardown, flushes output, returns strand-pool state,
  and then submits `SYSTEM_OP_PROC_KILL` through Manifest. It therefore pulls
  in more than just a process opcode wrapper.
- `box.say` must preserve the `Current("screen")` contract. The current screen
  path reaches the print state/batching machinery, display-daemon Brook lanes,
  IPC/Touch coordination, Result handling, memory helpers and VGA fallback.
  It is not safe to replace this with a second direct-VGA implementation.
- Generic `Current` additionally covers file, keyboard, log and arbitrary
  Brook backings. The first screen-only runtime may avoid unrelated Current
  backends, but only if the exported semantic contract remains
  `Current("screen")` and the implementation reuses the existing output path.

## Archive evidence

The live archive is `src/userspace/boxlib/libbox.a`. Its members include:

```text
notify.o
result.o
strand_self.o
manifest.o
print.o
current.o
system.o
memory.o
vga.o
brook.o
touch.o
turnin.o
runtime_init.o
...
```

The current user Makefiles already link these ELF relocatable members with
`boxlib_start.o` and `user.ld`. The future in-BoxOS linker must therefore be
able to consume the same class of ET_REL objects and archive members, resolve
their relocations, retain the `boxlib_start` entry contract, and emit a valid
ET_EXEC image.

## Measured symbol closures

The live archive and its object files were inspected with the target `nm` tool.
The closure is the transitive set of archive objects needed by each root
symbol, not merely the direct `#include` list.

| Root | Objects in closure | Main implication |
|---|---:|---|
| `pocket_submit` | 3 | `core/notify.o`, `core/strand_self.o`, `string.o` are a small raw doorbell path |
| `yield` | 3 | shares the same notify/strand routing path |
| `result_wait` | 19 | Result waiting pulls the complete result/stash/wait/output support cluster |
| `print_bytes` | 19 | screen output reaches print, VGA, Brook, IPC, Touch, memory and Manifest support |
| `_Exit` | 21 | exit includes teardown, output flush, memory/strand-pool cleanup and Manifest submission |
| `luggage_word_count` | 20 | Luggage parsing pulls memory/string and the wait/step-aside path used during first-use coordination |

The measured `result_wait`/`print_bytes` closure includes these existing object
families: `core/manifest.o`, `core/notify.o`, `core/result.o`,
`core/stash.o`, `core/strand_self.o`, `core/touch_ring.o`, `print.o`,
`vga.o`, `brook.o`, `bay.o`, `ipc.o`, `touch.o`, `turnin.o`, `memory.o`,
`debug.o`, `string.o`, `convert.o`, `clock.o` and `time.o` where the archive's
transitive symbols require them. `_Exit` additionally reaches
`runtime_init.o` and `system.o`.

The closure leaves only expected link-time/runtime symbols unresolved in this
object-level view: `_GLOBAL_OFFSET_TABLE_` and the runtime array symbols
(`__init_array_*`, `__fini_array_*`, plus weak `__cxa_finalize` for the exit
path). These are linker/runtime contract symbols, not missing BoxOS services.

The consequence is deliberate: the first runtime capsule may be large, but it
must be correct. We should not shrink it by cutting out async, cleanup, or
display dependencies and calling the result a complete `box.say`, `box.wait`
or `box.exit` implementation.

## Relocation and section evidence

Representative live ET_REL objects were inspected with the target objdump.
The current bootstrap linker must support at least:

- `R_X86_64_PLT32` for ordinary calls between runtime objects;
- `R_X86_64_PC32` for local code/data and `.eh_frame` references;
- `R_X86_64_32` for absolute 32-bit data references used by the current
  freestanding build;
- `R_X86_64_32S` for signed 32-bit data references;
- `R_X86_64_64` for absolute 64-bit addresses in read-only tables (the current
  `print.o` has these in `.rela.rodata`);
- `.text`, `.rodata*`, `.data`, `.bss`, and `.eh_frame` input sections;
- archive member extraction by defined/undefined symbol closure.

The selected objects contain local symbols, strong global function/data
symbols, and a weak `printf` definition. The first symbol resolver must make
strong/weak precedence deterministic and reject conflicting strong globals.
No COMMON symbol was observed in the inspected closure; an unsupported COMMON
definition must be rejected explicitly rather than silently treated as BSS.

The current entry object is `src/userspace/build/boxlib_start.o`. Its live
relocations are `PC32` references to `__box_runtime_init`, `main`, and `exit`.
The final link must keep the entry object first in `.text`, preserve the
`_start` entry point, and place the resulting loadable image according to
`src/userspace/boxlib/user.ld`.

No PLT/GOT runtime should be invented for the freestanding first target unless
the selected object set actually requires it. The current object audit leaves
`_GLOBAL_OFFSET_TABLE_` and runtime-array symbols as linker/runtime contract
symbols; they must be resolved explicitly or rejected with a named diagnostic,
never silently left in the produced image.

There is one more live ABI surface in the archive: `string.o` contains the
thread-local `g_strtok_cursor` in `.tbss` and three `R_X86_64_TPOFF32`
relocations. The current C-only images therefore already carry a `PT_TLS`
program header even though the kernel loader maps only `PT_LOAD`. C++ images
initialize the local-exec TLS block from the linker symbols in
`tls_mold.h`/`tls_init.cpp`; a pure-C image currently does not bootstrap an FS
base merely because the archive contains `string.o`. The in-BoxOS linker must
preserve this fact instead of dropping TLS sections or pretending that a
TPOFF32 relocation is an ordinary absolute address. A first runtime contract
must either initialize the exact existing TLS layout before exposing
thread-local operations, or reject a program whose selected runtime requires
such access with a named diagnostic.

## Consequence for the first toolchain

The first runtime capsule is not a copied source subset and not a collection of
hand-written replacement stubs. It is a deliberately selected set of existing
BoxOS runtime modules linked through the same ABI rules as the current
userspace programs.

The user-facing command remains:

```text
assemble program.asm -> program.elf
```

Internally it may perform:

```text
source → ET_REL
runtime archive → selected ET_REL members
link → ET_EXEC
```

No intermediate object needs to be created as a TagFS file by default.

## Artifact and shell path audit

The existing shell already provides the launch contract: an external command
is started by `proc_exec_gen(line, NULL, &generation)`, and the complete typed
line becomes the child's Luggage. The shell waits on that exact `(pid,
generation)` through `process_gone`; the assembler must not add another
launcher or another exit-event path.

The storage path is also already sufficient, but it has to be used in the
right order:

1. Resolve the source name inside the current Use Context. Zero matches and
   more than one match are distinct human errors; choosing one by array order
   is forbidden.
2. Read the source bytes through the existing Manifest-native storage API.
3. Read full source metadata through the self-describing
   `STORAGE_OBJ_GET_INFO` response. The public `file_info_t` helper is
   intentionally lossy and is unsuitable for tag inheritance.
4. Assemble and link completely in memory. Do not create a destination file
   while syntax, symbol, relocation or segment validation can still fail.
5. Create the destination through `create`, passing the inherited source tags
   plus explicit output tags. `create` also applies the current Use Context and
   the filename stem, with kernel-side deduplication.
   The final tag set must contain `app` or `utility`, because the existing
   `SYSTEM_OP_PROC_EXEC` candidate check refuses every active file without one
   of those keys.
6. Set the exact size with `file_span`, write all bytes with checked short-write
   handling, then call `anchor` so the new artifact is durable.
7. On a storage failure after creation, attempt `delete` of that exact id and
   surface both the original refusal and a cleanup refusal; no partial file is
   silently reported as a successful build.

This path uses only existing storage/user APIs and does not require a TagFS
kernel change. It does require a small, reusable BoxLib full-metadata adapter
before the assembler can promise lossless source-tag inheritance.

## Next bounded design phase

Before implementation, produce a symbol-level closure for the first complete
runtime target:

1. `box.notify` / `box.yield`;
2. `box.wait` with cookie-correct Result handling;
3. `box.exit` with teardown;
4. `box.say` through the existing screen output path;
5. the `_start` and Luggage contract.

For each target, record:

- required ET_REL members;
- exported symbols;
- undefined symbols after archive extraction;
- data sections and TLS requirements;
- ownership/lifetime assumptions;
- async completion paths;
- whether the current linker script and Nameplate path cover the image.

Only after this closure is complete should the first linker/runtime code be
written.

## First internal linker contract

The first linker is a bounded static linker for the existing BoxOS object
model. It is not a general GNU-ld replacement and it must fail before writing
an image when an input exceeds this contract.

### Inputs and extraction

1. Accept ELF64, little-endian, `ET_REL`, `EM_X86_64` objects with checked
   section and symbol-table bounds.
2. Read the current `ar` archive format and its symbol index. Starting from
   `_start` and the assembled program's undefined symbols, extract archive
   members until the undefined set reaches a fixed point. Archive order may
   affect diagnostics, never the chosen definition: the resolver is
   deterministic.
3. Keep each object's local symbols object-scoped. Resolve global symbols by
   strong-over-weak precedence; reject two different strong definitions and
   reject `COMMON` explicitly because the measured closure has none.
4. Preserve the source object name and symbol name in every diagnostic so a
   failed in-BoxOS build can be corrected without a host tool.

### Sections and layout

Aggregate input sections according to the existing `user.ld` contract:

```text
.text (entry object first)
.rodata* ".rodata"
.eh_frame
.gcc_except_table
.nameplate
.data
.init_array
.fini_array
.tdata
.tbss
.bss / COMMON (COMMON is rejected in this first contract)
```

Use each input section's alignment, checked power-of-two arithmetic, and the
same `USER_BASE = 0xC000`/page boundaries as the current linker script. Emit
W^X `PT_LOAD` segments matching the existing images: executable/read-only
content in the first load segment and writable/NX content in the second.
Emit `PT_TLS` when `.tdata` or `.tbss` exists, with `p_filesz`, `p_memsz` and
`p_align` derived from those sections. The kernel may ignore `PT_TLS` today,
but dropping it would make the artifact structurally different from the
existing ABI and would break the C++ bootstrap contract.

The linker must define the symbols consumed by the runtime, including
`__init_array_start/end`, `__fini_array_start/end`, `__tdata_start`,
`__tdata_size`, `__tbss_size`, `__eh_frame_start/end`, `__nameplate_start/end`
and `__program_end`. A generated symbol is either assigned an exact value or
the link fails; there are no zero-valued placeholders for a section that was
actually selected.

### Relocations

Apply `RELA` addends using the AMD64 formulas and check the written width and
signedness before modifying the output. The measured archive requires:

```text
R_X86_64_PLT32   S + A - P   (direct static call; no PLT is emitted)
R_X86_64_PC32    S + A - P
R_X86_64_32      S + A       (unsigned 32-bit range check)
R_X86_64_32S     S + A       (signed 32-bit range check)
R_X86_64_64      S + A       (full 64-bit)
R_X86_64_TPOFF32 S + A - TP  (only with the exact TLS layout contract)
```

Relocations against a discarded section, undefined symbol, unsupported type,
or out-of-range value are hard errors naming object, section, offset, symbol,
type and computed value. `_GLOBAL_OFFSET_TABLE_`, `__cxa_finalize` and the
init/fini array symbols are resolved as explicit runtime/linker contracts;
they are never left as unresolved bytes in an ET_EXEC image.

### Nameplate

Nameplate is part of the first complete image contract, not an optional host
post-process. After code/rodata addresses are fixed, the linker harvests the
same executable symbols that `tools/nameplate` accepts, builds the existing
`NameplateHeader` table, places `.nameplate` before `.data`, and then lays out
the writable sections. A final verification pass must re-derive each entry
from the emitted symbol table and reject address drift or a table that would
not fit the format. This removes the host-only two-pass dependency without
changing the in-image Nameplate format consumed by the kernel and BoxLib.

### Acceptance criteria for the linker phase

The phase is complete only when one implementation can:

- link the existing `boxlib_start.o` plus the measured runtime closure;
- resolve all five ordinary relocation classes and TPOFF32 under the stated
  TLS rule;
- emit valid `PT_LOAD`/`PT_TLS` metadata and W^X permissions;
- carry a verified Nameplate;
- reject unresolved, conflicting, unsupported and overflowed cases with
  actionable diagnostics; and
- produce an ET_EXEC image that the existing loader can launch without a
  kernel or TagFS change.

Until those checks pass, the assembler may produce an in-memory ET_REL object
for diagnostics, but it must not claim that `assemble ... -> program.elf`
created a runnable program.

## Quality constraints

- No copied or divergent print implementation.
- No ignored unresolved symbols.
- No runtime helper that silently falls back to a different semantic channel.
- No guessed lifetime for Manifest, Crate, Result or Current state.
- No “minimal” capsule that omits an async or cleanup dependency and claims to
  be complete.
- TagFS remains outside this phase; the artifact writer will use existing
  userspace storage APIs later.
