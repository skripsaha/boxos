#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/recipe.h"
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

/* One tag a line: the comma list the volume keeps, split at its commas. */
static void print_tags(const char *tags) {
    println("  Tags:");
    while (tags && *tags) {
        const char *end = tags;
        while (*end && *end != ',') end++;
        printf("    %.*s\n", (int)(end - tags), tags);
        tags = *end ? end + 1 : end;
    }
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
    if (FileMetadataRead(file[0], &metadata) != 0) {
        println("Could not look at that file; it may have been erased");
        exit(1);
        return 1;
    }

    println("File Info:");
    printf("  Filename: %s\n", metadata.Filename);
    printf("  Size: %u bytes\n", (unsigned)metadata.Size);
    print_tags(metadata.Tags);
    FileMetadataRelease(&metadata);
    print_recipe(file[0]);

    souvenir_things(file, 1);
    exit(0);
    return 0;
}
