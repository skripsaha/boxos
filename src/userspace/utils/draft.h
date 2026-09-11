#ifndef DRAFT_H
#define DRAFT_H


#include "box/vga.h"

#define DRAFT_NAME_MAX    31

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

extern DraftBook g_book;
extern uint32_t  g_fid;
extern char     *g_tags;
extern uint32_t  g_line;
extern uint32_t  g_byte;
extern uint32_t  g_want;
extern uint32_t  g_saves;
extern int       g_dirty;
extern int       g_cmd_open;
extern uint32_t  g_cmd_len;
extern uint32_t  g_rows;
extern uint32_t  g_cols;
extern TextCell *g_frame;

extern char s_name[DRAFT_NAME_MAX + 1];
extern char s_tagline[80];
extern char s_msg[DRAFT_SAY_MAX];
extern char s_cmd[DRAFT_SAY_MAX];

void say_str(char *buf, size_t cap, const char *s);
void say_uint(char *buf, size_t cap, uint32_t v);
void say_err(char *buf, size_t cap, int rc);
void msg_begin(void);
void msg_add(const char *s);
void msg_num(uint32_t v);

int       book_load(const char *bytes, uint32_t len);
uint32_t  book_bytes(void);
char     *book_join(uint32_t *out_len);

uint32_t col_after(uint32_t col, char c);
uint32_t width_of(const DraftLine *l, uint32_t nbytes);
uint32_t text_rows(void);

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

void paint(void);

void  tagline_from(uint32_t fid);
char *join_with_commas(const char *const *words, uint32_t count);
int   matches_by_name(const char *name, const char *tags, uint32_t **out_ids);
int   load_content(void);
void  save_book(void);

#endif