#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/recipe.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("takes optional thing \"the file to tell about\"\n"
         "gives things\n");

static int system_summary(void) {
    system_info_t sys;
    if (sysinfo(&sys) != 0) {
        printf("%colorError:%color Failed to get system info\n",
               COLOR_RED, COLOR_DEFAULT);
        return 1;
    }

    uint64_t up_ms = sys.uptime_ns / 1000000ULL;
    printf("%color%s%color  uptime %u ms  procs %u\n",
           COLOR_CYAN, sys.name, COLOR_DEFAULT,
           (unsigned)up_ms, sys.process_count);
    printf("  RAM:  %u MB used / %u MB total\n",
           (unsigned)(sys.used_memory  >> 20),
           (unsigned)(sys.total_memory >> 20));
    printf("  CPUs: %u (%u K, %u App)%s\n",
           sys.cpu_total, sys.cpu_k_cores, sys.cpu_app_cores,
           sys.multicore_active ? " AMP" : "");
    return 0;
}

/* The name of a program's file while it is on the volume — a heap copy — and
 * NULL when it is not (a file id is never given twice) or memory is out. */
static char *program_named(file_id_t program) {
    FileMetadata metadata;
    if (FileMetadataRead(MY_TAGS, program, &metadata) != 0) return NULL;
    char *name = NULL;
    if (metadata.KeptUntil == 0 && metadata.Filename) {
        size_t length = strlen(metadata.Filename);
        name          = malloc(length + 1);
        if (name) memcpy(name, metadata.Filename, length + 1);
    }
    FileMetadataRelease(&metadata);
    return name;
}

/* Each tag `owner` owns of the comma list the volume keeps, one a line. */
static void print_owned(const FileMetadata *metadata, file_id_t owner) {
    const char *tag = metadata->Tags;
    for (uint32_t i = 0; tag && *tag; i++) {
        const char *end = tag;
        while (*end && *end != ',') end++;
        if ((metadata->Owners ? metadata->Owners[i] : PERSON_OWNS) == owner)
            printf("    %.*s\n", (int)(end - tag), tag);
        tag = *end ? end + 1 : end;
    }
}

/* The person's tags, then the marks each program put on the file for itself,
 * under the program's name: the person sees every mark on the file. */
static void print_tags(const FileMetadata *metadata) {
    println("  Tags:");
    print_owned(metadata, PERSON_OWNS);
    for (uint32_t i = 0; metadata->Owners && i < metadata->TagCount; i++) {
        file_id_t owner = metadata->Owners[i];
        bool      told  = owner == PERSON_OWNS;
        for (uint32_t j = 0; j < i && !told; j++) told = metadata->Owners[j] == owner;
        if (told) continue;
        char *name = program_named(owner);
        if (name) printf("  Marks of the program %s:\n", name);
        else      println("  Marks of a program no longer on the volume:");
        free(name);
        print_owned(metadata, owner);
    }
}

/* What the system keeps the file as, when it is more than an ordinary file. */
static void print_role(uint8_t role) {
    const char *as = role == FILE_ROLE_SYSTEM ? "a system file"
                     : role == FILE_ROLE_BOOT ? "a boot file"
                     : role == FILE_ROLE_KERNEL ? "the kernel"
                                                : NULL;
    if (as) printf("  Kept by the system as %s\n", as);
}

/* Who made the file: the program's file, by its name while that file is on the
 * volume, and an honest word when it is not (a file id is never given twice).
 * Nothing for a file no program of the volume made. */
static void print_maker(file_id_t maker) {
    if (maker == 0) return;
    char *name = program_named(maker);
    if (name) printf("  Made by the program %s\n", name);
    else      println("  Made by a program that is no longer on the volume");
    free(name);
}

/* What a program made here was made from, by which tool and in which Use
 * Context, and how each of those files stands now. Nothing for a file that
 * carries no Recipe. */
static void print_recipe(file_id_t file) {
    BoxRecipe recipe;
    if (recipe_of_file(file, &recipe) != 0) {
        println("  Recipe: it cannot be read");
        return;
    }
    if (!recipe.Declared) return;

    uint8_t *states = NULL;
    bool known = recipe_check(&recipe, &states) == 0;
    if (recipe.Context[0])
        printf("  Made by %s in the Use Context %s, from:\n", recipe.By, recipe.Context);
    else
        printf("  Made by %s with no Use Context, from:\n", recipe.By);
    for (size_t i = 0; i < recipe.FileCount; i++) {
        const char *now = !known                          ? ""
                          : states[i] == RECIPE_CHANGED ? " (changed since)"
                          : states[i] == RECIPE_GONE    ? " (gone)"
                                                        : "";
        printf("    %s%s\n", recipe.Files[i].Name, now);
    }
    if (!known) println("    (whether they changed cannot be read now)");
    free(states);
    recipe_release(&recipe);
}

int main(void) {
    const file_id_t *file;
    size_t file_count;

    if (form_things(0, &file, &file_count) != 0) {
        exit(1);
        return 1;
    }

    if (file_count == 0) {
        int status = system_summary();
        exit(status);
        return status;
    }

    FileMetadata metadata;
    if (FileMetadataRead(ALL_TAGS, file[0], &metadata) != 0) {
        println("Could not look at that file; it may have been erased");
        exit(1);
        return 1;
    }

    println("File Info:");
    printf("  Filename: %s\n", metadata.Filename);
    printf("  Size: %u bytes\n", (unsigned)metadata.Size);
    print_role(metadata.Role);
    print_tags(&metadata);
    const file_id_t maker = metadata.MadeBy;
    FileMetadataRelease(&metadata);
    print_maker(maker);
    print_recipe(file[0]);

    souvenir_things(file, 1);
    exit(0);
    return 0;
}
