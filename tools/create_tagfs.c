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
 * Usage: create_tagfs <disk_image> [<file> <tags>] ...
 *        create_tagfs --ground [<file> <tags>] ...
 *        create_tagfs --frame-only <disk_image> [<file> <tags>] ...
 */

#include "create_tagfs.h"

int main(int argc, char* argv[]) {
    /* --ground answers a question about a file list and touches no medium. */
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        if ((argc - 2) % 2 != 0) {
            fprintf(stderr, "Usage: %s --ground [<file> <tags>] ...\n", argv[0]);
            return 1;
        }
        return say_the_ground((argc - 2) / 2, argv, 2);
    }

    /* --frame-only lays a format-v2 Deed over files and writes no book. */
    if (argc >= 2 && strcmp(argv[1], "--frame-only") == 0) {
        if (argc < 3 || (argc - 3) % 2 != 0) {
            fprintf(stderr, "Usage: %s --frame-only <disk_image> [<file> <tags>] ...\n", argv[0]);
            return 1;
        }
        return FrameOnlyMake(argv[2], (argc - 3) / 2, argv, 3);
    }

    if (argc < 2 || (argc > 2 && (argc - 2) % 2 != 0)) {
        fprintf(stderr, "Usage: %s <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "       %s --ground [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "       %s --frame-only <disk_image> [<file> <tags>] ...\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
        fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
        fprintf(stderr, "type GUID in a GPT). Nothing outside that run is touched.\n");
        fprintf(stderr, "The file whose name stems to 'kernel' is the one the Deed\n");
        fprintf(stderr, "points a loader at.\n");
        return 1;
    }

    return V1PackMake(argv[1], (argc - 2) / 2, argv, 2);
}
