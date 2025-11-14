#ifndef SHELL_H
#define SHELL_H

#include "ktypes.h"

// Shell configuration
#define SHELL_INPUT_BUFFER_SIZE 256
#define SHELL_MAX_ARGS 16
#define SHELL_PROMPT "boxos> "

// Shell command structure
typedef struct {
    const char* name;
    const char* description;
    int (*handler)(int argc, char** argv);
} shell_command_t;

// Shell initialization and main loop
void shell_init(void);
void shell_run(void);

// Command handlers
int cmd_help(int argc, char** argv);
int cmd_clear(int argc, char** argv);
int cmd_echo(int argc, char** argv);
int cmd_ls(int argc, char** argv);
int cmd_cat(int argc, char** argv);
int cmd_mkdir(int argc, char** argv);
int cmd_touch(int argc, char** argv);
int cmd_rm(int argc, char** argv);
int cmd_create(int argc, char** argv);
int cmd_eye(int argc, char** argv);
int cmd_use(int argc, char** argv);
int cmd_trash(int argc, char** argv);
int cmd_erase(int argc, char** argv);
int cmd_restore(int argc, char** argv);
int cmd_ps(int argc, char** argv);
int cmd_info(int argc, char** argv);
int cmd_reboot(int argc, char** argv);

#endif // SHELL_H
