"""volumeread — the one Python owner of the TagFS format: its book.

    ondisk      what lies under the book: the units, the Deed and its stamps,
                a file's flags, the partition tables — every constant with the
                C line it is copied from
    medium      a medium and an in-memory overlay on one; where the volume stands
    blklog      QEMU blklogwrites logs: epochs, what each write touched, cuts
    bookread    the book's alphabet — the book sector and its CRC32C, varints,
                the self-checking pointer, units and their records — and the
                block digest (WyHash64)
    bookground  a volume's ground as its book reads it: its FRAME, its sectors
                judged once as the book's
    tally       the Spine's slots and the Tally: their records and nodes written
                and read, and the Tally a slot names read off a medium
    binder      the Tally laid down the one way every writer lays it, and the
                circle that binds a Tally read back again byte for byte
    stitch      the ribbon: a stitch's records, the ribbon walked and scanned,
                and a pen that sews stitches
    bookmount   a volume mounted the way spec_v2 §4.5 mounts it
    bookcensus  its roll call (§4.6), the mount's findings and the account
    bootread    which kernel the UEFI loader jumps into, and why
    selftest    every census rule planted and caught, the cut logic on a
                synthetic log, and the constants held against the headers —
                with the book's landings beside it (selftest_book*) and its
                header checks (selftest_headers)

Imported by tools/volume-read.py, which is only the command line. Anything
else on the host that needs to read a volume imports this package rather than
spelling the format a second time.
"""
