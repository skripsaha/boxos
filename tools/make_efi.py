#!/usr/bin/env python3
"""
make_efi.py — Convert an ELF64 PIC shared object to a PE32+ EFI application.

Used when the GNU cross-toolchain objcopy lacks the efi-app-x86-64 target.

Design:
  The linker script places code/data starting at VMA 0x1000 with ImageBase = 0,
  so ELF VMAs equal PE RVAs directly.  The converter:
    1. Reads LOAD segments and copies data verbatim into PE sections.
    2. Reads .rela.dyn R_X86_64_RELATIVE entries → PE .reloc (base relocations).
    3. Writes a proper PE32+ optional header with all required fields.

Usage:
  python3 tools/make_efi.py <input.so> <output.efi>
"""
import struct
import sys

# ── ELF constants ────────────────────────────────────────────────────────────
PT_LOAD            = 1
PF_X               = 1   # segment executable flag
SHT_RELA           = 4
R_X86_64_RELATIVE  = 8   # reloc type: *site = load_base + addend

# ── PE32+ constants ──────────────────────────────────────────────────────────
PE_MACHINE_AMD64       = 0x8664
PE_OPT_MAGIC_PE32PLUS  = 0x020B
IMAGE_SUBSYSTEM_EFI    = 10

IMAGE_FILE_EXECUTABLE  = 0x0002
IMAGE_FILE_LINE_NUMS   = 0x0004
IMAGE_FILE_LOCAL_SYMS  = 0x0008
IMAGE_FILE_LARGE_ADDR  = 0x0020
PE_CHARACTERISTICS     = (IMAGE_FILE_EXECUTABLE | IMAGE_FILE_LINE_NUMS |
                           IMAGE_FILE_LOCAL_SYMS | IMAGE_FILE_LARGE_ADDR)

SCN_CNT_CODE    = 0x00000020
SCN_CNT_IDATA   = 0x00000040
SCN_MEM_EXEC    = 0x20000000
SCN_MEM_READ    = 0x40000000
SCN_MEM_WRITE   = 0x80000000
SCN_DISCARDABLE = 0x02000000

FILE_ALIGN = 0x200    # 512 bytes
SECT_ALIGN = 0x1000   # 4 KB


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def parse_elf(data):
    """
    Parse an ELF64 shared object.  Returns:
      e_entry   — ELF entry point VMA
      segments  — list of dicts for PT_LOAD segments
      relocs    — list of (r_offset, r_addend) from .rela.dyn R_X86_64_RELATIVE
    """
    if data[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")
    if data[4] != 2:
        raise ValueError("Not ELF64")
    if data[5] != 1:
        raise ValueError("Not little-endian ELF")

    (e_type, e_machine, _e_ver, e_entry,
     e_phoff, e_shoff, _e_flags, e_ehsize,
     e_phentsize, e_phnum,
     e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)

    # ── Program headers (LOAD segments) ──────────────────────────────────────
    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_flags, p_offset, p_vaddr, _p_paddr,
         p_filesz, p_memsz, _p_align) = struct.unpack_from('<IIQQQQQQ', data, off)
        if p_type == PT_LOAD:
            segments.append({
                'flags':  p_flags,
                'vaddr':  p_vaddr,
                'filesz': p_filesz,
                'memsz':  p_memsz,
                'data':   data[p_offset : p_offset + p_filesz],
            })

    if not segments:
        raise ValueError("No PT_LOAD segments found")

    # ── Section headers — find .rela.dyn ─────────────────────────────────────
    relocs = []
    if e_shnum > 0 and e_shoff > 0:
        # String table for section names
        shstr_off_entry = e_shoff + e_shstrndx * e_shentsize
        shstr_file_off  = struct.unpack_from('<Q', data, shstr_off_entry + 24)[0]

        for i in range(e_shnum):
            sec = e_shoff + i * e_shentsize
            (sh_name, sh_type, _sh_flags, _sh_addr, sh_offset,
             sh_size, _sh_link, _sh_info, _sh_addralign,
             sh_entsize) = struct.unpack_from('<IIQQQQIIQQ', data, sec)

            name_start = shstr_file_off + sh_name
            name_end   = data.index(b'\x00', name_start)
            name       = data[name_start:name_end].decode('ascii', errors='replace')

            if sh_type == SHT_RELA and name == '.rela.dyn':
                entry_size = sh_entsize if sh_entsize else 24
                count = sh_size // entry_size
                for j in range(count):
                    r_offset, r_info, r_addend = struct.unpack_from(
                        '<QQq', data, sh_offset + j * entry_size)
                    r_type = r_info & 0xFFFFFFFF
                    if r_type == R_X86_64_RELATIVE:
                        relocs.append((r_offset, r_addend))

    return e_entry, segments, relocs


def build_reloc_section(relocs):
    """
    Build PE .reloc section from a list of (rva, addend) pairs.
    Each RVA marks a 64-bit slot for IMAGE_REL_BASED_DIR64 (type 10):
      *(site) += (load_address - ImageBase)
    Entries are grouped by 4 KB page per the PE COFF specification.

    Padding to even entry count uses IMAGE_REL_BASED_ABSOLUTE (type 0, offset 0),
    which is a no-op.  Using type 10 for padding would apply a second relocation
    to offset 0 of the page and corrupt the GOT.
    """
    if not relocs:
        return b''

    pages = {}
    for (rva, _addend) in relocs:
        page = rva & ~0xFFF
        pages.setdefault(page, []).append(rva & 0xFFF)

    out = b''
    for page in sorted(pages):
        offsets = sorted(pages[page])
        # Pad to even count with IMAGE_REL_BASED_ABSOLUTE (type 0) — a no-op.
        need_pad = (len(offsets) % 2 != 0)
        block_size = 8 + 2 * len(offsets) + (2 if need_pad else 0)
        out += struct.pack('<II', page, block_size)
        for off in offsets:
            out += struct.pack('<H', (10 << 12) | off)   # DIR64
        if need_pad:
            out += struct.pack('<H', 0)                   # ABSOLUTE (no-op padding)
    return out


def make_pe(elf_path, efi_path):
    data = open(elf_path, 'rb').read()
    e_entry, segments, relocs = parse_elf(data)

    # ── Separate code (RX) and data (RW) segments ────────────────────────────
    code_segs = [s for s in segments if s['flags'] & PF_X]
    data_segs = [s for s in segments if not (s['flags'] & PF_X)]

    if not code_segs:
        raise ValueError("No executable segment found")

    # Use the lowest VMA as the canonical image base for this ELF.
    # Our linker script starts at 0x1000, so elf_image_base = 0x1000.
    # Since PE ImageBase = 0, ELF VMAs equal PE RVAs directly.
    elf_image_base = min(s['vaddr'] for s in segments)

    def seg_rva(seg):
        return seg['vaddr']   # == vma - 0 (ImageBase=0)

    # Build flattened code blob (all RX segments, in VMA order)
    code_segs.sort(key=lambda s: s['vaddr'])
    code_rva   = seg_rva(code_segs[0])
    code_data  = b''.join(s['data'] for s in code_segs)
    code_memsz = sum(s['memsz'] for s in code_segs)

    # Build flattened data blob (all RW segments, in VMA order)
    if data_segs:
        data_segs.sort(key=lambda s: s['vaddr'])
        data_rva   = seg_rva(data_segs[0])
        data_data  = b''.join(s['data'] for s in data_segs)
        data_memsz = sum(s['memsz'] for s in data_segs)
    else:
        data_rva   = align_up(code_rva + code_memsz, SECT_ALIGN)
        data_data  = b''
        data_memsz = 0

    # .reloc section
    reloc_data = build_reloc_section(relocs)

    # ── Section count and file layout ─────────────────────────────────────────
    has_reloc  = bool(reloc_data)
    num_sects  = 2 + (1 if has_reloc else 0)

    # PE header area: DOS(0x80) + sig(4) + COFF(20) + OptHdr(240) + sections
    OPT_HDR_SIZE = 240
    pe_hdr_size  = 0x80 + 4 + 20 + OPT_HDR_SIZE + num_sects * 40
    hdr_file_sz  = align_up(pe_hdr_size, FILE_ALIGN)

    # Verify the ELF image base is far enough for our headers
    if elf_image_base < hdr_file_sz:
        raise ValueError(
            f"ELF image base 0x{elf_image_base:x} < PE header size 0x{hdr_file_sz:x}. "
            "Increase the linker script start address.")

    # Raw (file) sizes, padded to FILE_ALIGN
    text_rawsz  = align_up(len(code_data), FILE_ALIGN)
    data_rawsz  = align_up(len(data_data), FILE_ALIGN) if data_data else 0
    reloc_rawsz = align_up(len(reloc_data), FILE_ALIGN) if reloc_data else 0

    # Virtual sizes (rounded to SECT_ALIGN for SizeOfImage accounting)
    text_vsz  = align_up(code_memsz, SECT_ALIGN)
    data_vsz  = align_up(data_memsz, SECT_ALIGN) if data_memsz else 0
    reloc_vsz = align_up(len(reloc_data), SECT_ALIGN) if reloc_data else 0

    # .reloc RVA follows .data
    reloc_rva = data_rva + data_vsz

    # SizeOfImage = end of last section, rounded up to SECT_ALIGN
    image_size = align_up(reloc_rva + reloc_vsz, SECT_ALIGN)

    # Entry point: ELF VMA == PE RVA (ImageBase = 0)
    entry_rva  = e_entry

    # File offsets for section data
    text_raw_off  = hdr_file_sz
    data_raw_off  = text_raw_off + text_rawsz
    reloc_raw_off = data_raw_off + data_rawsz

    total_size = reloc_raw_off + reloc_rawsz

    # DataDirectory: index 5 = .reloc
    data_dirs = [(0, 0)] * 16
    if has_reloc:
        data_dirs[5] = (reloc_rva, len(reloc_data))

    # ── Build binary ──────────────────────────────────────────────────────────
    out = bytearray(total_size)

    # DOS stub (minimal)
    struct.pack_into('<H', out, 0x00, 0x5A4D)     # MZ magic
    struct.pack_into('<H', out, 0x02, 0x0090)     # e_cblp
    struct.pack_into('<H', out, 0x04, 0x0003)     # e_cp
    struct.pack_into('<H', out, 0x18, 0x0040)     # e_cparhdr
    struct.pack_into('<I', out, 0x3C, 0x80)       # e_lfanew → PE header at 0x80

    # DOS stub message (purely cosmetic, never executed from UEFI)
    stub_msg = b'This program requires UEFI firmware.\r\n$'
    out[0x40 : 0x40 + len(stub_msg)] = stub_msg

    p = 0x80
    # PE signature
    struct.pack_into('<4s', out, p, b'PE\x00\x00'); p += 4

    # COFF header (20 bytes)
    struct.pack_into('<HHIIIHH', out, p,
        PE_MACHINE_AMD64,   # Machine
        num_sects,          # NumberOfSections
        0,                  # TimeDateStamp
        0,                  # PointerToSymbolTable
        0,                  # NumberOfSymbols
        OPT_HDR_SIZE,       # SizeOfOptionalHeader
        PE_CHARACTERISTICS  # Characteristics
    ); p += 20

    # Optional header PE32+ (240 bytes)
    opt_start = p
    struct.pack_into('<H',  out, p, PE_OPT_MAGIC_PE32PLUS);  p += 2
    struct.pack_into('<BB', out, p, 14, 0);                   p += 2   # linker ver
    struct.pack_into('<I',  out, p, text_rawsz);              p += 4   # SizeOfCode
    struct.pack_into('<I',  out, p, data_rawsz + reloc_rawsz); p += 4  # SizeOfInitializedData
    struct.pack_into('<I',  out, p, 0);                       p += 4   # SizeOfUninitializedData
    struct.pack_into('<I',  out, p, entry_rva);               p += 4   # AddressOfEntryPoint
    struct.pack_into('<I',  out, p, code_rva);                p += 4   # BaseOfCode
    struct.pack_into('<Q',  out, p, 0);                       p += 8   # ImageBase = 0
    struct.pack_into('<I',  out, p, SECT_ALIGN);              p += 4   # SectionAlignment
    struct.pack_into('<I',  out, p, FILE_ALIGN);              p += 4   # FileAlignment
    struct.pack_into('<HH', out, p, 0, 0);                    p += 4   # MajorOS/Minor
    struct.pack_into('<HH', out, p, 0, 0);                    p += 4   # MajorImage/Minor
    struct.pack_into('<HH', out, p, 0, 0);                    p += 4   # MajorSub/Minor
    struct.pack_into('<I',  out, p, 0);                       p += 4   # Win32VersionValue
    struct.pack_into('<I',  out, p, image_size);              p += 4   # SizeOfImage
    struct.pack_into('<I',  out, p, hdr_file_sz);             p += 4   # SizeOfHeaders
    struct.pack_into('<I',  out, p, 0);                       p += 4   # CheckSum
    struct.pack_into('<H',  out, p, IMAGE_SUBSYSTEM_EFI);     p += 2   # Subsystem
    struct.pack_into('<H',  out, p, 0);                       p += 2   # DllCharacteristics
    struct.pack_into('<QQ', out, p, 0, 0);                    p += 16  # SizeOfStackReserve/Commit (EFI ignores)
    struct.pack_into('<QQ', out, p, 0, 0);                    p += 16  # SizeOfHeapReserve/Commit  (EFI ignores)
    struct.pack_into('<I',  out, p, 0);                       p += 4   # LoaderFlags
    struct.pack_into('<I',  out, p, 16);                      p += 4   # NumberOfRvaAndSizes
    for rva, sz in data_dirs:
        struct.pack_into('<II', out, p, rva, sz);             p += 8
    assert p - opt_start == OPT_HDR_SIZE, \
        f"Optional header size mismatch: {p - opt_start} vs {OPT_HDR_SIZE}"

    # Section headers (40 bytes each)
    def write_section_hdr(name, virt_sz, rva, raw_sz, raw_off, flags):
        nonlocal p
        n = name.encode('ascii').ljust(8, b'\x00')[:8]
        struct.pack_into('<8sIIIIIIHHI', out, p,
            n, virt_sz, rva, raw_sz, raw_off, 0, 0, 0, 0, flags)
        p += 40

    write_section_hdr('.text',  code_memsz, code_rva,  text_rawsz,  text_raw_off,
                      SCN_CNT_CODE  | SCN_MEM_EXEC | SCN_MEM_READ)
    write_section_hdr('.data',  data_memsz, data_rva,  data_rawsz,  data_raw_off,
                      SCN_CNT_IDATA | SCN_MEM_WRITE | SCN_MEM_READ)
    if has_reloc:
        write_section_hdr('.reloc', len(reloc_data), reloc_rva, reloc_rawsz, reloc_raw_off,
                          SCN_CNT_IDATA | SCN_MEM_READ | SCN_DISCARDABLE)

    # ── Section data ──────────────────────────────────────────────────────────
    out[text_raw_off  : text_raw_off  + len(code_data)] = code_data
    out[data_raw_off  : data_raw_off  + len(data_data)] = data_data
    if has_reloc:
        out[reloc_raw_off : reloc_raw_off + len(reloc_data)] = reloc_data

    open(efi_path, 'wb').write(bytes(out))

    print(f"  {efi_path}")
    print(f"    size:      {total_size} bytes")
    print(f"    ImageBase: 0x0  (UEFI relocates via .reloc)")
    print(f"    entry RVA: 0x{entry_rva:x}")
    print(f"    .text:     RVA=0x{code_rva:x}  size={code_memsz}")
    print(f"    .data:     RVA=0x{data_rva:x}  size={data_memsz}")
    if has_reloc:
        print(f"    .reloc:    RVA=0x{reloc_rva:x}  entries={len(relocs)}")
    else:
        print(f"    .reloc:    (none — binary has no base relocations)")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.so> <output.efi>", file=sys.stderr)
        sys.exit(1)
    try:
        make_pe(sys.argv[1], sys.argv[2])
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
