#!/usr/bin/env python3
"""gen_unicode_blocks.py — src/userspace/utils/glyphs/blocks.h, Unicode's
blocks as glyphs pages through them, written out of the Unicode Character
Database's Blocks.txt:

    curl -O https://www.unicode.org/Public/17.0.0/ucd/Blocks.txt
    python3 -I tools/gen_unicode_blocks.py Blocks.txt > src/userspace/utils/glyphs/blocks.h

Not a build dependency: the header is checked in, and this is run again only
for a later Unicode.

Every block is kept but the surrogates', which hold no character. Each must
begin on a row of sixteen and end at the end of one, the blocks in order and
apart, or nothing is written.
"""
import re
import sys

if len(sys.argv) != 2:
    sys.exit("usage: gen_unicode_blocks.py Blocks.txt")
text = open(sys.argv[1], encoding="utf-8").read()
version = re.match(r"# Blocks-(\d+\.\d+\.\d+)\.txt", text)
if not version:
    sys.exit("gen_unicode_blocks: %s does not begin as Blocks.txt does" % sys.argv[1])

blocks = []
for line in text.splitlines():
    found = re.match(r"([0-9A-F]{4,6})\.\.([0-9A-F]{4,6}); (.+)$", line)
    if not found:
        continue
    first, last, name = int(found.group(1), 16), int(found.group(2), 16), found.group(3).strip()
    if "Surrogates" in name:
        continue
    if first % 16 or (last + 1) % 16 or not re.fullmatch(r"[A-Za-z0-9 \-]+", name):
        sys.exit("gen_unicode_blocks: %04X..%04X %s is not a block of whole rows" % (first, last, name))
    if blocks and first <= blocks[-1][1]:
        sys.exit("gen_unicode_blocks: %04X..%04X %s is out of order" % (first, last, name))
    blocks.append((first, last, name))
if len(blocks) < 300:
    sys.exit("gen_unicode_blocks: only %d blocks read" % len(blocks))

print("""/* blocks.h — Unicode %s's blocks, the surrogates' left out: they hold no
 * character. Written by tools/gen_unicode_blocks.py from the Unicode
 * Character Database's Blocks.txt; not edited by hand. Each begins on a row
 * of sixteen and ends at the end of one, in order. */
#ifndef GLYPHS_BLOCKS_H
#define GLYPHS_BLOCKS_H

namespace glyphs {

struct UnicodeBlock {
    char32_t    First, Last;
    const char *Name;
};

inline constexpr UnicodeBlock UnicodeBlocks[] = {""" % version.group(1))
for first, last, name in blocks:
    print('    {0x%04X, 0x%04X, "%s"},' % (first, last, name))
print("""};

}  // namespace glyphs

#endif  // GLYPHS_BLOCKS_H""")
