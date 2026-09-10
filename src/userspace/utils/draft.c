/* draft — a text still in work, held on the glass and put back byte for byte.
 *
 * A volume full of tagged files and no way to change one of them by hand is a
 * library with no pen. This is the pen. It takes the whole screen as a grid of
 * cells and lays every frame down in one blit, and it hears every key as an
 * event, so a machine with an editor open on it spends no core on waiting. It
 * is the weight of a note pad rather than of a workshop — two chords and a
 * word line — because the hands that reach for it are on a bench beside the
 * board, and a chord nobody can remember is a chord nobody uses.
 *
 *   draft NAME           -> that file, or an empty page the first save creates
 *   draft NAME TAG...    -> the tags narrow which NAME is meant when several
 *                           answer, and are stamped on a file that is created
 *   ^S / ^Q              -> save / leave
 *   ESC then a word      -> save, quit, quit!, name, tag, untag, find, go, help
 *
 * Two things here must not be undone. The round trip is a rule: N newlines
 * become N+1 lines, so "a\nb\n" holds a trailing empty line and joining with
 * '\n' writes the same bytes back — draft never adds or removes a byte the
 * person did not type, and a byte it cannot draw is drawn '?' and KEPT, never
 * rewritten to match the glass. And nothing may printf between the first paint
 * and the last clear: printed text travels the display daemon's lane and lands
 * on top of the frame at a moment nobody chose.
 *
 * This file is the one that decides. It holds the state the other three parts
 * read (draft.h names it), takes the keys and the word line, and opens and
 * closes the whole thing: the book is in draft_book.c, the glass in
 * draft_paint.c, the volume in draft_file.c.
 */

#include "box/print.h"
#include "box/vga.h"
#include "box/color.h"
#include "box/keyboard.h"
#include "box/touch.h"
#include "box/system.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/file.h"
#include "box/error.h"
#include "box/luggage.h"

#include "draft.h"

DraftBook g_book;
uint32_t  g_fid;        /* 0 until the volume has this draft */
char     *g_tags;       /* comma list create() stamps, NULL when none */
uint32_t  g_line;       /* the cursor's line */
uint32_t  g_byte;       /* the cursor's byte inside that line */
uint32_t  g_want;       /* display column Up and Down aim at */
uint32_t  g_saves;
int       g_dirty;
static int g_leaving;
int       g_cmd_open;
uint32_t  g_cmd_len;
uint32_t  g_rows;
uint32_t  g_cols;
TextCell *g_frame;

char s_name[DRAFT_NAME_MAX + 1];
char s_tagline[80];             /* the tags as the title bar says them */
char s_msg[DRAFT_SAY_MAX];
char s_cmd[DRAFT_SAY_MAX];
static char s_find[DRAFT_SAY_MAX];     /* what `find` repeats */
static char s_work[DRAFT_SAY_MAX];     /* the command line, cut in place */

/* ---------------------------------------------------------------------------
 * The word line — everything that is not a chord.
 * ------------------------------------------------------------------------- */

static char *first_word(char *s)
{
    char *p = s;
    while (*p && *p != ' ' && *p != '\t') p++;
    *p = '\0';
    return s;
}

static void leave_or_refuse(void)
{
    if (!g_dirty) { g_leaving = 1; return; }
    msg_begin();
    msg_add("unsaved changes - ^S to save, or ESC then  quit!");
}

static void cmd_save(char *rest)
{
    if (!*rest) { save_book(); return; }

    /* A line of length n holds at most (n+1)/2 words — one byte each and a
     * blank between — so the cut is sized from what was typed, not guessed. */
    uint32_t room  = (uint32_t)(strlen(rest) + 1) / 2 + 1;
    char   **words = malloc(room * sizeof(char *));
    if (!words) { no_memory("that line's words"); return; }

    uint32_t count = luggage_cut(rest, words, room);
    if (count == 0) { free(words); save_book(); return; }

    if (strlen(words[0]) > DRAFT_NAME_MAX) {
        msg_begin();
        msg_add("that name is longer than a file's name may be");
        free(words);
        return;
    }

    char *tags = NULL;
    if (count > 1) {
        tags = join_with_commas((const char *const *)words + 1, count - 1);
        if (!tags) { no_memory("that tag list"); free(words); return; }
    }

    s_name[0] = '\0';
    say_str(s_name, sizeof(s_name), words[0]);
    free(words);

    /* A new name is a new file, stamped with the words that came with it; the
     * editor edits that one from here on, and the old one keeps its own last
     * saved bytes. */
    free(g_tags);
    g_tags = tags;
    g_fid  = 0;
    tagline_from(0);
    save_book();
}

static void cmd_name(char *rest)
{
    char *fresh = first_word(rest);
    if (!*fresh) { msg_begin(); msg_add("name what? say  name <newname>"); return; }
    if (strlen(fresh) > DRAFT_NAME_MAX) {
        msg_begin();
        msg_add("that name is longer than a file's name may be");
        return;
    }
    if (g_fid == 0) {
        msg_begin();
        msg_add("this draft is not on the volume yet - save it first");
        return;
    }

    int rc = file_rename(g_fid, fresh);
    if (rc != 0) {
        msg_begin();
        msg_add("the volume would not rename it");
        say_err(s_msg, sizeof(s_msg), rc);
        return;
    }
    s_name[0] = '\0';
    say_str(s_name, sizeof(s_name), fresh);
    tagline_from(g_fid);
    msg_begin();
    msg_add("now called ");
    msg_add(s_name);
}

static void cmd_tag(char *rest, int adding)
{
    char *word = first_word(rest);
    if (!*word) {
        msg_begin();
        msg_add(adding ? "tag what? say  tag <key:value>" : "untag what? say  untag <key>");
        return;
    }
    if (g_fid == 0) {
        msg_begin();
        msg_add("this draft is not on the volume yet - save it first");
        return;
    }

    int rc = adding ? tag_add(g_fid, word) : tag_remove(g_fid, word);
    if (rc != 0) {
        msg_begin();
        msg_add(adding ? "that tag would not go on" : "that tag would not come off");
        say_err(s_msg, sizeof(s_msg), rc);
        return;
    }
    tagline_from(g_fid);
    msg_begin();
    msg_add(adding ? "tagged " : "untagged ");
    msg_add(word);
}

static int line_holds(const DraftLine *l, uint32_t from, const char *needle,
                      uint32_t nlen, uint32_t *out)
{
    if (nlen == 0 || nlen > l->len) return 0;
    for (uint32_t i = from; i + nlen <= l->len; i++) {
        if (memcmp(l->text + i, needle, nlen) == 0) { *out = i; return 1; }
    }
    return 0;
}

/* Forward from just after the cursor, wrapping once. The second pass searches
 * the starting line from its head, so a match behind the cursor on the same
 * line is found last rather than never. */
static void cmd_find(char *rest)
{
    if (*rest) {
        s_find[0] = '\0';
        say_str(s_find, sizeof(s_find), rest);
    }
    if (!s_find[0]) { msg_begin(); msg_add("find what? say  find <text>"); return; }

    uint32_t nlen  = (uint32_t)strlen(s_find);
    uint32_t start = g_line;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t first = (pass == 0) ? start : 0;
        uint32_t last  = (pass == 0) ? g_book.count - 1 : start;
        for (uint32_t li = first; li <= last; li++) {
            uint32_t from = (pass == 0 && li == start) ? g_byte + 1 : 0;
            uint32_t at   = 0;
            if (!line_holds(&g_book.lines[li], from, s_find, nlen, &at)) continue;
            g_line = li;
            g_byte = at;
            mark_column();
            msg_begin();
            msg_add("found on line ");
            msg_num(li + 1);
            return;
        }
    }
    msg_begin();
    msg_add("no  ");
    msg_add(s_find);
    msg_add("  in this draft");
}

static void cmd_go(char *rest)
{
    char *word = first_word(rest);
    if (!is_number(word)) { msg_begin(); msg_add("go where? say  go <n>"); return; }

    int n = to_int(word);
    if (n < 1) n = 1;
    if ((uint32_t)n > g_book.count) n = (int)g_book.count;
    g_line = (uint32_t)n - 1;
    g_byte = 0;
    g_want = 0;
}

static void run_command(void)
{
    s_work[0] = '\0';
    say_str(s_work, sizeof(s_work), s_cmd);

    char *p = s_work;
    while (*p == ' ' || *p == '\t') p++;
    char *word = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    char *rest = p;
    if (*rest) {
        *rest++ = '\0';
        while (*rest == ' ' || *rest == '\t') rest++;
    }

    if (!*word)                       return;   /* ESC then Enter: nothing said */
    if (strcmp(word, "save")  == 0) { cmd_save(rest); return; }
    if (strcmp(word, "quit")  == 0) { leave_or_refuse(); return; }
    if (strcmp(word, "quit!") == 0) { g_leaving = 1; return; }
    if (strcmp(word, "name")  == 0) { cmd_name(rest); return; }
    if (strcmp(word, "tag")   == 0) { cmd_tag(rest, 1); return; }
    if (strcmp(word, "untag") == 0) { cmd_tag(rest, 0); return; }
    if (strcmp(word, "find")  == 0) { cmd_find(rest); return; }
    if (strcmp(word, "go")    == 0) { cmd_go(rest); return; }
    if (strcmp(word, "help")  == 0) {
        msg_begin();
        msg_add("save   save <name> [tag..]   quit   quit!   name <newname>   "
                "tag <key:value>   untag <key>   find <text>   go <n>   help");
        return;
    }
    msg_begin();
    msg_add("draft: no such word - try  help");
}

/* ---------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

static void handle_extended(uint8_t scancode)
{
    switch (scancode) {
    case KEY_LEFT:   move_left();   break;
    case KEY_RIGHT:  move_right();  break;
    case KEY_UP:     move_line(0);  break;
    case KEY_DOWN:   move_line(1);  break;
    case KEY_PAGEUP: move_page(0);  break;
    case KEY_PAGEDN: move_page(1);  break;
    case KEY_DELETE: delete_forward(); break;
    case KEY_HOME:   g_byte = 0; g_want = 0; break;
    case KEY_END:    g_byte = g_book.lines[g_line].len; mark_column(); break;
    default: break;
    }
}

static void handle_edit_key(const kb_event_t *k)
{
    if (k->mods & KB_MOD_CTRL) {
        if (k->ascii == 's' || k->ascii == 'S') save_book();
        else if (k->ascii == 'q' || k->ascii == 'Q') leave_or_refuse();
        return;         /* every other chord is deliberately not a command */
    }
    if (k->mods & KB_MOD_EXTENDED) { handle_extended(k->scancode); return; }

    unsigned char c = (unsigned char)k->ascii;
    if (c == KEY_ESCAPE) {
        g_cmd_open = 1;
        g_cmd_len  = 0;
        s_cmd[0]   = '\0';
        return;
    }
    /* '\r' as well as '\n': a serial line sends the carriage return, and the
     * house line editor has always taken both for Enter. */
    if (c == '\n' || c == '\r')                { split_line(); return; }
    if (c == KEY_BACKSPACE || c == 0x7F)       { backspace();  return; }
    if (c == '\t' || (c >= 0x20 && c < 0x7F))  insert_byte((char)c);
}

static void handle_command_key(const kb_event_t *k)
{
    if (k->mods & (KB_MOD_CTRL | KB_MOD_EXTENDED)) return;

    unsigned char c = (unsigned char)k->ascii;
    if (c == KEY_ESCAPE)                  { g_cmd_open = 0; return; }
    if (c == '\n' || c == '\r')           { g_cmd_open = 0; run_command(); return; }
    if (c == KEY_BACKSPACE || c == 0x7F) {
        if (g_cmd_len) s_cmd[--g_cmd_len] = '\0';
        return;
    }
    if (c >= 0x20 && c < 0x7F && g_cmd_len + 1 < sizeof(s_cmd)) {
        s_cmd[g_cmd_len++] = (char)c;
        s_cmd[g_cmd_len]   = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * Opening, running, leaving.
 * ------------------------------------------------------------------------- */

/* Everything that must be settled before the screen is taken. Returns NULL
 * when the draft is open, or the sentence to say on the way out — a refusal
 * has to be printed while printing is still allowed, and once the frame is on
 * the glass it no longer is. */
static const char *open_the_draft(int argc)
{
    const char *name = luggage_word(1);
    if (strlen(name) > DRAFT_NAME_MAX) return "That name is longer than a file's name may be.";
    say_str(s_name, sizeof(s_name), name);

    if (argc > 2) {
        const char **words = malloc((size_t)(argc - 2) * sizeof(char *));
        if (!words) return "There is no memory for the tag list.";
        for (int i = 2; i < argc; i++) words[i - 2] = luggage_word((uint32_t)i);
        g_tags = join_with_commas(words, (uint32_t)(argc - 2));
        free(words);
        if (!g_tags) return "There is no memory for the tag list.";
    }

    uint32_t *ids = NULL;
    int       n   = matches_by_name(s_name, g_tags, &ids);
    if (n < 0) return "The volume could not be asked which files are called that.";
    if (n > 1) {
        println("Several files are called that:");
        for (int i = 0; i < n; i++) {
            file_info_t info;
            if (file_info(ids[i], &info) == 0)
                printf("  %s [file_id=%u]\n", info.filename, ids[i]);
        }
        free(ids);
        return "Name a tag that tells them apart, after the filename.";
    }
    g_fid = (n == 1) ? ids[0] : 0;
    free(ids);

    if (!(g_fid ? load_content() : book_load("", 0)))
        return "That draft could not be read into memory.";
    return NULL;
}

static const char *take_the_screen(TouchTag *ear)
{
    vga_dimensions_t dim;
    if (vga_getdimensions(&dim) != 0) return "This screen would not say how big it is.";
    /* A zero means there is no console at all. It used to mean something else
     * as well — the geometry travels in two bytes and the kernel CAST a wider
     * screen into them, so 320 columns arrived as 64 and only an exact
     * multiple of 2048 pixels wrapped to zero. The kernel clamps now: a screen
     * with more columns than can be named answers 255, and a draft is written
     * on the part of it this ABI can address. */
    if (dim.rows == 0 || dim.cols == 0)
        return "This machine has no console to write a draft on.";
    if (dim.rows < 3) return "This screen has too few rows for a title, a text and a foot.";
    g_rows = dim.rows;
    g_cols = dim.cols;

    g_frame = malloc((size_t)g_rows * g_cols * sizeof(TextCell));
    if (!g_frame) return "There is no memory for a frame of this screen.";

    *ear = console_listen();
    if (*ear == TOUCH_TAG_INVALID) return "This machine has no way to hear a key.";
    return NULL;
}

static void run_the_editor(TouchTag ear)
{
    while (!g_leaving) {
        paint();

        /* No deadline: an editor has nothing to do between keys, and the
         * kernel parks the whole cabin until one arrives.
         *
         * A refusal is therefore not "not yet", it is "there is nothing left
         * to hear" — the ear has been taken or the ring is gone. Looping on it
         * would repaint the screen as fast as the machine can, forever, with
         * no key able to stop it: a full-screen program nobody can leave. */
        Touch t;
        if (touch_await(ear, &t, 0) != 0) break;
        if (t.payload_len < sizeof(kb_event_t)) continue;

        kb_event_t k;
        memcpy(&k, t.payload, sizeof(k));
        s_msg[0] = '\0';        /* a message stands until the next key */
        if (g_cmd_open) handle_command_key(&k);
        else            handle_edit_key(&k);
    }
}

int main(void)
{
    int argc = (int)luggage_word_count();
    if (argc < 2) { println("Usage: draft <filename> [tag...]"); exit(1); return 1; }

    const char *refusal = open_the_draft(argc);
    if (refusal) { println(refusal); exit(1); return 1; }

    TouchTag ear = TOUCH_TAG_INVALID;
    refusal = take_the_screen(&ear);
    if (refusal) { println(refusal); exit(1); return 1; }

    tagline_from(g_fid);
    run_the_editor(ear);

    console_unlisten();
    (void)vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    (void)vga_setcursor(0, 0);

    if (g_saves == 0) printf("[DRAFT] %s - left unsaved\n", s_name);
    else printf("[DRAFT] %s - %u lines, %u bytes, saved %u time%s\n",
                s_name, g_book.count, book_bytes(), g_saves, g_saves == 1 ? "" : "s");

    exit(0);
    return 0;
}
