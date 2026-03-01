#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TAGFS_MAGIC             0x54414746
#define TAGFS_METADATA_MAGIC    0x544D4554
#define TAGFS_VERSION           2
#define TAGFS_BLOCK_SIZE        4096
#define TAGFS_MAX_FILES         1024
#define TAGFS_FILE_ACTIVE       (1 << 0)

// Journal constants (must match kernel journal.h)
#define JOURNAL_MAGIC               0x4A4F5552  // "JOUR"
#define JOURNAL_VERSION             1
#define JOURNAL_SUPERBLOCK_SECTOR   2059
#define JOURNAL_SUPERBLOCK_BACKUP   2060
#define JOURNAL_ENTRIES_START       2061
#define JOURNAL_ENTRY_COUNT         512

typedef struct __attribute__((packed)) {
    uint8_t type;
    char key[11];
    char value[12];
} TagFSTag;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_files;
    uint32_t max_files;
    uint32_t metadata_start_sector;
    uint32_t data_start_sector;
    uint32_t tag_index_block;
    uint64_t fs_created_time;
    uint64_t fs_modified_time;
    uint8_t  fs_uuid[16];
    uint8_t  reserved[440];
} TagFSSuperblock;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t file_id;
    uint32_t flags;
    uint64_t size;
    uint32_t start_block;
    uint32_t block_count;
    uint64_t created_time;
    uint64_t modified_time;
    uint8_t  tag_count;
    uint8_t  reserved1[3];
    char filename[32];
    TagFSTag tags[16];
    uint8_t  reserved2[48];
} TagFSMetadata;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t start_sector;
    uint32_t entry_count;
    uint32_t head;
    uint32_t tail;
    uint32_t commit_seq;
    uint8_t  reserved[484];
} JournalSuperblock;

void generate_uuid(uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) {
        uuid[i] = rand() & 0xFF;
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

int parse_tags(const char* tag_string, TagFSTag* tags, uint8_t* count) {
    *count = 0;
    char buffer[256];
    strncpy(buffer, tag_string, 255);
    buffer[255] = '\0';

    char* token = strtok(buffer, ",");
    while (token && *count < 16) {
        while (*token == ' ') token++;

        char* colon = strchr(token, ':');

        if (!colon) {
            tags[*count].type = 1;
            strncpy(tags[*count].key, token, 10);
            tags[*count].key[10] = '\0';
            tags[*count].value[0] = '\0';
        } else {
            *colon = '\0';
            const char* key = token;
            const char* value = colon + 1;

            tags[*count].type = 0;
            strncpy(tags[*count].key, key, 10);
            tags[*count].key[10] = '\0';
            strncpy(tags[*count].value, value, 11);
            tags[*count].value[11] = '\0';
        }

        (*count)++;
        token = strtok(NULL, ",");
    }

    return 0;
}

int add_file_to_fs(FILE* disk, TagFSSuperblock* sb, const char* filepath, const char* tag_string) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        perror("Failed to open file");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    uint32_t blocks_needed = (file_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    if (blocks_needed > sb->free_blocks) {
        fprintf(stderr, "Not enough free blocks (need %u, have %u)\n", blocks_needed, sb->free_blocks);
        fclose(file);
        return -1;
    }

    uint32_t file_id = sb->total_files + 1;
    uint32_t metadata_index = sb->total_files;
    uint32_t start_block = sb->total_blocks - sb->free_blocks;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = TAGFS_METADATA_MAGIC;
    meta.file_id = file_id;
    meta.flags = TAGFS_FILE_ACTIVE;
    meta.size = file_size;
    meta.start_block = start_block;
    meta.block_count = blocks_needed;
    meta.created_time = (uint64_t)time(NULL);
    meta.modified_time = meta.created_time;

    const char* filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    strncpy(meta.filename, filename, 31);
    meta.filename[31] = '\0';

    if (parse_tags(tag_string, meta.tags, &meta.tag_count) != 0) {
        fclose(file);
        return -1;
    }

    // Auto label-tag: filename stem (without extension) as a label
    {
        char stem[32];
        const char* dot = strrchr(filename, '.');
        size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
        if (stem_len > 31) stem_len = 31;
        memcpy(stem, filename, stem_len);
        stem[stem_len] = '\0';

        if (stem_len > 0 && meta.tag_count < 16) {
            /* Label: kernel expects key=label, value="" (format_tag shows key when value empty) */
            meta.tags[meta.tag_count].type = 1;
            strncpy(meta.tags[meta.tag_count].key, stem, sizeof(meta.tags[0].key) - 1);
            meta.tags[meta.tag_count].key[sizeof(meta.tags[0].key) - 1] = '\0';
            meta.tags[meta.tag_count].value[0] = '\0';
            meta.tag_count++;
        }
    }

    uint32_t metadata_sector = sb->metadata_start_sector + metadata_index;
    if (fseek(disk, metadata_sector * 512, SEEK_SET) != 0) {
        perror("Failed to seek to metadata sector");
        fclose(file);
        return -1;
    }

    if (fwrite(&meta, sizeof(meta), 1, disk) != 1) {
        perror("Failed to write metadata");
        fclose(file);
        return -1;
    }

    uint32_t data_sector = sb->data_start_sector + (start_block * 8);
    if (fseek(disk, data_sector * 512, SEEK_SET) != 0) {
        perror("Failed to seek to data sector");
        fclose(file);
        return -1;
    }

    uint8_t buffer[TAGFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < blocks_needed; i++) {
        memset(buffer, 0, TAGFS_BLOCK_SIZE);
        size_t bytes_to_read = (i == blocks_needed - 1) ? (file_size % TAGFS_BLOCK_SIZE) : TAGFS_BLOCK_SIZE;
        if (bytes_to_read == 0) bytes_to_read = TAGFS_BLOCK_SIZE;

        size_t bytes_read = fread(buffer, 1, bytes_to_read, file);
        if (bytes_read != bytes_to_read) {
            fprintf(stderr, "Failed to read file block %u\n", i);
            fclose(file);
            return -1;
        }

        if (fwrite(buffer, TAGFS_BLOCK_SIZE, 1, disk) != 1) {
            fprintf(stderr, "Failed to write data block %u\n", i);
            fclose(file);
            return -1;
        }
    }

    fclose(file);

    sb->total_files++;
    sb->free_blocks -= blocks_needed;
    sb->fs_modified_time = (uint64_t)time(NULL);

    printf("  Added file: %s (ID=%u, size=%ld bytes, blocks=%u)\n", filename, file_id, file_size, blocks_needed);
    printf("    Tags: %s\n", tag_string);

    return 0;
}

int create_empty_journal(FILE* disk) {
    printf("[create_tagfs] Initializing empty journal...\n");

    uint8_t* buffer = malloc(512);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }

    JournalSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = JOURNAL_MAGIC;
    sb.version = JOURNAL_VERSION;
    sb.start_sector = JOURNAL_ENTRIES_START;
    sb.entry_count = JOURNAL_ENTRY_COUNT;
    sb.head = 0;
    sb.tail = 0;
    sb.commit_seq = 1;

    memset(buffer, 0, 512);
    memcpy(buffer, &sb, sizeof(sb));

    if (fseek(disk, JOURNAL_SUPERBLOCK_SECTOR * 512, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to seek to journal superblock sector\n");
        free(buffer);
        return -1;
    }

    if (fwrite(buffer, 512, 1, disk) != 1) {
        fprintf(stderr, "Failed to write journal superblock\n");
        free(buffer);
        return -1;
    }

    if (fseek(disk, JOURNAL_SUPERBLOCK_BACKUP * 512, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to seek to journal backup sector\n");
        free(buffer);
        return -1;
    }

    if (fwrite(buffer, 512, 1, disk) != 1) {
        fprintf(stderr, "Failed to write journal backup superblock\n");
        free(buffer);
        return -1;
    }

    memset(buffer, 0, 512);

    for (uint32_t i = 0; i < JOURNAL_ENTRY_COUNT * 2; i++) {
        uint32_t sector = JOURNAL_ENTRIES_START + i;
        if (fseek(disk, sector * 512, SEEK_SET) != 0) {
            fprintf(stderr, "Failed to seek to journal entry sector %u\n", i);
            free(buffer);
            return -1;
        }

        if (fwrite(buffer, 512, 1, disk) != 1) {
            fprintf(stderr, "Failed to zero journal entry sector %u\n", i);
            free(buffer);
            return -1;
        }
    }

    free(buffer);
    printf("  Journal initialized (sectors %u-%u)\n",
           JOURNAL_SUPERBLOCK_SECTOR,
           JOURNAL_ENTRIES_START + (JOURNAL_ENTRY_COUNT * 2) - 1);

    return 0;
}

void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <img> <sb_sector> <meta_sector> <data_sector> [<file> <tags>]...\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Create TagFS filesystem in disk image.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  img          - Disk image path\n");
    fprintf(stderr, "  sb_sector    - Superblock sector (e.g., 400)\n");
    fprintf(stderr, "  meta_sector  - Metadata table sector (e.g., 401)\n");
    fprintf(stderr, "  data_sector  - Data start sector (e.g., 1425)\n");
    fprintf(stderr, "  file         - File to add\n");
    fprintf(stderr, "  tags         - Tag string: \"key1:value1,key2:value2\"\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s disk.img 400 401 1425 kernel.bin \"system\"\n", prog);
    fprintf(stderr, "\n");
}

int main(int argc, char* argv[]) {
    if (argc < 5 || (argc - 5) % 2 != 0) {
        print_usage(argv[0]);
        return 1;
    }

    const char* disk_path = argv[1];
    uint32_t sb_sector = atoi(argv[2]);
    uint32_t meta_sector = atoi(argv[3]);
    uint32_t data_sector = atoi(argv[4]);

    printf("[create_tagfs] Formatting TagFS on %s...\n", disk_path);
    printf("  Superblock sector: %u\n", sb_sector);
    printf("  Metadata sector: %u\n", meta_sector);
    printf("  Data start sector: %u\n", data_sector);

    FILE* disk = fopen(disk_path, "r+b");
    if (!disk) {
        perror("Failed to open disk image");
        return 1;
    }

    fseek(disk, 0, SEEK_END);
    long disk_size = ftell(disk);
    fseek(disk, 0, SEEK_SET);

    if (disk_size < (data_sector * 512)) {
        fprintf(stderr, "Disk image too small (need at least %u bytes)\n", data_sector * 512);
        fclose(disk);
        return 1;
    }

    uint32_t total_sectors = disk_size / 512;
    uint32_t data_sectors = total_sectors - data_sector;
    uint32_t total_blocks = data_sectors / 8;

    TagFSSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = TAGFS_MAGIC;
    sb.version = TAGFS_VERSION;
    sb.block_size = TAGFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks;
    sb.free_blocks = total_blocks;
    sb.total_files = 0;
    sb.max_files = TAGFS_MAX_FILES;
    sb.metadata_start_sector = meta_sector;
    sb.data_start_sector = data_sector;
    sb.tag_index_block = 0;
    sb.fs_created_time = (uint64_t)time(NULL);
    sb.fs_modified_time = sb.fs_created_time;

    srand(sb.fs_created_time);
    generate_uuid(sb.fs_uuid);

    printf("\n[create_tagfs] Filesystem parameters:\n");
    printf("  Total blocks: %u (%u MB)\n", sb.total_blocks, (sb.total_blocks * 4) / 1024);
    printf("  Max files: %u\n", sb.max_files);

    TagFSMetadata empty_metadata;
    memset(&empty_metadata, 0, sizeof(empty_metadata));

    for (uint32_t i = 0; i < TAGFS_MAX_FILES; i++) {
        if (fseek(disk, (meta_sector + i) * 512, SEEK_SET) != 0) {
            fprintf(stderr, "Failed to seek to metadata entry %u\n", i);
            fclose(disk);
            return 1;
        }

        if (fwrite(&empty_metadata, sizeof(empty_metadata), 1, disk) != 1) {
            fprintf(stderr, "Failed to write metadata entry %u\n", i);
            fclose(disk);
            return 1;
        }
    }

    printf("\n[create_tagfs] Adding files...\n");
    for (int i = 5; i < argc; i += 2) {
        const char* filepath = argv[i];
        const char* tags = argv[i + 1];

        if (add_file_to_fs(disk, &sb, filepath, tags) != 0) {
            fprintf(stderr, "Failed to add file: %s\n", filepath);
            fclose(disk);
            return 1;
        }
    }

    if (fseek(disk, sb_sector * 512, SEEK_SET) != 0) {
        perror("Failed to seek to superblock");
        fclose(disk);
        return 1;
    }

    if (fwrite(&sb, sizeof(sb), 1, disk) != 1) {
        perror("Failed to write superblock");
        fclose(disk);
        return 1;
    }

    // Initialize empty journal
    if (create_empty_journal(disk) != 0) {
        fprintf(stderr, "Failed to initialize journal\n");
        fclose(disk);
        return 1;
    }

    fclose(disk);

    printf("\n[create_tagfs] TagFS formatting complete!\n");
    printf("  Total files: %u\n", sb.total_files);
    printf("  Free blocks: %u / %u\n", sb.free_blocks, sb.total_blocks);
    return 0;
}
