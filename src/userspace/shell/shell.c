#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "box/io.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/system.h"
#include "box/notify.h"

static shell_state_t g_shell_state;

void shell_init(void)
{
    memset(&g_shell_state, 0, sizeof(shell_state_t));
    g_shell_state.running = true;
    memcpy(g_shell_state.prompt, "~ ", 3);

    CabinInfo *ci = cabin_info();

    if (ci->spawner_pid == 0)
    {
        // Root shell (autostart): create display process
        int display_pid = proc_exec("display");
        if (display_pid > 0)
        {
            Result entry;
            if (receive_wait(&entry, 2000))
            {
                io_set_mode(IO_MODE_IPC);
            }
        }
    }
    else
    {
        // Nested shell: display already exists, reuse it via IPC
        io_set_mode(IO_MODE_IPC);
    }

    clear();
    println("BoxOS Shell v1.0");
    println("Type 'help' for available commands");
    println("");
}

void shell_update_prompt(void)
{
    if (g_shell_state.context_tag_count > 0)
    {
        char ctx_str[SHELL_PROMPT_SIZE] = "[";
        size_t pos = 1;

        for (uint32_t i = 0; i < g_shell_state.context_tag_count && pos < SHELL_PROMPT_SIZE - 4; i++)
        {
            size_t tag_len = strlen(g_shell_state.context_tags[i]);
            if (pos + tag_len + 2 < SHELL_PROMPT_SIZE - 4)
            {
                memcpy(ctx_str + pos, g_shell_state.context_tags[i], tag_len);
                pos += tag_len;

                if (i < g_shell_state.context_tag_count - 1)
                {
                    ctx_str[pos++] = ',';
                }
            }
        }

        ctx_str[pos++] = ']';
        ctx_str[pos++] = ' ';
        ctx_str[pos++] = '~';
        ctx_str[pos++] = ' ';
        ctx_str[pos] = '\0';

        memcpy(g_shell_state.prompt, ctx_str, pos + 1);
    }
    else
    {
        memcpy(g_shell_state.prompt, "~ ", 3);
    }
}

void shell_print_prompt()
{
    print(g_shell_state.prompt);
    io_flush();
}

void shell_main_loop(void)
{
    shell_print_prompt();

    while (g_shell_state.running)
    {
        memset(g_shell_state.input_buffer, 0, SHELL_MAX_INPUT);

        int len = readline(g_shell_state.input_buffer, SHELL_MAX_INPUT);
        if (len < 0) {
            shell_print_prompt();
            continue;
        }
        if (len == 0) {
            shell_print_prompt();
            continue;
        }

        parsed_command_t cmd;
        if (parser_parse(g_shell_state.input_buffer, &cmd) != 0)
        {
            println("Error: Failed to parse command");
            shell_print_prompt();
            continue;
        }

        if (cmd.argc == 0)
        {
            shell_print_prompt();
            continue;
        }

        int result = executor_run(&cmd);
        if (result != 0)
        {
            const char *error = executor_get_error();
            if (error && error[0] != '\0')
            {
                printf("Error: %s\n", error);
            }
        }

        shell_print_prompt();
    }
}

void shell_stop(void)
{
    g_shell_state.running = false;
}

shell_state_t *shell_get_state(void)
{
    return &g_shell_state;
}

int main(void)
{
    shell_init();
    shell_main_loop();
    return 0;
}
