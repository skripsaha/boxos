# Project brief

BoxOS is a bare-metal x86-64 operating system written in C and assembly. It
deliberately avoids Unix/POSIX assumptions and carries meaning through tags,
manifests, capabilities, and asynchronous message paths.

The main architecture is an AMP split: K-Cores run kernel work while App Cores
run userspace. Pocket/Guide/Deck provide asynchronous kernel requests. Touch
provides semantic event delivery. Brook, Bay, Current, Result, and Manifest
are distinct primitives with different ownership and delivery contracts.

TagFS is the central storage model. It is tag-oriented and does not use a VFS
or path-first interface. The current long-running effort is Quill, the TagFS
v2 write and durability design: a format-v2 book, a replayable ribbon of
stitches, fresh-ground data writes, integrity digests, a derived block map,
Fallow reclamation, and a Binder that writes complete Tallies and Spine slots.

The user's seven design words are dynamics, asynchrony, uniqueness, stability,
tags, semantics, and convenience. A proposed mechanism must strengthen those
words or explain why it is rejected.
