#include "system_deck_defrag.h"
#include "system_deck_process.h"
#include "klib.h"
#include "../../tagfs/tagfs.h"

int system_deck_defrag_file(Event* event) {
    if (!event) {
        return -1;
    }

    defrag_file_event_t* req = (defrag_file_event_t*)event->data;
    defrag_file_response_t* resp = (defrag_file_response_t*)event->data;

    uint32_t file_id = req->file_id;
    uint32_t target_block = req->target_block;

    memset(event->data, 0, EVENT_DATA_SIZE);

    int result = tagfs_defrag_file(file_id, target_block);

    if (result == 0) {
        resp->error_code = SYSTEM_ERR_SUCCESS;
        resp->fragmentation_score = tagfs_get_fragmentation_score();
        event->state = EVENT_STATE_COMPLETED;
        return 0;
    } else {
        resp->error_code = SYSTEM_ERR_INVALID_ARGS;
        resp->fragmentation_score = tagfs_get_fragmentation_score();
        event->state = EVENT_STATE_ERROR;
        return -1;
    }
}

int system_deck_fragmentation_score(Event* event) {
    if (!event) {
        return -1;
    }

    fragmentation_score_response_t* resp = (fragmentation_score_response_t*)event->data;
    memset(event->data, 0, EVENT_DATA_SIZE);

    uint32_t score = tagfs_get_fragmentation_score();

    TagFSState* state = tagfs_get_state();
    uint32_t total_files = 0;
    uint32_t total_gaps = 0;

    if (state && state->initialized) {
        for (uint32_t i = 0; i < TAGFS_MAX_FILES; i++) {
            TagFSMetadata* meta = tagfs_get_metadata(i + 1);
            if (meta && (meta->flags & TAGFS_FILE_ACTIVE) && meta->block_count > 0) {
                total_files++;
            }
        }
    }

    resp->score = score;
    resp->total_files = total_files;
    resp->total_gaps = total_gaps;

    event->state = EVENT_STATE_COMPLETED;
    return 0;
}
