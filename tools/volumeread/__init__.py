"""volumeread — the one Python owner of the TagFS formats, v1 and the book of v2.

    ondisk      the format: every constant with the C line it is copied from, the
                checksums and the parsers — what a block, a record or a Ledger
                copy says, nothing about what the mount makes of it
    mount       the mount's decision procedure, step for step: which seal the
                volume stands on, which record of a file wins, what rolls back,
                who owns which block, the next file id and the next tag id
    medium      a medium and an in-memory overlay on one; where the volume stands
    census      GroundCensus (the account) and KernelView (the mount), held
                against each other
    blklog      QEMU blklogwrites logs: epochs, what each write touched, cuts
    bookread    format v2's alphabet — the book sector and its CRC32C, varints,
                the self-checking pointer, units and their records — and the
                block digest (WyHash64)
    bookground  a format-v2 volume's ground as its book reads it: its FRAME,
                its sectors judged once as the book's
    tally       the Spine's slots and the Tally: their records and nodes written
                and read, and the Tally a slot names read off a medium
    binder      the Tally laid down the one way every writer lays it, and the
                circle that binds a Tally read back again byte for byte
    stitch      the ribbon: a stitch's records, the ribbon walked and scanned,
                and a pen that sews stitches
    bookmount   a format-v2 volume mounted the way spec_v2 §4.5 mounts it
    bookcensus  its roll call (§4.6), the mount's findings and the account
    selftest    every census rule planted and caught, the cut logic on a
                synthetic log, and the constants held against the headers —
                with its plants, cases and header checks beside it, and the
                book's landings (selftest_book)

Imported by tools/volume-read.py, which is only the command line. Anything
else on the host that needs to read a volume imports this package rather than
spelling the format a second time.
"""
