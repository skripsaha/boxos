# System patterns

## Kernel and userspace

- AMP separates K-Core kernel service from App-Core userspace execution.
- Requests travel through Pocket rings to Guide and Deck handlers; completion
  uses Result rings or named event primitives.
- Touch is semantic multicast/reactive delivery, not a Unix signal or a polling
  loop. Brook is a stream, Bay is shared storage, and Current is late-bound
  channel access; they must not be collapsed into one generic abstraction.
- Lock ordering and interrupt context are part of the design. Allocation,
  sleeping, VMM work, and cross-core waits need an explicit context audit.

## TagFS and Quill

- Format v2 is a distinct book format with shared codecs used by the kernel,
  TagBoot, mkfs, and `volume-read`.
- The ribbon is append-oriented and sector-checked. A stitch names a complete
  state transition; retry at `next_quire` follows the reader's matching rule.
- File data is written on fresh ground. Overwrites create new stakes and splice
  only the affected record parcels; the old ground becomes Fallow after the
  format's release conditions.
- The Binder writes a complete Tally in bounded steps, two node copies, and a
  Spine slot. It preserves rotten ranges and named locked files according to
  the mount rules instead of inventing a second recovery model.
- Kernel and Python packing rules must be identical. `volume-read` is an
  independent witness, not a convenience parser.
- v1 paths remain protected by parity and regression gates until v2 has the
  required coverage and the planned final removal phase is explicitly reached.

Before adding a mechanism, consult the BoxOS master table and name the existing
primitive that was considered and rejected.
