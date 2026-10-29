/*
 * form_line.c — the words of a line, cut the way Luggage cuts them, and told
 * where each stood.
 *
 * Blanks separate and "double quotes" group, as in luggage.c. On top of that
 * a comma outside quotes ends a word and joins it to the next: `a, b` and
 * `a ,b` and `a,b` are the same list. Each word keeps where it stood in the
 * line as typed, so the line can be given back without the arrow.
 */

#include "box/core/form_internal.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int add_token(FormTokens *tokens, size_t *capacity, const char *text, size_t length,
                     uint32_t start, uint32_t end, uint8_t quoted, uint8_t comma_before)
{
    if (tokens->Count == *capacity) {
        size_t     grown = *capacity ? *capacity * 2 : 8;
        FormToken *bigger = realloc(tokens->Tokens, grown * sizeof(FormToken));
        if (!bigger) return -ERR_NO_MEMORY;
        tokens->Tokens = bigger;
        *capacity      = grown;
    }
    char *copy = malloc(length + 1);
    if (!copy) return -ERR_NO_MEMORY;
    memcpy(copy, text, length);
    copy[length] = '\0';

    FormToken *token = &tokens->Tokens[tokens->Count++];
    token->Text        = copy;
    token->Start       = start;
    token->End         = end;
    token->Quoted      = quoted;
    token->CommaBefore = comma_before;
    token->CommaAfter  = 0;
    return 0;
}

/* One word with no quotes: its pieces between commas, from `at` to a blank. */
static int cut_bare(const char *line, uint32_t *at, FormTokens *tokens, size_t *capacity,
                    bool *comma_pending)
{
    uint32_t end = *at;
    while (line[end] != '\0' && !is_blank(line[end])) end++;

    uint32_t piece = *at;
    for (uint32_t i = *at; i <= end; i++) {
        bool comma = i < end && line[i] == ',';
        if (i < end && !comma) continue;

        if (i > piece) {
            int rc = add_token(tokens, capacity, line + piece, i - piece, piece, i, 0, *comma_pending);
            if (rc != 0) return rc;
            *comma_pending = false;
        }
        if (comma) {
            if (tokens->Count > 0) tokens->Tokens[tokens->Count - 1].CommaAfter = 1;
            *comma_pending = true;
        }
        piece = i + 1;
    }
    *at = end;
    return 0;
}

static int cut_quoted(const char *line, uint32_t *at, FormTokens *tokens, size_t *capacity,
                      bool *comma_pending)
{
    uint32_t start = *at;
    uint32_t text  = start + 1;
    uint32_t end   = text;
    while (line[end] != '\0' && line[end] != '"') end++;

    uint32_t stop = line[end] == '"' ? end + 1 : end;
    int rc = add_token(tokens, capacity, line + text, end - text, start, stop, 1, *comma_pending);
    *comma_pending = false;
    *at = stop;
    return rc;
}

int form_line_cut(const char *line, FormTokens *out)
{
    size_t capacity = 0;
    bool   comma_pending = false;
    uint32_t at = 0;

    out->Tokens = NULL;
    out->Count  = 0;
    for (;;) {
        while (is_blank(line[at])) at++;
        if (line[at] == '\0') break;

        int rc = line[at] == '"'
                     ? cut_quoted(line, &at, out, &capacity, &comma_pending)
                     : cut_bare(line, &at, out, &capacity, &comma_pending);
        if (rc != 0) {
            form_line_release(out);
            return rc;
        }
    }
    return 0;
}

void form_line_release(FormTokens *tokens)
{
    for (size_t i = 0; i < tokens->Count; i++) free(tokens->Tokens[i].Text);
    free(tokens->Tokens);
    tokens->Tokens = NULL;
    tokens->Count  = 0;
}
