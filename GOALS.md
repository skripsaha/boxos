# BoxOS goals

BoxOS is a bare-metal x86-64 operating system built around a non-Unix
paradigm. The project aims to become a production-quality system for real
hardware, with durable behavior, honest failure reporting, and long-term
maintainability.

The enduring outcomes are:

- Preserve the BoxOS model: AMP K-Cores and App Cores, semantic tags, async
  Pocket/Guide/Deck calls, Touch events, and TagFS instead of POSIX-shaped
  substitutes.
- Finish TagFS v2 as a real filesystem: crash-consistent format, fresh-ground
  writes, complete mount/replay, bounded metadata growth, integrity checks,
  and evidence from the kernel and the independent host reader.
- Keep BIOS and UEFI boot paths, multicore behavior, keyboard/input paths, VMM,
  and storage correct together on real hardware.
- Treat every failed gate, race, flake, silent refusal, and data-integrity
  discrepancy as a root-cause problem.

The project does not pursue POSIX compatibility or temporary compatibility
layers when they weaken these goals. The detailed philosophy and constraints
are summarized in `memory-bank/projectbrief.md` and
`memory-bank/systemPatterns.md`.
