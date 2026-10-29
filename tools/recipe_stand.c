/* recipe_stand — BoxLib's Recipe (src/recipe*.c) and SHA-256 (src/sha256.c)
 * on the host, over a made-up volume.
 *
 *     tools/recipe_stand.sh
 *
 * Only BoxLib's own headers are seen here, so the sources are built as they
 * are in BoxOS; fread and the metadata reads are renamed on the command line
 * to the readers below. SHA-256 is held to the vectors of FIPS 180-4 and, line
 * by line on standard output, to the host's own implementation (the script
 * checks every `digest LEN HEX` line). The Recipe's text is held to exact
 * bytes both ways, every refusal to its error, the image to what the reader
 * finds in it, and the check to the state of every file. */

#include "box/defs.h"
#include "box/types.h"
#include "box/error.h"
#include "box/file.h"
#include "box/recipe.h"
#include "box/sha256.h"
#include "box/string.h"
#include "box/memory.h"

int printf(const char *format, ...);

static int g_passed;
static int g_failed;

static void check_int(const char *label, long got, long want)
{
    if (got == want) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %ld\n  want: %ld\n", label, got, want);
}

static void check_text(const char *label, const char *got, const char *want)
{
    if (got && strcmp(got, want) == 0) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %s\n  want: %s\n", label, got ? got : "(null)", want);
}

static void hex_of(const uint8_t digest[SHA256_BYTES], char out[2 * SHA256_BYTES + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_BYTES; i++) {
        out[2 * i]     = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[2 * SHA256_BYTES] = '\0';
}

/* ---- the volume ---------------------------------------------------------- */

typedef struct {
    file_id_t   Id;
    const char *Bytes;
    size_t      Size;
    uint32_t    Flags;
    bool        Refuses;       /* the volume will not answer for it */
} StandFile;

#define STAND_FILES 8u

static StandFile      g_files[STAND_FILES];
static size_t         g_file_count;
static unsigned char *g_image;
static size_t         g_image_size;
static size_t         g_read_most = (size_t)-1;   /* the most one fread hands back */

static void volume_reset(void)
{
    g_file_count = 0;
    g_read_most  = (size_t)-1;
}

static void volume_add(file_id_t id, const char *bytes, size_t size, uint32_t flags, bool refuses)
{
    g_files[g_file_count++] = (StandFile){ id, bytes, size, flags, refuses };
}

static const StandFile *volume_file(file_id_t id)
{
    for (size_t i = 0; i < g_file_count; i++) {
        if (g_files[i].Id == id) return &g_files[i];
    }
    return NULL;
}

/* File 7 is the image; every other file is in the table. */
int64_t StandRead(file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    const char *bytes;
    size_t      whole;

    if (file == 7) {
        bytes = (const char *)g_image;
        whole = g_image_size;
    } else {
        const StandFile *found = volume_file(file);
        if (!found) return -ERR_FILE_NOT_FOUND;
        if (found->Refuses) return -ERR_IO;
        bytes = found->Bytes;
        whole = found->Size;
    }
    if (offset >= whole) return 0;
    size_t take = whole - (size_t)offset;
    if (take > size) take = size;
    if (take > g_read_most) take = g_read_most;
    memcpy(buffer, bytes + offset, take);
    return (int64_t)take;
}

int StandMetadata(file_id_t file, FileMetadata *out)
{
    memset(out, 0, sizeof(*out));
    if (file == 7) {
        out->Size = g_image_size;
        return 0;
    }
    const StandFile *found = volume_file(file);
    if (!found) return -ERR_FILE_NOT_FOUND;
    if (found->Refuses && found->Size == 0) return -ERR_IO;
    out->Size  = found->Size;
    out->Flags = found->Flags;
    return 0;
}

void StandMetadataRelease(FileMetadata *metadata)
{
    (void)metadata;
}

/* ---- SHA-256 ------------------------------------------------------------- */

static void check_digest(const char *label, const void *bytes, size_t length, const char *want)
{
    uint8_t digest[SHA256_BYTES];
    char    hex[2 * SHA256_BYTES + 1];

    sha256(bytes, length, digest);
    hex_of(digest, hex);
    check_text(label, hex, want);
}

/* The same bytes fed one byte, then pieces of every size up to 130: the
 * digest never depends on how the bytes arrive. */
static void check_pieces(const uint8_t *bytes, size_t length)
{
    uint8_t whole[SHA256_BYTES], pieces[SHA256_BYTES];
    sha256(bytes, length, whole);

    for (size_t piece = 1; piece <= 130; piece++) {
        Sha256 state;
        sha256_begin(&state);
        for (size_t at = 0; at < length; at += piece)
            sha256_add(&state, bytes + at, length - at < piece ? length - at : piece);
        sha256_end(&state, pieces);
        if (memcmp(whole, pieces, SHA256_BYTES) != 0) {
            check_int("sha256 pieces", (long)piece, 0);
            return;
        }
    }
    check_int("sha256 pieces", 0, 0);
}

static void test_sha256(void)
{
    check_digest("sha256 empty", "", 0,
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_digest("sha256 abc", "abc", 3,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check_digest("sha256 448 bits", two_blocks, sizeof(two_blocks) - 1,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static const char long_vector[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    check_digest("sha256 896 bits", long_vector, sizeof(long_vector) - 1,
                 "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    char *million = malloc(1000000);
    memset(million, 'a', 1000000);
    check_digest("sha256 a million a", million, 1000000,
                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million);

    uint8_t pattern[1100];
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i * 131u + 7u);
    check_pieces(pattern, sizeof(pattern));

    /* Every length from nothing to a little past a thousand, for the host to
     * check: each crosses the padding's edges somewhere. */
    for (size_t length = 0; length <= sizeof(pattern); length++) {
        uint8_t digest[SHA256_BYTES];
        char    hex[2 * SHA256_BYTES + 1];
        sha256(pattern, length, digest);
        hex_of(digest, hex);
        printf("digest %u %s\n", (unsigned)length, hex);
    }
}

/* ---- the text ------------------------------------------------------------ */

#define DIGEST_ABC   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define DIGEST_EMPTY "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

static BoxRecipeFile g_two[2];

static BoxRecipe two_files(const char *by, const char *context)
{
    g_two[0].Id = 41;
    sha256("abc", 3, g_two[0].Digest);
    g_two[0].Name = (char *)"planets.asm";
    g_two[1].Id = 18446744073709551615ull;
    sha256("", 0, g_two[1].Digest);
    g_two[1].Name = (char *)"my lines.inc";
    BoxRecipe recipe = { (char *)by, (char *)context, g_two, 2, 1, 1 };
    return recipe;
}

static const char g_text[] =
    "recipe 1\n"
    "by assemble\n"
    "context demo,asm\n"
    "from #41 " DIGEST_ABC " planets.asm\n"
    "from #18446744073709551615 " DIGEST_EMPTY " my lines.inc\n";

static void check_written(const char *label, const BoxRecipe *recipe, const char *want)
{
    char  *text = NULL;
    size_t length = 0;
    int    rc = recipe_text(recipe, &text, &length);
    check_int(label, rc, 0);
    if (rc != 0) return;
    char *ended = malloc(length + 1);
    memcpy(ended, text, length);
    ended[length] = '\0';
    check_text(label, ended, want);
    free(ended);
    free(text);
}

static int parse_text(const char *text, size_t length)
{
    BoxRecipe recipe;
    int rc = recipe_parse(text, length, &recipe);
    if (rc == 0) recipe_release(&recipe);
    return rc;
}

#define REFUSED(label, text) \
    check_int("parse " label, parse_text(text, sizeof(text) - 1), -ERR_INVALID_ARGUMENT)
#define READ(label, text) check_int("parse " label, parse_text(text, sizeof(text) - 1), 0)

#define HEAD "recipe 1\nby assemble\ncontext demo\n"
#define FROM "from #41 " DIGEST_ABC " planets.asm\n"

static void test_text(void)
{
    BoxRecipe recipe = two_files("assemble", "demo,asm");
    check_written("text two files", &recipe, g_text);

    BoxRecipe read;
    check_int("parse two files", recipe_parse(g_text, sizeof(g_text) - 1, &read), 0);
    check_int("parse declared", read.Declared, 1);
    check_text("parse by", read.By, "assemble");
    check_text("parse context", read.Context, "demo,asm");
    check_int("parse count", (long)read.FileCount, 2);
    check_int("parse first id", (long)read.Files[0].Id, 41);
    check_int("parse first digest", memcmp(read.Files[0].Digest, g_two[0].Digest, SHA256_BYTES), 0);
    check_text("parse first name", read.Files[0].Name, "planets.asm");
    check_int("parse big id", read.Files[1].Id == 18446744073709551615ull, 1);
    check_text("parse name with a blank", read.Files[1].Name, "my lines.inc");
    check_int("parse second digest", memcmp(read.Files[1].Digest, g_two[1].Digest, SHA256_BYTES), 0);
    recipe_release(&read);
    check_int("released", read.FileCount == 0 && !read.Files && !read.By, 1);

    recipe = two_files("assemble", "");
    check_written("text no context", &recipe,
                  "recipe 1\nby assemble\ncontext\nfrom #41 " DIGEST_ABC " planets.asm\n"
                  "from #18446744073709551615 " DIGEST_EMPTY " my lines.inc\n");
    recipe = two_files("assemble", NULL);
    char *text;
    size_t length;
    check_int("text null context", recipe_text(&recipe, &text, &length), 0);
    check_int("parse null context", recipe_parse(text, length, &read), 0);
    check_text("parse empty context", read.Context, "");
    recipe_release(&read);
    free(text);

    recipe = two_files("assemble", "demo");
    g_two[0].Name = (char *)"a\tb\x7f" "c";
    check_written("text name shown", &recipe,
                  "recipe 1\nby assemble\ncontext demo\nfrom #41 " DIGEST_ABC " a?b?c\n"
                  "from #18446744073709551615 " DIGEST_EMPTY " my lines.inc\n");

    recipe = two_files(NULL, "demo");
    check_int("text no by", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("", "demo");
    check_int("text empty by", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("as\nsemble", "demo");
    check_int("text by two lines", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("assemble", "de\nmo");
    check_int("text context two lines", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("assemble", "de\rmo");
    check_int("text context return", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("assemble", "demo");
    recipe.FileCount = 0;
    check_int("text no file", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("assemble", "demo");
    g_two[1].Id = 0;
    check_int("text file 0", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    recipe = two_files("assemble", "demo");
    g_two[1].Name = (char *)"";
    check_int("text unnamed", recipe_text(&recipe, &text, &length), -ERR_INVALID_ARGUMENT);
    check_int("text nothing", recipe_text(NULL, &text, &length), -ERR_INVALID_ARGUMENT);

    READ("good", HEAD FROM);
    READ("no last line break", HEAD "from #41 " DIGEST_ABC " planets.asm");
    READ("unknown line", HEAD "made on a tuesday\n" FROM);
    READ("empty line", HEAD "\n" FROM);
    READ("word like a kind", HEAD "bypass x\n" FROM);
    READ("lines in any order", "recipe 1\n" FROM "context demo\nby assemble\n");
    REFUSED("nothing", "");
    REFUSED("not first", "by assemble\nrecipe 1\ncontext demo\n" FROM);
    REFUSED("recipe 2", "recipe 2\nby assemble\ncontext demo\n" FROM);
    REFUSED("recipe twice", HEAD "recipe 1\n" FROM);
    REFUSED("no by", "recipe 1\ncontext demo\n" FROM);
    REFUSED("by twice", HEAD "by other\n" FROM);
    REFUSED("by empty", "recipe 1\nby\ncontext demo\n" FROM);
    REFUSED("no context", "recipe 1\nby assemble\n" FROM);
    REFUSED("context twice", HEAD "context other\n" FROM);
    REFUSED("no from", HEAD);
    REFUSED("id 0", HEAD "from #0 " DIGEST_ABC " planets.asm\n");
    REFUSED("id leading zero", HEAD "from #041 " DIGEST_ABC " planets.asm\n");
    REFUSED("id letters", HEAD "from #4x " DIGEST_ABC " planets.asm\n");
    REFUSED("id overflow", HEAD "from #18446744073709551616 " DIGEST_ABC " planets.asm\n");
    REFUSED("id no hash", HEAD "from 41 " DIGEST_ABC " planets.asm\n");
    REFUSED("id empty", HEAD "from # " DIGEST_ABC " planets.asm\n");
    REFUSED("digest upper", HEAD "from #41 BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD planets.asm\n");
    REFUSED("digest short", HEAD "from #41 ba7816bf planets.asm\n");
    REFUSED("digest long", HEAD "from #41 " DIGEST_ABC "0 planets.asm\n");
    REFUSED("no name", HEAD "from #41 " DIGEST_ABC "\n");
    REFUSED("name empty", HEAD "from #41 " DIGEST_ABC " \n");
    REFUSED("from bare", HEAD "from\n");
    REFUSED("nul inside", HEAD "from #41 " DIGEST_ABC " plan\0ets.asm\n");
}

/* ---- the image ------------------------------------------------------------ */

static void put32(unsigned char *at, uint64_t value) { for (int i = 0; i < 4; i++) at[i] = (unsigned char)(value >> (8 * i)); }
static void put64(unsigned char *at, uint64_t value) { for (int i = 0; i < 8; i++) at[i] = (unsigned char)(value >> (8 * i)); }

/* An ELF64 image of three sections: .text, the section called Section
 * holding Text, and the names. */
static void build_image(const char *section, const char *text, size_t text_length, bool elf)
{
    char   names[64];
    size_t section_length = strlen(section);
    size_t names_length = 0;

    memcpy(names, "\0.text\0", 7);
    names_length = 7;
    memcpy(names + names_length, section, section_length + 1);
    names_length += section_length + 1;
    memcpy(names + names_length, ".shstrtab", 10);
    names_length += 10;

    size_t text_at = 64, names_at = text_at + text_length;
    size_t headers_at = (names_at + names_length + 7) & ~(size_t)7;
    free(g_image);
    g_image_size = headers_at + 4 * 64;
    g_image = calloc(1, g_image_size);
    memcpy(g_image, elf ? "\x7f" "ELF" : "TEXT", 4);
    g_image[4] = 2;
    g_image[5] = 1;
    put64(g_image + 40, headers_at);
    g_image[58] = 64;
    g_image[60] = 4;
    g_image[62] = 3;
    memcpy(g_image + text_at, text, text_length);
    memcpy(g_image + names_at, names, names_length);

    unsigned char *headers = g_image + headers_at;
    put32(headers + 64, 1);
    put32(headers + 64 + 4, 1);
    put32(headers + 128, 7);
    put32(headers + 128 + 4, 1);
    put64(headers + 128 + 24, text_at);
    put64(headers + 128 + 32, text_length);
    put32(headers + 192, (uint32_t)(7 + section_length + 1));
    put32(headers + 192 + 4, 3);
    put64(headers + 192 + 24, names_at);
    put64(headers + 192 + 32, names_length);
}

static void test_image(void)
{
    BoxRecipe recipe;

    build_image(".boxos.recipe", g_text, sizeof(g_text) - 1, true);
    check_int("image good", recipe_of_file(7, &recipe), 0);
    check_int("image declared", recipe.Declared, 1);
    check_int("image is image", recipe.Image, 1);
    check_int("image files", (long)recipe.FileCount, 2);
    check_text("image by", recipe.By, "assemble");
    recipe_release(&recipe);

    build_image(".boxos.form", g_text, sizeof(g_text) - 1, true);
    check_int("image without recipe", recipe_of_file(7, &recipe), 0);
    check_int("image without recipe declared", recipe.Declared, 0);
    check_int("image without recipe is image", recipe.Image, 1);

    build_image(".boxos.recipe", "", 0, true);
    check_int("image empty recipe", recipe_of_file(7, &recipe), 0);
    check_int("image empty recipe declared", recipe.Declared, 0);
    check_int("image empty recipe is image", recipe.Image, 1);

    build_image(".boxos.recipe", g_text, sizeof(g_text) - 1, false);
    check_int("not an image", recipe_of_file(7, &recipe), 0);
    check_int("not an image declared", recipe.Declared, 0);
    check_int("not an image is image", recipe.Image, 0);

    static const char bad[] = "recipe 1\nby assemble\n";
    build_image(".boxos.recipe", bad, sizeof(bad) - 1, true);
    check_int("image bad recipe", recipe_of_file(7, &recipe), -ERR_INVALID_ARGUMENT);
    check_int("image bad recipe declared", recipe.Declared, 0);

    check_int("image no out", recipe_of_file(7, NULL), -ERR_INVALID_ARGUMENT);
    check_int("image no file", recipe_of_file(99, &recipe), -ERR_FILE_NOT_FOUND);
}

/* ---- the check ------------------------------------------------------------ */

static long states_of(const BoxRecipe *recipe, char *out)
{
    uint8_t *states;
    int rc = recipe_check(recipe, &states);
    out[0] = '\0';
    if (rc != 0) return rc;
    for (size_t i = 0; i < recipe->FileCount; i++) {
        static const char *const names[] = { "same", "changed", "gone" };
        if (i > 0) strcat(out, " ");
        strcat(out, names[states[i]]);
    }
    free(states);
    return 0;
}

static void test_check(void)
{
    static char big[200000];
    char  shown[64];
    BoxRecipeFile files[3];

    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i % 251);
    files[0] = (BoxRecipeFile){ 41, { 0 }, (char *)"planets.asm" };
    files[1] = (BoxRecipeFile){ 42, { 0 }, (char *)"planets.inc" };
    files[2] = (BoxRecipeFile){ 43, { 0 }, (char *)"big.inc" };
    sha256("abc", 3, files[0].Digest);
    sha256("", 0, files[1].Digest);
    sha256(big, sizeof(big), files[2].Digest);
    BoxRecipe recipe = { (char *)"assemble", (char *)"demo", files, 3, 1, 1 };

    volume_reset();
    volume_add(41, "abc", 3, 0, false);
    volume_add(42, "", 0, 0, false);
    volume_add(43, big, sizeof(big), 0, false);
    check_int("check same", states_of(&recipe, shown), 0);
    check_text("check same states", shown, "same same same");

    g_read_most = 7;
    check_int("check short reads", states_of(&recipe, shown), 0);
    check_text("check short reads states", shown, "same same same");
    g_read_most = (size_t)-1;

    volume_reset();
    volume_add(41, "abd", 3, 0, false);
    volume_add(42, "x", 1, 0, false);
    volume_add(43, big, sizeof(big) - 1, 0, false);
    check_int("check changed", states_of(&recipe, shown), 0);
    check_text("check changed states", shown, "changed changed changed");

    volume_reset();
    volume_add(41, "abc", 3, FILE_FLAG_TRASHED, false);
    volume_add(43, big, sizeof(big), 0, false);
    check_int("check gone", states_of(&recipe, shown), 0);
    check_text("check gone states", shown, "gone gone same");

    volume_reset();
    volume_add(41, "abc", 3, 0, false);
    volume_add(42, "", 0, 0, false);
    volume_add(43, big, sizeof(big), 0, true);
    check_int("check read refused", states_of(&recipe, shown), -ERR_IO);

    volume_reset();
    volume_add(41, "abc", 3, 0, false);
    volume_add(42, "", 0, 0, true);
    check_int("check metadata refused", states_of(&recipe, shown), -ERR_IO);

    uint8_t *states;
    BoxRecipe undeclared = recipe;
    undeclared.Declared = 0;
    check_int("check undeclared", recipe_check(&undeclared, &states), -ERR_INVALID_ARGUMENT);
    check_int("check nothing", recipe_check(NULL, &states), -ERR_INVALID_ARGUMENT);
}

int main(void)
{
    test_sha256();
    test_text();
    test_image();
    test_check();
    free(g_image);
    printf("recipe: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
