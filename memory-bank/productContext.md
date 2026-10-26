# Product context

BoxOS is intended to be a real operating system for real users and real
hardware, not a classroom kernel or a POSIX compatibility exercise. The
quality bar is production correctness over short-term momentum: no silent data
loss, no unexplained flakes, no unbounded hidden limits, and no timeout used to
hide a missing event or ownership protocol.

The filesystem must scale beyond toy images while retaining a clear recovery
story. A write must either become a complete durable state or be named as
refused, torn, rotten, or not landed. The kernel reader, the host
`volume-read` reader, and the test oracle must agree byte-for-byte where the
format promises agreement.

The user approved keeping compression, deduplication, and self-healing in the
v2 direction when they are rebuilt as live, tag-driven mechanisms rather than
dead initialization code. This is a project decision preserved from the
Claude context, not an assumption to silently reinterpret.
