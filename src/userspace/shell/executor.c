/*
 * executor.c — BoxOS shell command executor
 *
 * Built-in commands are looked up in g_commands[].
 * External commands are spawned via proc_exec_gen(); args ship over IPC and the
 * child's exit is observed on the kernel's process:died Touch event.
 */

#include "executor.h"
#include "shell.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/touch.h"
#include "box/debug.h"
#include "box/core/result.h"
#include "box/print.h"

static char g_error[SHELL_ERROR_MAX];

/* =========================================================================
 * Command table — all built-in commands registered here
 * ========================================================================= */

const ShellCommand g_commands[] = {
    {"help",  cmd_help,  "help",       "Show available commands"},
    {"use",   cmd_use,   "use [tags]", "Set/clear context tags"},
    {"exit",  cmd_exit,  "exit",       "Exit shell (or Ctrl+Q)"},
    {"clear", cmd_clear, "clear",      "Clear screen"},
    {NULL, NULL, NULL, NULL}
};

/* =========================================================================
 * External command execution
 * ========================================================================= */

/* The shell's single standing process:died claim. Interned + claimed once
 * (lazily, on the first external command) and held for the shell's life:
 * process:died is a tag-multicast on the TouchRing — a separate ring from the
 * keyboard/args/display ResultRing — so this claim never disturbs input or IPC.
 * A child's exit (clean OR crash) is observed here, never via a send from the
 * child. */
static TouchTag g_pdied       = TOUCH_TAG_INVALID;
static bool     g_pdied_ready = false;

static void ensure_death_watch(void)
{
    if (g_pdied_ready) return;
    g_pdied = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    if (g_pdied != TOUCH_TAG_INVALID && touch_claim(g_pdied, TOUCH_REST, 0, 0) == OK)
        g_pdied_ready = true;
}

static int RunExternal(const char *name, ParsedCommand *cmd)
{
    /* Claim BEFORE the first spawn so a child that dies immediately can never
     * beat us to its own death event, then drop any stale/foreign deaths
     * banked on the TouchRing so the wait below only sees this child's. */
    ensure_death_watch();
    if (g_pdied_ready) {
        Touch drain;
        while (touch_try_pop_tag(g_pdied, &drain)) { }
    }

    uint32_t gen = 0;
    int pid = proc_exec_gen(name, NULL, &gen);
    if (pid <= 0) return -1;

    /* Build args + context tags into IPC buffer. SHELL_ARGS_BUF_MAX
     * (240 B) bounds the legacy send() payload; if the user's command
     * + tags exceeds that, we ship a partial argv to the child and
     * warn the user rather than silently corrupting their input. */
    ShellState *state = ShellGetState();
    char buf[SHELL_ARGS_BUF_MAX];
    int pos = 0;
    int args_sent = 0;

    buf[pos++] = (char)cmd->argc;
    for (int i = 0; i < cmd->argc; i++) {
        size_t len = strlen(cmd->argv[i]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX - 2) break;
        memcpy(buf + pos, cmd->argv[i], len);
        pos += (int)len;
        buf[pos++] = '\0';
        args_sent++;
    }
    /* Fix up the count byte so the child loops over what we actually
     * shipped, not what the user originally typed. */
    buf[0] = (char)args_sent;

    if (args_sent < cmd->argc) {
        printf("%colorWarning:%color shell IPC buffer full, sent %d/%d args\n",
               COLOR_YELLOW, COLOR_DEFAULT, args_sent, cmd->argc);
    }

    /* Append context tags */
    int tags_sent = 0;
    if (pos < SHELL_ARGS_BUF_MAX)
        buf[pos++] = (char)state->context_tag_count;
    for (uint32_t ci = 0; ci < state->context_tag_count; ci++) {
        size_t len = strlen(state->context_tags[ci]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX) break;
        memcpy(buf + pos, state->context_tags[ci], len);
        pos += (int)len;
        buf[pos++] = '\0';
        tags_sent++;
    }
    if (tags_sent < (int)state->context_tag_count) {
        printf("%colorWarning:%color sent %d/%u context tags\n",
               COLOR_YELLOW, COLOR_DEFAULT,
               tags_sent, state->context_tag_count);
    }

    send((uint32_t)pid, buf, (uint16_t)pos);

    if (!g_pdied_ready) {
        /* Cannot-happen: the registry rejected our claim. Rather than block on
         * a child we cannot observe, run it detached and stay responsive. */
        kdbg_print("[shell] process:died claim unavailable; running '%s' detached", name);
        ShellDrainStaleIpc();
        return 0;
    }

    /* Park forever on THIS child's death, matched by its canonical (pid,
     * generation) so a recycled pid's foreign death cannot wake us early.
     * process:died fires on clean exit AND crash, so there is no lost-death
     * case to poll around — an unfired death would be a kernel-substrate bug. */
    for (;;) {
        Touch t;
        if (!touch_wait_tag(g_pdied, &t, 0)) continue;
        if (t.payload_len < sizeof(TouchProcessDied)) continue;
        TouchProcessDied d;
        memcpy(&d, t.payload, sizeof d);
        if (d.pid == (uint32_t)pid && (gen == 0 || d.generation == gen)) break;
    }

    /* Drain stray ResultRing residue (display PING replies, stray broadcasts). */
    ShellDrainStaleIpc();

    return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int ExecutorRun(ParsedCommand *cmd)
{
    g_error[0] = '\0';

    /* Drain stale IPC before dispatching so a leftover child-exit
     * sentinel cannot be mis-routed to the new command. */
    ShellDrainStaleIpc();

    if (!cmd || cmd->argc == 0) {
        memcpy(g_error, "No command", 11);
        return -1;
    }

    const char *name = cmd->argv[0];

    /* Check built-in commands */
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(name, g_commands[i].name) == 0)
            return g_commands[i].handler(cmd->argc, cmd->argv);
    }

    /* Try external utility */
    if (RunExternal(name, cmd) == 0)
        return 0;

    /* Not found */
    size_t name_len = strlen(name);
    if (name_len > SHELL_ERROR_MAX - 20) name_len = SHELL_ERROR_MAX - 20;
    memcpy(g_error, "Unknown command: ", 17);
    memcpy(g_error + 17, name, name_len);
    g_error[17 + name_len] = '\0';
    return -1;
}

const char *ExecutorGetError(void)
{
    return g_error;
}
