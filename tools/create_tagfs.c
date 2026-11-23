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
 * Usage: create_tagfs [--block <bytes>] [--kernel <file>] [--system <file>]... <disk_image> [<file> <tags>] ...
 *        create_tagfs --ground [--kernel <file>] [--system <file>]... [<file> <tags>] ...
 *        create_tagfs --frame-only [--kernel <file>] [--system <file>]... <disk_image> [<file> <tags>] ...
 *
 * ‼ THE TAGS ARE THE FILE'S USER'S WORDS, AND THE ROLE IS NOT ONE OF THEM.
 * What the volume keeps a file as — the kernel, a program of the system, one
 * a loader may boot with — is never in the list of tags. The kernel is the
 * file --kernel names, which must be one of the files given; a file that is
 * not a box but the system keeps — the loader a volume's maker puts on an
 * ESP — is kept in the system's role when --system names it; a program says
 * what it is in its own box (the part `role`, tools/pack --role), and this
 * tool, which lays the boxes it has just packed, trusts it. Nothing running
 * on the volume grants a role from a box, and no tag ever did.
 */

#include "create_tagfs.h"

static int Usage(const char* self)
{
    fprintf(stderr, "Usage: %s [--block <bytes>] [--name <name>] [--kernel <file>] [--system <file>]... <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --ground [--kernel <file>] [--system <file>]... [<file> <tags>] ...\n", self);
    fprintf(stderr, "       %s --frame-only [--kernel <file>] [--system <file>]... <disk_image> [<file> <tags>] ...\n", self);
    fprintf(stderr, "\n");
    fprintf(stderr, "Makes a BoxOS volume on the run of this image claimed for\n");
    fprintf(stderr, "BoxOS by its partition table (MBR type 0x7F, or the BoxOS\n");
    fprintf(stderr, "type GUID in a GPT), its book bound over its files.\n");
    fprintf(stderr, "Nothing outside that run is touched. A file's tags are its\n");
    fprintf(stderr, "user's words; what the system keeps a file as is its role:\n");
    fprintf(stderr, "the kernel is the file --kernel names (the one the Deed\n");
    fprintf(stderr, "points a loader at), a file --system names is kept in the\n");
    fprintf(stderr, "system's role, a program says its own in its box.\n");
    fprintf(stderr, "--block is the block the image's medium is addressed in\n");
    fprintf(stderr, "(4096 for a 4Kn medium, 512 by default): its partition\n");
    fprintf(stderr, "table counts in it.\n");
    fprintf(stderr, "--name is the name the person gives the volume: UTF-8 of any\n");
    fprintf(stderr, "language, at most %u bytes, no control character; kept\n", TALLY_VOLUME_NAME_MOST);
    fprintf(stderr, "in its book, it goes with the volume. With none, the volume\n");
    fprintf(stderr, "is shown by its medium.\n");
    return 1;
}

/* The option --block <bytes> first, taken out of the arguments when it is
 * there: the block the image's medium is addressed in. False for a block no
 * medium is seated with. */
static int BlockOptionTake(int* argc, char* argv[])
{
    if (*argc < 2 || strcmp(argv[1], "--block") != 0) return 1;
    if (*argc < 3) return 0;
    char* end = NULL;
    unsigned long block = strtoul(argv[2], &end, 0);
    if (!end || *end || block % TAGFS_SECTOR_BYTES || block < TAGFS_SECTOR_BYTES || block > TAGFS_BLOCK_SIZE)
        return 0;
    g_medium_block = (uint32_t)block;
    for (int i = 1; i + 2 < *argc + 1; i++) argv[i] = argv[i + 2];
    *argc -= 2;
    return 1;
}

const char* g_volume_name;

/* The option --name <name> at argv[1], taken out of the arguments when it is
 * there: the name the book is laid with. False for one with no name after it,
 * or one not spelled as a name (TallyNameSpelled), said. */
static int NameOptionTake(int* argc, char* argv[])
{
    if (*argc < 2 || strcmp(argv[1], "--name") != 0) return 1;
    if (*argc < 3) return 0;
    if (!argv[2][0] || !TallyNameSpelled((const uint8_t*)argv[2], strlen(argv[2]))) {
        fprintf(stderr, "--name \"%s\" is not a name: UTF-8, at most %u bytes, no control character\n", argv[2],
                TALLY_VOLUME_NAME_MOST);
        return 0;
    }
    g_volume_name = argv[2];
    for (int i = 1; i + 2 < *argc + 1; i++) argv[i] = argv[i + 2];
    *argc -= 2;
    return 1;
}

/* The options --kernel <file> and --system <file> at argv[at], in any order
 * and --system as often as there are files, taken out of the arguments while
 * they are there: each file is named to the cargo, and the arguments after it
 * move up. False for an option with no file after it, or no memory. */
static int RoleOptionsTake(int* argc, char* argv[], int at)
{
    while (*argc > at && (strcmp(argv[at], "--kernel") == 0 || strcmp(argv[at], "--system") == 0)) {
        if (*argc < at + 2) return 0;
        if (argv[at][2] == 'k') CargoNameKernel(argv[at + 1]);
        else if (CargoKeepSystem(argv[at + 1]) != 0) return 0;
        for (int i = at; i + 2 < *argc + 1; i++) argv[i] = argv[i + 2];
        *argc -= 2;
    }
    return 1;
}

int main(int argc, char* argv[]) {
    if (!BlockOptionTake(&argc, argv) || !NameOptionTake(&argc, argv)) return Usage(argv[0]);
    /* --ground answers a question about a file list and touches no medium;
     * --frame-only writes no book a name would be kept in. */
    if (g_volume_name && argc >= 2 && (strcmp(argv[1], "--ground") == 0 || strcmp(argv[1], "--frame-only") == 0))
        return Usage(argv[0]);
    if (argc >= 2 && strcmp(argv[1], "--ground") == 0) {
        if (!RoleOptionsTake(&argc, argv, 2) || (argc - 2) % 2 != 0) return Usage(argv[0]);
        return GroundSay((argc - 2) / 2, argv, 2);
    }

    /* --frame-only lays a Deed over files and writes no book. */
    if (argc >= 3 && strcmp(argv[1], "--frame-only") == 0) {
        if (!RoleOptionsTake(&argc, argv, 2) || argc < 3 || (argc - 3) % 2 != 0) return Usage(argv[0]);
        return FrameOnlyMake(argv[2], (argc - 3) / 2, argv, 3);
    }

    if (!RoleOptionsTake(&argc, argv, 1) || argc < 2 || (argc - 2) % 2 != 0 || argv[1][0] == '-')
        return Usage(argv[0]);
    return BookMake(argv[1], (argc - 2) / 2, argv, 2);
}
