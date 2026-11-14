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
 * Usage: create_tagfs --v1 <disk_image> [<file> <tags>] ...
 *        create_tagfs --v2 <disk_image> [<file> <tags>] ...
 *        create_tagfs --ground --v1 [<file> <tags>] ...
 *        create_tagfs --ground --v2 [<file> <tags>] ...
 *        create_tagfs --frame-only <disk_image> [<file> <tags>] ...
 *
 * The format is always named: a tool that made one format when none was named
 * would make that one for every caller that relied on it, whichever the build
 * makes by default (Makefile VOLUME_FORMAT) — so it makes neither, and says so.
 */

#include "create_tagfs.h"

static int Usage(const char* self)
{
    fprintf(stderr, "Usage: %s --v1|--v2 <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --ground --v1|--v2 [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --frame-only <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "\n");
    fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
    fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
    fprintf(stderr, "type GUID in a GPT), in the format named — always named.\n");
    fprintf(stderr, "Nothing outside that run is touched. A file's tags are its\n");
    fprintf(stderr, "user's words; what the system keeps a file as is its role,\n");
    fprintf(stderr, "named first in the tag string: \"@system\", \"@boot\" or\n");
    fprintf(stderr, "\"@kernel\" (\"@system utility,fun\"). The file in the role\n");
    fprintf(stderr, "@kernel is the one the Deed points a loader at.\n");
    return 1;
}

/* Whether `arg` names a format, and which: 1 for --v1, 2 for --v2, else 0. */
static int FormatNamed(const char* arg)
{
    return strcmp(arg, "--v1") == 0 ? 1 : strcmp(arg, "--v2") == 0 ? 2 : 0;
}

int main(int argc, char* argv[]) {
    /* --ground answers a question about a file list and touches no medium. */
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        int format = argc >= 3 ? FormatNamed(argv[2]) : 0;
        if (!format || (argc - 3) % 2 != 0)
            return Usage(argv[0]);
        return format == 2 ? V2GroundSay((argc - 3) / 2, argv, 3) : say_the_ground((argc - 3) / 2, argv, 3);
    }

    /* --frame-only lays a format-v2 Deed over files and writes no book. */
    if (argc >= 3 && strcmp(argv[1], "--frame-only") == 0 && (argc - 3) % 2 == 0)
        return FrameOnlyMake(argv[2], (argc - 3) / 2, argv, 3);

    int format = argc >= 3 ? FormatNamed(argv[1]) : 0;
    if (!format || (argc - 3) % 2 != 0)
        return Usage(argv[0]);
    return format == 2 ? V2BindMake(argv[2], (argc - 3) / 2, argv, 3) : V1PackMake(argv[2], (argc - 3) / 2, argv, 3);
}
