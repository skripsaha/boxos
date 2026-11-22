#!/usr/bin/env python3
"""box_author — the public half of the author's key the tree's boxes are signed with, as a C header.

    tools/box_author.py KEY.pem > box_author.h

The kernel trusts by birth one author: BoxOS, whoever builds this tree — the key lives on the building
machine, outside the tree (Makefile, BOX_SIGN_KEY), and only its public modulus reaches the kernel, here.
OpenSSL reads the key; nothing of the private half is printed."""

import subprocess
import sys

BYTES = 256     # box_format.h BOX_SIGN_RSA2048_BYTES


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: box_author.py KEY.pem")
    said = subprocess.run(["openssl", "rsa", "-in", sys.argv[1], "-noout", "-modulus"],
                          capture_output=True, text=True)
    line = said.stdout.strip()
    if said.returncode != 0 or not line.startswith("Modulus="):
        sys.exit("box_author: %s is no RSA key OpenSSL reads" % sys.argv[1])
    modulus = bytes.fromhex(line[len("Modulus="):])
    if len(modulus) != BYTES:
        sys.exit("box_author: %s is no RSA key of 2048 bits" % sys.argv[1])
    rows = ",\n".join("    " + ", ".join("0x%02x" % b for b in modulus[i:i + 12]) for i in range(0, BYTES, 12))
    print("/* box_author.h — made by tools/box_author.py from the building machine's key, never in the tree:")
    print(" * the public modulus of BoxOS's own, the one author the kernel trusts by birth (box_trust.c). */")
    print("#ifndef BOX_AUTHOR_H")
    print("#define BOX_AUTHOR_H")
    print()
    print("#define BOX_AUTHOR_BOXOS_MODULUS { \\")
    print(" \\\n".join(r for r in rows.split("\n")) + " }")
    print()
    print("#endif")


main()
