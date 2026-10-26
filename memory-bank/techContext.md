# Technical context

- Language and target: freestanding C, C++, and x86-64 assembly.
- Build: `make clean && make`; single-core QEMU: `make run`; SMP: `make run
  CORES=4 MEM=16G`.
- Cross-compilation uses the x86_64-elf toolchain. BIOS and UEFI are separate
  boot paths and both matter for hardware-facing changes.
- Verification is layered: clean build, stack/health checks, kernel and host
  self-tests, TagFS stand tests, power-cut/replug scenarios, and real-hardware
  runs. A passing QEMU run is not a real-hardware proof.
- Relevant source roots are `src/kernel/tagfs`, `src/kernel/core`,
  `src/kernel/drivers`, `src/boot`, `tools/volumeread`, and
  `src/userspace/apps/quilltest*`.
- Never run destructive cleanup against Claude state or Git objects while a
  worktree or session archive is still part of the active handoff.
