
#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "box/print.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/core/result.h"
#include "box/system.h"
#include "box/use.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"

static ShellState    g_state;



void ShellInit(void)
{
    memset(&g_state, 0, sizeof(ShellState));
    g_state.running = true;
    memcpy(g_state.prompt, "~ ", 3);


    CabinInfo *ci = cabin_info();

    io_set_mode(IO_MODE_IPC);

    if (ci->spawner_pid != 0) {
        ShellDrainStaleIpc();
    }

    ShellUpdatePrompt();

    clear();
    println("BoxOS Shell v2.0");
    println("Type 'help' for commands, 'exit' to quit.");
    println("");
}


void ShellUpdatePrompt(void)
{
    char list[SHELL_PROMPT_MAX - 5];
    int  tags = use_get(list, sizeof(list), NULL);

    if (tags > 0) {
        size_t len = strlen(list);
        g_state.prompt[0] = '[';
        memcpy(g_state.prompt + 1, list, len);
        memcpy(g_state.prompt + 1 + len, "] ~ ", 5);
    } else {
        memcpy(g_state.prompt, "~ ", 3);
    }
}


void ShellDrainStaleIpc(void)
{
    Result drain;
    while (receive(&drain)) { }
}

void ShellMainLoop(void)
{
    char input[SHELL_LINE_MAX];

    while (g_state.running) {
        print(g_state.prompt);
        io_flush();

        int len = readline(input, SHELL_LINE_MAX);
        if (len <= 0)
            continue;

        ParsedCommand cmd;
        if (ParserParse(input, &cmd) != 0) {
            println("Error: failed to parse command");
            continue;
        }

        if (cmd.argc == 0)
            continue;

        int result = ExecutorRun(&cmd, input);
        if (result != 0) {
            const char *err = ExecutorGetError();
            if (err && err[0] != '\0') {
                printf("%colorError: %s%color\n",
                       COLOR_RED, err, COLOR_DEFAULT);
            }
        }
    }
}

void ShellStop(void)
{
    g_state.running = false;
}

ShellState *ShellGetState(void)
{
    return &g_state;
}


int main(void)
{
    ShellInit();
    ShellMainLoop();
    return 0;
}