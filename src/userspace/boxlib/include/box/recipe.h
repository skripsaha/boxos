#ifndef BOX_RECIPE_H
#define BOX_RECIPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"
#include "box/sha256.h"

/*
 * Recipe — what a program was made from, kept in its own image.
 *
 * The tool that makes a program writes it into the image's .boxos.recipe
 * section, beside the Form: every file the build read, by id, with the
 * SHA-256 of the bytes read and the name the file had; the Use Context the
 * build looked in; and the tool's own name. The Form says what a program
 * takes; the Recipe says where it came from. Whoever starts the program can
 * tell whether it is still what its files make, and have the tool make it
 * again — in the context it was made in — by handing it the program:
 * `assemble it`, `it` being the program.
 *
 *     recipe 1
 *     by assemble
 *     context demo
 *     from #41 <64 hex digits> planets.asm      the file the build began with
 *     from #42 <64 hex digits> planets.inc      every file it read after it
 *
 * Lines end with '\n'. `context` with nothing after it: the build had no Use
 * Context. A name is the rest of its line, as the file was called then: names
 * are for saying, ids are what is followed, and a byte below 0x20 or 0x7F in
 * a name is written '?'. The digits are lowercase. A line of a kind the
 * reader does not know is passed over: a later Recipe may say more.
 *
 * Section empty or absent: the program carries no Recipe — it was made
 * elsewhere (the system's own programs are built on the host).
 */

typedef struct {
    file_id_t Id;
    uint8_t   Digest[SHA256_BYTES];
    char     *Name;
} BoxRecipeFile;

typedef struct {
    char          *By;         /* the tool that makes it again, by the name the shell knows it */
    char          *Context;    /* the Use Context it was made in, "" for none */
    BoxRecipeFile *Files;      /* Files[0] is the one the build began with */
    size_t         FileCount;
    uint8_t        Declared;   /* the image carries a Recipe */
    uint8_t        Image;      /* the file is a program image at all */
} BoxRecipe;

/* The Recipe from its text: 0, or -ERR_INVALID_ARGUMENT for text that is no
 * Recipe one could follow (no `recipe 1` first, no `by`, no `context`, no
 * `from`, a line of a known kind spelled wrong) — nothing is kept then. */
int  recipe_parse(const char *text, size_t length, BoxRecipe *out);
/* The Recipe a program on the volume carries. 0 with Declared 0 when it has
 * none; Image says whether the file is a program image at all. */
int  recipe_of_file(file_id_t program, BoxRecipe *out);
void recipe_release(BoxRecipe *recipe);

/* The text of By, Context and Files, as above, into a heap buffer the caller
 * frees. -ERR_INVALID_ARGUMENT for a Recipe that could not be read back: no
 * By, no file, a file 0, a line break in By or Context. */
int  recipe_text(const BoxRecipe *recipe, char **text, size_t *length);

typedef enum {
    RECIPE_SAME = 0,           /* the file is as the build read it */
    RECIPE_CHANGED,            /* it is there, its bytes are not those */
    RECIPE_GONE                /* no file has that id any more, or it is in the trash */
} BoxRecipeState;

/* Each file of the Recipe read again from the volume and held to its
 * digest: States[i] for Files[i], an array the caller frees. 0, or a
 * negative -error_t when the volume would not answer for a file (nothing is
 * known then). */
int  recipe_check(const BoxRecipe *recipe, uint8_t **states);

#ifdef __cplusplus
}
#endif

#endif /* BOX_RECIPE_H */
