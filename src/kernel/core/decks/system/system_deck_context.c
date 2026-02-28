#include "system_deck_context.h"
#include "scheduler.h"
#include "klib.h"

static int ctx_use_parse_tags(const char* input, char tags[][CTX_TAG_LENGTH], uint32_t* tag_count) {
    if (!input || !tags || !tag_count) {
        return CTX_USE_ERR_INVALID_FORMAT;
    }

    *tag_count = 0;

    if (input[0] == '\0') {
        return CTX_USE_ERR_SUCCESS;
    }

    char buffer[EVENT_DATA_SIZE];
    strncpy(buffer, input, EVENT_DATA_SIZE - 1);
    buffer[EVENT_DATA_SIZE - 1] = '\0';

    char* saveptr = NULL;
    char* token = strtok_r(buffer, ",", &saveptr);

    while (token != NULL) {
        while (*token == ' ' || *token == '\t') {
            token++;
        }

        char* end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        if (*token == '\0') {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        if (*tag_count >= MAX_CTX_USE_TAGS) {
            return CTX_USE_ERR_TOO_MANY_TAGS;
        }

        size_t tag_len = strlen(token);
        if (tag_len >= CTX_TAG_LENGTH) {
            return CTX_USE_ERR_TAG_TOO_LONG;
        }

        strncpy(tags[*tag_count], token, CTX_TAG_LENGTH - 1);
        tags[*tag_count][CTX_TAG_LENGTH - 1] = '\0';
        (*tag_count)++;

        token = strtok_r(NULL, ",", &saveptr);
    }

    return CTX_USE_ERR_SUCCESS;
}

int system_deck_ctx_use(Event* event) {
    if (!event) {
        return -1;
    }

    ctx_use_event_t* req = (ctx_use_event_t*)event->data;
    ctx_use_response_t* resp = (ctx_use_response_t*)event->data;

    char parsed_tags[MAX_CTX_USE_TAGS][CTX_TAG_LENGTH];
    uint32_t tag_count = 0;

    memset(parsed_tags, 0, sizeof(parsed_tags));

    int parse_result = ctx_use_parse_tags(req->context_string, parsed_tags, &tag_count);

    if (parse_result != CTX_USE_ERR_SUCCESS) {
        memset(event->data, 0, EVENT_DATA_SIZE);
        resp->error_code = parse_result;

        switch (parse_result) {
            case CTX_USE_ERR_TOO_MANY_TAGS:
                strncpy(resp->message, "Too many tags (max 3)", sizeof(resp->message) - 1);
                break;
            case CTX_USE_ERR_TAG_TOO_LONG:
                strncpy(resp->message, "Tag too long (max 63 bytes)", sizeof(resp->message) - 1);
                break;
            case CTX_USE_ERR_INVALID_FORMAT:
            default:
                strncpy(resp->message, "Invalid format", sizeof(resp->message) - 1);
                break;
        }
        resp->message[sizeof(resp->message) - 1] = '\0';

        event->state = EVENT_STATE_ERROR;
        debug_printf("[CTX_USE] Parse error: %s\n", resp->message);
        return -1;
    }

    if (tag_count == 0) {
        scheduler_clear_use_context();
        memset(event->data, 0, EVENT_DATA_SIZE);
        resp->error_code = CTX_USE_ERR_SUCCESS;
        strncpy(resp->message, "Context cleared", sizeof(resp->message) - 1);
        resp->message[sizeof(resp->message) - 1] = '\0';
        event->state = EVENT_STATE_COMPLETED;
        debug_printf("[CTX_USE] Context cleared\n");
        return 0;
    }

    const char* tag_ptrs[MAX_CTX_USE_TAGS];
    for (uint32_t i = 0; i < tag_count; i++) {
        tag_ptrs[i] = parsed_tags[i];
    }

    scheduler_set_use_context(tag_ptrs, tag_count);

    memset(event->data, 0, EVENT_DATA_SIZE);
    resp->error_code = CTX_USE_ERR_SUCCESS;

    char context_desc[128];
    size_t offset = 0;
    strncpy(context_desc, "Context set: ", sizeof(context_desc) - 1);
    offset = strlen(context_desc);

    for (uint32_t i = 0; i < tag_count && offset < sizeof(context_desc) - 1; i++) {
        if (i > 0 && offset < sizeof(context_desc) - 2) {
            context_desc[offset++] = ',';
            context_desc[offset++] = ' ';
        }
        size_t remaining = sizeof(context_desc) - offset - 1;
        strncpy(context_desc + offset, parsed_tags[i], remaining);
        offset += strlen(parsed_tags[i]);
        if (offset >= sizeof(context_desc) - 1) {
            offset = sizeof(context_desc) - 1;
            break;
        }
    }
    context_desc[offset] = '\0';

    strncpy(resp->message, context_desc, sizeof(resp->message) - 1);
    resp->message[sizeof(resp->message) - 1] = '\0';

    event->state = EVENT_STATE_COMPLETED;
    debug_printf("[CTX_USE] %s\n", resp->message);

    return 0;
}
