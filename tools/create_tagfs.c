/*
 * create_tagfs.c — Make a BoxOS volume on ground somebody has claimed for it.
 *
 * ‼ IT DOES NOT DECIDE WHERE THE VOLUME GOES
 *
 * It reads the medium's partition table, exactly as the kernel does, and lays
 * the volume down on the run of sectors claimed for BoxOS. So the tool that
 * writes a volume and the kernel that mounts it answer "where is it?" the same
 * way, out of the same bytes, instead of both being told the same number by a
 * build script and drifting apart the first time one of them is edited.
 *
 * That is what replaced sector 1034 — an absolute address the tool, the kernel
 * twice, and the UEFI loader each spelled out for themselves.
 *
 * This file is the modes and nothing else; what each of them writes is in the
 * files create_tagfs.h names.
 *
 * Usage: create_tagfs [--kernel <file>] <disk_image> [<file> <tags>] ...
 *        create_tagfs --ground [--kernel <file>] [<file> <tags>] ...
 *        create_tagfs --frame-only [--kernel <file>] <disk_image> [<file> <tags>] ...
 *
 * ‼ THE TAGS ARE THE FILE'S USER'S WORDS, AND THE ROLE IS NOT ONE OF THEM.
 * What the volume keeps a file as — the kernel, a program of the system, one
 * a loader may boot with — is never in the list of tags. The kernel is the
 * file --kernel names, which must be one of the files given; a program says
 * what it is in its own box (the part `role`, tools/pack --role), and this
 * tool, which lays the boxes it has just packed, trusts it. Nothing running
 * on the volume grants a role from a box, and no tag ever did.
 */

#include "create_tagfs.h"

static int Usage(const char* self)
{
    fprintf(stderr, "Usage: %s [--kernel <file>] <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --ground [--kernel <file>] [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --frame-only [--kernel <file>] <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "\n");
    fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
    fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
    fprintf(stderr, "type GUID in a GPT), its book bound over its files.\n");
    fprintf(stderr, "Nothing outside that run is touched. A file's tags are its\n");
    fprintf(stderr, "user's words; what the system keeps a file as is its role:\n");
    fprintf(stderr, "the kernel is the file --kernel names (the one the Deed\n");
    fprintf(stderr, "points a loader at), a program says its own in its box.\n");
    return 1;
}

/* The option --kernel <file> at argv[at], taken out of the arguments when it
 * is there: the file is named to the cargo, and the arguments after it move
 * up. False when the option has no file after it. */
static int KernelOptionTake(int* argc, char* argv[], int at)
{
    if (*argc <= at || strcmp(argv[at], "--kernel") != 0) return 1;
    if (*argc < at + 2) return 0;
    CargoNameKernel(argv[at + 1]);
    for (int i = at; i + 2 < *argc + 1; i++) argv[i] = argv[i + 2];
    *argc -= 2;
    return 1;
}

int main(int argc, char* argv[]) {
    /* --ground answers a question about a file list and touches no medium. */
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        if (!KernelOptionTake(&argc, argv, 2) || (argc - 2) % 2 != 0) return Usage(argv[0]);
        return V2GroundSay((argc - 2) / 2, argv, 2);
    }

    /* --frame-only lays a Deed over files and writes no book. */
    if (argc >= 3 && strcmp(argv[1], "--frame-only") == 0) {
        if (!KernelOptionTake(&argc, argv, 2) || argc < 3 || (argc - 3) % 2 != 0) return Usage(argv[0]);
        return FrameOnlyMake(argv[2], (argc - 3) / 2, argv, 3);
    }

    if (!KernelOptionTake(&argc, argv, 1) || argc < 2 || (argc - 2) % 2 != 0 || argv[1][0] == '-')
        return Usage(argv[0]);
    return V2BindMake(argv[1], (argc - 2) / 2, argv, 2);
}
