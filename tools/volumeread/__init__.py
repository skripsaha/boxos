"""volumeread — the one Python owner of the TagFS v1 format.

    ondisk    the format: every constant with the C line it is copied from, the
              checksums, the parsers, and the walks that are format semantics
              (how far a chain runs, which record of a file wins, what the
              mount's roll call makes of a file)
    medium    a medium and an in-memory overlay on one; where the volume stands
    census    GroundCensus (the truth) and KernelView (the mount), held
              against each other
    blklog    QEMU blklogwrites logs: epochs, what each write touched, cuts
    selftest  every census rule planted and caught, the cut logic on a
              synthetic log, and the constants held against the headers

Imported by tools/volume-read.py, which is only the command line. Anything
else on the host that needs to read a volume imports this package rather than
spelling the format a second time.
"""
