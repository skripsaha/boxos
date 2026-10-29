"""selftest_book_memo — the memo of block digests a book's census keeps for the
next one (bookcensus.py DigestMemoWrite, DigestMemoRead): PowerCut's checks of
one record each run in a process of their own, and every one after the first
takes the digests of the base's files from the memo instead of hashing them
again. A memo that lost an entry only costs time; one that was not read at
all, or read as something else, would let a census hold a block to a digest
nobody took of it — so the memo is held to both: kept whole, and consulted."""

import hashlib
import os
import tempfile

from . import bookcensus, bookread
from .selftest_cases import Case


def _Blocks(c):
    """(block, its bytes) of every data block of every live file of the book."""
    for fid in sorted(c.mount.files):
        for s, n in c.mount.files[fid].Ground():
            for b in range(s, s + n):
                yield b, c.g.Block(b) or b""


def _Key(c, raw):
    return bookread.SeedLanes(c.g.uuid)[0], hashlib.sha1(raw).digest()


def _Through(memo):
    """The process's digests written out as a memo, forgotten, and read back:
    what the next census of the run starts from."""
    fd, path = tempfile.mkstemp(suffix=".digests")
    os.close(fd)
    try:
        bookcensus.DigestMemoWrite(path)
        bookcensus._DIGESTS.clear()
        bookcensus.DigestMemoRead(path)
    finally:
        os.remove(path)
    return dict(bookcensus._DIGESTS) == memo


def _Kept(c):
    """Every data block the census held to its digest is in the memo it keeps,
    and the memo read back is the one written."""
    saved = dict(bookcensus._DIGESTS)
    try:
        keys = {_Key(c, raw) for _b, raw in _Blocks(c)}
        return bool(keys) and keys <= set(saved) and _Through(saved)
    finally:
        bookcensus._DIGESTS.clear()
        bookcensus._DIGESTS.update(saved)


def _Consulted(c):
    """A census after a memo is read holds a block to the digest the memo
    gives for its bytes: one entry made wrong, and the block is named
    DIGEST_MISMATCH by a census of the same medium that found nothing."""
    saved = dict(bookcensus._DIGESTS)
    b, raw = next(_Blocks(c))
    try:
        bookcensus._DIGESTS[_Key(c, raw)] = saved[_Key(c, raw)] ^ 1
        if not _Through(dict(bookcensus._DIGESTS)):
            return False
        again = bookcensus.BookCensus(c.medium)
        return any(x.code == "DIGEST_MISMATCH" and any(s <= b < s + n for s, n in x.runs) for x in again.findings)
    finally:
        bookcensus._DIGESTS.clear()
        bookcensus._DIGESTS.update(saved)


def MemoCases(bt):
    """B0j: the memo of digests one census keeps, read by the next."""
    return [Case("B0j the block digests one census took are kept and read by the next as its own",
                 lambda o: None, checks=[
                     ("every data block of every live file is in the memo the census keeps, read back whole",
                      _Kept),
                     ("a digest the memo holds is the one the next census holds the block to — one made wrong "
                      "names the block DIGEST_MISMATCH", _Consulted)])]
