"""bootbook — the kernels a format-v2 volume's book names by role, as the UEFI
loader finds them once the kernel its Deed names is not attested (spec_v2
§16.7 пп.3, 5; §9.2 п.2): the role of a file is bits 5 and 6 of its flags
(ondisk.FILE_ROLE_*) — kernel, boot or system, never a tag, a file's tags being
its user's; every file of FILES kept in one; the stitches past the slot's `at`,
sewn on by rules (а)/(б)/(б′)/(в) (stitch_ribbon.Ribbon, its direct scan left
out: a loader writes nothing and needs no next seq), and their records
replayed narrowly — the files kept in a role or put in one on the ribbon
alone, each record held to its base, an Undone passing over what it names; and
the digest of every block a record of the ribbon puts in.

The loader's Lectern does the same by its own code (src/boot/uefi/
tagboot_lectern_tree.c, _ribbon.c, _replay.c), and the host stand holds the
two to one answer line for line (tools/tagboot_stand.sh).

What the loader leaves to the kernel's mount, where the mount alone can
judge it (the narrowing, said here once): a file's records are replayed only
for the files it follows, so a Born of a fid it does not follow, and a Labeled's name and tags are not
weighed; a next_quire is not held against the ground of the book and the
files; and the data of the newest stitch is not held to its digests before
the replay (§4.5 п.6) — a block that did not land is one that does not match
its digest when the kernel that names it is attested."""

from . import ondisk, stitch, tally
from .bookmount import Blocks, BookMount, LastBlock, Mismatch, Splice
from .bookread import Short
from .stitch import BORN, LABELED, QUITCLAIM, SHAPED, UNDONE, KIND_NAMES, LABEL_FLAGS
from .stitch_ribbon import Ribbon

# The roles a kernel is looked for by, in the order they are asked (§16.7 п.3).
ROLES = (ondisk.FILE_ROLE_KERNEL, ondisk.FILE_ROLE_BOOT, ondisk.FILE_ROLE_SYSTEM)

FILE_KINDS = (BORN, SHAPED, LABELED, QUITCLAIM)


def Role(flags):
    """The role a file's flags put it in, or 0 for none."""
    return flags & ondisk.FILE_ROLE_MASK


def Absolute(splices):
    """Splices as absolute positions (bookmount.BookMount._Absolute)."""
    out, end = [], 0
    for at, remove, runs in splices:
        out.append((end + at, remove, runs))
        end = end + at + sum(r.count for r in runs)
    return out


def Inserted(kind, d):
    return d["runs"] if kind == BORN else [r for _a, _m, rs in d["splices"] for r in rs]


class BootBook:
    """What the loader reads of the book past the Tally of `slot` (`t` read),
    each line said into `say`: `files` {fid: FileState} the files it follows as
    the ribbon leaves them, `overlay` {block: digest} of the blocks the ribbon
    put in, and `candidates` — the files kept in a role in the order they are
    tried."""

    def __init__(self, g, slot, t, say):
        self.g, self.slot, self.t, self.say = g, slot, t, say
        self.ribbon = Ribbon(g, slot, (), scan=False)
        seqs = [u.seq for u in self.ribbon.stitches]
        say("the ribbon past slot %u's Tally: %s" % (slot.index, "%u stitch(es) sewn on, seq %u to %u" % (
            len(seqs), seqs[0], seqs[-1]) if seqs else "no stitch sewn on"))
        self.interest = set()
        self._Before()
        self._Copies(tally.FILES)
        self.files = {fid: fs.Copy() for fid, fs in t.content.files.items()
                      if Role(fs.flags) or fid in self.interest}
        self.followed = set(self.files)
        self.skip, self.refused = {}, set()
        self._Undone()
        for u in self.ribbon.stitches:
            self._Stitch(u)
        self.overlay = {}
        self._Overlay()
        self.candidates = self._Order()

    def _Copies(self, tree):
        """Every node of `tree` read past a copy that did not hold, in the order
        the tree is walked — root first, each node before its children."""
        for n in self.t.trees[tree].nodes if tree in self.t.trees else []:
            if n.copy:
                self.say("the %s node of level %u at block(s) %s: %u of its copies do not hold — copy %u is read"
                         % (tally.TREE_NAMES[tree], n.level, "/".join(str(b) for b in n.blocks), n.copy,
                            n.copy + 1))

    # --- before the files: the files the ribbon puts in a role ---------------

    def _Before(self):
        """Every file a Labeled puts in a role, in the order of the ribbon —
        followed from the Tally on, whatever its records come to."""
        for u in self.ribbon.stitches:
            for kind, _ign, body, _at in u.records:
                if kind != LABELED:
                    continue
                try:
                    d = stitch.RecordDecode(kind, body, u.seq)
                except Short:
                    continue
                if d["what"] & LABEL_FLAGS and Role(d["flags"][0]):
                    self.interest.add(d["fid"])

    # --- pass 1: the Undones (bookmount.BookMount._Undone) ----------------------

    def _Refuse(self, u, why):
        if u.seq not in self.refused:
            self.refused.add(u.seq)
            self.say("stitch %u is not applied: %s" % (u.seq, why))

    def _Undone(self):
        sewn = {u.seq for u in self.ribbon.stitches}
        for u in self.ribbon.stitches:
            for i, (kind, _ign, body, _at) in enumerate(u.records):
                if kind != UNDONE:
                    continue
                try:
                    d = stitch.RecordDecode(kind, body, u.seq)
                except Short:
                    self._Refuse(u, "it carries an Undone that does not read")
                    continue
                target = u.seq - d["seq_back"]
                if any(k != UNDONE for k, _i, _b, _a in u.records[:i]) or not d["seq_back"]:
                    why = "it carries an Undone after another record"
                elif target <= self.slot.stitch_seq:
                    continue
                elif target not in sewn or target >= u.seq:
                    why = "it carries an Undone of seq %u, which is no stitch of this ribbon before it" % target
                else:
                    self.skip.setdefault(target, set()).update(d["fids"])
                    self.say("stitch %u's Undone passes over stitch %u's records of %u file(s)"
                             % (u.seq, target, len(d["fids"])))
                    continue
                self._Refuse(u, why)

    # --- pass 2: each stitch, its files followed --------------------------------

    def _Refusal(self, u):
        """Why the stitch may not be applied, judged off its records alone —
        the first reason (bookmount.BookMount._Stitch, _Record, _Fresh)."""
        if u.head is not None and u.head.longer:
            return "its StitchHead or Clasp is %u byte(s) longer than this reader knows" % u.head.longer
        named = 0
        for kind, ign, body, _at in u.records:
            if kind not in KIND_NAMES or kind in (stitch.STITCH_HEAD, stitch.CLASP):
                if not ign:
                    return "it carries a record of kind %u this reader does not know and may not step over" % kind
                continue
            try:
                d = stitch.RecordDecode(kind, body, u.seq)
            except Short:
                return "its %s does not read" % KIND_NAMES[kind]
            if d["longer"]:
                return "its %s is %u byte(s) longer than this reader knows" % (KIND_NAMES[kind], d["longer"])
            if kind == BORN and d["fid"] not in self.skip.get(u.seq, ()) and not d["flags"] & 1:
                return "its Born of fid %u is not ACTIVE" % d["fid"]
            if kind in (BORN, SHAPED):
                named += sum(r.count for r in Inserted(kind, d)
                             if not r.hole and tally.EXT_SHARED not in stitch.Extensions(r.ext))
        if u.Sound() and named != u.parsed.fresh:
            return "its Clasp names %u fresh block(s) and its records %u" % (u.parsed.fresh, named)
        return None

    def _Stitch(self, u):
        if u.seq in self.refused:
            return
        why = self._Refusal(u)
        if why:
            self._Refuse(u, why)
            return
        skip, now = self.skip.setdefault(u.seq, set()), {}
        for kind, _ign, body, _at in u.records:
            if kind not in FILE_KINDS:
                continue
            d = stitch.RecordDecode(kind, body, u.seq)
            fid = d["fid"]
            if fid in skip:
                continue
            if fid not in self.followed:
                if kind != BORN or not (Role(d["flags"]) or fid in self.interest):
                    continue
                self.followed.add(fid)
            old = now[fid] if fid in now else self.files.get(fid)
            try:
                now[fid] = self._Apply(u.seq, kind, d, old)
            except Mismatch as e:
                skip.add(fid)
                now.pop(fid, None)
                self.say("stitch %u's %s of fid %u does not fit the file: %s — the file stays as it last matched"
                         % (u.seq, KIND_NAMES[kind], fid, e))
        for fid, fs in now.items():
            if fs is None:
                self.files.pop(fid, None)
            else:
                self.files[fid] = fs

    @staticmethod
    def _Apply(seq, kind, d, old):
        """The file `old` (None: not there) as the record leaves it — the rule
        of its kind (bookmount.BookMount._File), a Born of a file already there
        passed over. Mismatch when it does not fit."""
        if kind == BORN:
            if old is not None:
                return old
            fs = tally.FileState(d["fid"], seq, d["flags"], d["size"], d["created_ns"], d["modified_ns"], d["name"],
                                 d["tags"], d["runs"])
        else:
            at = (seq - d["base"]) % (1 << 64)
            if old is None or old.gen != at:
                raise Mismatch("its base names the file at %u and the file stands at %s"
                               % (at, "nothing" if old is None else old.gen))
            fs = old.Copy()
            fs.gen = seq
            if kind == QUITCLAIM:
                return None
            if kind == LABELED:
                if d["what"] & LABEL_FLAGS:
                    fs.flags = (fs.flags | d["flags"][0]) & ~d["flags"][1]
                return fs
            before = LastBlock(old)
            for at, remove, runs in Absolute(d["splices"]):
                fs.runs, _gone = Splice(fs.runs, at, remove, runs)
            fs.size = d["size"]
            if d["splices"] and LastBlock(fs) != before:
                fs.slips = []
            fs.slips = BookMount._SlipsCut(fs.slips, fs.size)
        if fs.Blocks() != Blocks(fs.size):
            raise Mismatch("its runs cover %u blocks and its %u bytes need %u" % (fs.Blocks(), fs.size,
                                                                                Blocks(fs.size)))
        BookMount._Slip(fs, d["slip"])
        return fs

    # --- the digests the ribbon carries, and the order of the kernels -----------

    def _Overlay(self):
        """{block: digest} every record of a stitch applied puts in, a later
        one over an earlier — but a file's record passed over (bookmount
        overlay.update)."""
        for u in self.ribbon.stitches:
            if u.seq in self.refused:
                continue
            for kind, _ign, body, _at in u.records:
                if kind not in (BORN, SHAPED):
                    continue
                d = stitch.RecordDecode(kind, body, u.seq)
                if d["fid"] in self.skip.get(u.seq, ()):
                    continue
                blocks = [r.start + i for r in Inserted(kind, d)
                          if not r.hole and tally.EXT_SHARED not in stitch.Extensions(r.ext) for i in range(r.count)]
                self.overlay.update(zip(blocks, d["digests"]))

    def _Order(self):
        """The files followed that are kept in a role, role by role in the
        order they are asked, the newest gen first and of one gen the larger
        fid — each once; a file naming ground outside the data run is the
        mount's to lock, and no kernel."""
        g, out = self.g, []
        for role in ROLES:
            group = [fs for fs in self.files.values() if Role(fs.flags) == role and
                     not any(not r.hole and (r.start < g.data_block or r.start + r.count > g.data_end)
                             for r in fs.runs)]
            out.extend(sorted(group, key=lambda f: (-f.gen, -f.fid)))
        return out
