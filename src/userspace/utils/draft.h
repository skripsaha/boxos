#ifndef DRAFT_H
#define DRAFT_H

/* draft — what the editor is made of, and why it comes in parts.
 *
 * A draft on the glass is four jobs wearing one name: a BOOK of lines that
 * grow and split, a PAINTER that turns that book into one frame of cells, a
 * VOLUME side that resolves a name and puts the bytes back, and the WORD LINE
 * and keys that decide which of the three is asked. Each of those reads the
 * same handful of state — where the caret is, how big the screen is, what the
 * foot is saying — so the state is defined once in draft.c and named here,
 * and each part exports only what another part actually calls. What a part
 * keeps to itself stays static inside it and is not in this header.
 */

#include "box/vga.h"

/* Shared by more than one part: the name buffer's size, and the size of every
 * line the editor says. */
#define DRAFT_NAME_MAX    31      /* create() refuses a name of 32 bytes or more */

/* One screen row is at most 255 cells, so a word line longer than this could
 * not be shown even if it could be typed: the ceiling is the glass, not a
 * guess about how much anyone means to say. */
#define DRAFT_SAY_MAX     256

typedef struct DraftLine {
    char    *text;
    uint32_t len;
    uint32_t cap;
} DraftLine;

typedef struct DraftBook {
    DraftLine *lines;
    uint32_t   count;
    uint32_t   cap;
} DraftBook;

/* The state every part reads. Defined in draft.c. */
extern DraftBook g_book;
extern uint32_t  g_fid;        /* 0 until the volume has this draft */
extern char     *g_tags;       /* comma list create() stamps, NULL when none */
extern uint32_t  g_line;       /* the cursor's line */
extern uint32_t  g_byte;       /* the cursor's byte inside that line */
extern uint32_t  g_want;       /* display column Up and Down aim at */
extern uint32_t  g_saves;
extern int       g_dirty;
extern int       g_cmd_open;
extern uint32_t  g_cmd_len;
extern uint32_t  g_rows;
extern uint32_t  g_cols;
extern TextCell *g_frame;

extern char s_name[DRAFT_NAME_MAX + 1];
extern char s_tagline[80];             /* the tags as the title bar says them */
extern char s_msg[DRAFT_SAY_MAX];
extern char s_cmd[DRAFT_SAY_MAX];

/* draft_book.c — saying things into a buffer */
void say_str(char *buf, size_t cap, const char *s);
void say_uint(char *buf, size_t cap, uint32_t v);
void say_err(char *buf, size_t cap, int rc);
void msg_begin(void);
void msg_add(const char *s);
void msg_num(uint32_t v);

/* draft_book.c — the book */
int       book_load(const char *bytes, uint32_t len);
uint32_t  book_bytes(void);
char     *book_join(uint32_t *out_len);

/* draft_book.c — geometry */
uint32_t col_after(uint32_t col, char c);
uint32_t width_of(const DraftLine *l, uint32_t nbytes);
uint32_t text_rows(void);

/* draft_book.c — editing */
void mark_column(void);
void no_memory(const char *what);
void insert_byte(char c);
void split_line(void);
void backspace(void);
void delete_forward(void);
void move_left(void);
void move_right(void);
void move_line(int down);
void move_page(int down);

/* draft_paint.c */
void paint(void);

/* draft_file.c */
void  tagline_from(uint32_t fid);
char *join_with_commas(const char *const *words, uint32_t count);
int   matches_by_name(const char *name, const char *tags, uint32_t **out_ids);
int   load_content(void);
void  save_book(void);

#endif /* DRAFT_H */
