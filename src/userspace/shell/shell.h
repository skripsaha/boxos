#ifndef SHELL_H
#define SHELL_H

#include "box/defs.h"

#define SHELL_MAX_INPUT 256
#define SHELL_MAX_ARGS 16
#define SHELL_PROMPT_SIZE 64
#define SHELL_MAX_CONTEXT_TAGS 8

// Shell state
typedef struct {
    char input_buffer[SHELL_MAX_INPUT];
    char prompt[SHELL_PROMPT_SIZE];
    bool running;
    uint32_t context_tag_count;
    char context_tags[SHELL_MAX_CONTEXT_TAGS][32];
} shell_state_t;

// Parsed command
typedef struct {
    int argc;
    char* argv[SHELL_MAX_ARGS];
    char arg_storage[SHELL_MAX_INPUT];
} parsed_command_t;

// Command handler function type
typedef int (*command_handler_t)(int argc, char* argv[]);

// Command definition
typedef struct {
    const char* name;
    command_handler_t handler;
    const char* usage;
    const char* description;
} shell_command_t;

// ============================================================================
// SHELL CORE FUNCTIONS
// ============================================================================

// Initialize shell
void shell_init(void);

// Main loop
void shell_main_loop(void);

// Stop shell
void shell_stop(void);

// Get shell state
shell_state_t* shell_get_state(void);

// Update prompt based on context
void shell_update_prompt(void);

#endif // SHELL_H
