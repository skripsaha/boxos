/*
 * boxasm_stand — the in-BoxOS assembler and linker, built for the host.
 *
 * The very sources `assemble` is made of, compiled here so tools/boxasm_stand.sh
 * can hold their output to GNU as, GNU ld and tools/nameplate on the inputs a
 * BoxOS image links. Nothing here re-implements them.
 *
 *     boxasm_stand assemble <source.asm> <out.o>
 *     boxasm_stand link <entry.o> <program.o> <archive.a> <out.elf> [symbols]
 *
 * A refusal prints the toolchain's own sentence on stderr and exits 1.
 */
#include "boxasm_link.h"
#include "boxasm_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *ReadAll(const char *Path, uint64_t *Size)
{
    FILE *File = fopen(Path, "rb");
    unsigned char *Bytes;
    long Length;

    if (!File)
        return NULL;
    if (fseek(File, 0, SEEK_END) != 0 || (Length = ftell(File)) < 0 ||
        fseek(File, 0, SEEK_SET) != 0) {
        fclose(File);
        return NULL;
    }
    Bytes = malloc((size_t)Length + 1u);
    if (!Bytes || fread(Bytes, 1, (size_t)Length, File) != (size_t)Length) {
        free(Bytes);
        fclose(File);
        return NULL;
    }
    fclose(File);
    Bytes[Length] = '\0';
    *Size = (uint64_t)Length;
    return Bytes;
}

static int WriteAll(const char *Path, const uint8_t *Data, uint64_t Size)
{
    FILE *File = fopen(Path, "wb");
    int Ok;

    if (!File)
        return 0;
    Ok = fwrite(Data, 1, (size_t)Size, File) == (size_t)Size;
    return fclose(File) == 0 && Ok;
}

static const char *BaseName(const char *Path)
{
    const char *Slash = strrchr(Path, '/');
    return Slash ? Slash + 1 : Path;
}

static int Load(const char *Path, BoxAsmBlob *Blob)
{
    uint64_t Size = 0;
    unsigned char *Bytes = ReadAll(Path, &Size);

    if (!Bytes) {
        fprintf(stderr, "boxasm_stand: cannot read %s\n", Path);
        return 0;
    }
    *Blob = (BoxAsmBlob){ Bytes, Size, BaseName(Path) };
    return 1;
}

static int Finish(int Rc, const char *Error, const BoxAsmBlob *Out, const char *Path)
{
    if (Rc != 0) {
        fprintf(stderr, "%s\n", Error[0] ? Error : "(refused without a sentence)");
        return 1;
    }
    if (!WriteAll(Path, Out->Data, Out->Size)) {
        fprintf(stderr, "boxasm_stand: cannot write %s\n", Path);
        return 2;
    }
    return 0;
}

int main(int Argc, char **Argv)
{
    char Error[512] = { 0 };
    BoxAsmBlob Out = { 0 };

    if (Argc == 4 && strcmp(Argv[1], "assemble") == 0) {
        BoxAsmBlob Source;
        if (!Load(Argv[2], &Source))
            return 2;
        return Finish(BoxAsmSourceAssemble((const char *)Source.Data, (size_t)Source.Size,
                                           &Out, Error, sizeof(Error)),
                      Error, &Out, Argv[3]);
    }
    if ((Argc == 6 || (Argc == 7 && strcmp(Argv[6], "symbols") == 0)) &&
        strcmp(Argv[1], "link") == 0) {
        BoxAsmBlob Entry, Program, Archive;
        if (!Load(Argv[2], &Entry) || !Load(Argv[3], &Program) || !Load(Argv[4], &Archive))
            return 2;
        return Finish(BoxAsmLink(&Entry, &Program, &Archive,
                                 Argc == 7 ? BOXASM_LINK_SYMBOLS : 0u,
                                 &Out, Error, sizeof(Error)),
                      Error, &Out, Argv[5]);
    }
    fprintf(stderr,
            "usage: %s assemble <source.asm> <out.o>\n"
            "       %s link <entry.o> <program.o> <archive.a> <out.elf> [symbols]\n",
            Argv[0], Argv[0]);
    return 2;
}
