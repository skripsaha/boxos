#ifndef COMMANDS_H
#define COMMANDS_H

int cmd_use(int argc, char *argv[]);
int cmd_exit(int argc, char *argv[]);
int cmd_clear(int argc, char *argv[]);
int cmd_help(int argc, char *argv[]);

/* Built in rather than spawned, because the failure it exists to record is one
 * where nothing on the volume can be spawned at all. See cmd_said.c. */
int cmd_said(int argc, char *argv[]);

#endif /* COMMANDS_H */
