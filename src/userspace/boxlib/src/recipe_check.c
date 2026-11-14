/*
 * recipe_check.c — the files of a Recipe read again from the volume and held
 * to the digests the build kept.
 *
 * Kept apart from recipe.c so a reader of the text alone does not bring the
 * volume with it.
 */

#include "box/recipe.h"
#include "box/file.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define RECIPE_PIECE 65536u

/* The file's digest as it is now, read piece by piece — a short read is the
 * volume answering with what it had in hand, asked again until the whole
 * size is read. */
static int digest_now(file_id_t file, uint64_t size, uint8_t *piece, uint8_t digest[SHA256_BYTES])
{
    Sha256   state;
    uint64_t offset = 0;

    sha256_begin(&state);
    while (offset < size) {
        uint64_t left = size - offset;
        int64_t  got  = fread(file, offset, piece, left > RECIPE_PIECE ? RECIPE_PIECE : (size_t)left);
        if (got <= 0 || (uint64_t)got > left) return got < 0 ? (int)got : -ERR_IO;
        sha256_add(&state, piece, (size_t)got);
        offset += (uint64_t)got;
    }
    sha256_end(&state, digest);
    return 0;
}

static int state_of(const BoxRecipeFile *file, uint8_t *piece, uint8_t *state)
{
    FileMetadata metadata;
    int rc = FileMetadataRead(file->Id, &metadata);
    if (rc == -ERR_FILE_NOT_FOUND) {
        *state = RECIPE_GONE;
        return 0;
    }
    if (rc != 0) return rc;
    uint64_t size    = metadata.Size;
    bool     deleted = metadata.KeptUntil != 0;
    FileMetadataRelease(&metadata);
    if (deleted) {
        *state = RECIPE_GONE;
        return 0;
    }

    uint8_t digest[SHA256_BYTES];
    rc = digest_now(file->Id, size, piece, digest);
    if (rc != 0) return rc;
    *state = memcmp(digest, file->Digest, SHA256_BYTES) == 0 ? RECIPE_SAME : RECIPE_CHANGED;
    return 0;
}

int recipe_check(const BoxRecipe *recipe, uint8_t **states)
{
    if (!recipe || !states || !recipe->Declared || recipe->FileCount == 0) return -ERR_INVALID_ARGUMENT;
    *states = NULL;

    uint8_t *found = malloc(recipe->FileCount);
    uint8_t *piece = malloc(RECIPE_PIECE);
    int      rc    = found && piece ? 0 : -ERR_NO_MEMORY;

    for (size_t i = 0; rc == 0 && i < recipe->FileCount; i++)
        rc = state_of(&recipe->Files[i], piece, &found[i]);
    free(piece);
    if (rc != 0) {
        free(found);
        return rc;
    }
    *states = found;
    return 0;
}
