#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Fixed lab limits; runtime settings are collected in one structure. */
#define MAX_UPLOAD_SIZE (10U * 1024U * 1024U)
#define MAX_HEADER_SIZE 16384U
#define MAX_TARGET_SIZE 2048U

struct server_config {
    int port;
    int timeout;
    int foreground;
    char root[PATH_MAX];
    char log_path[PATH_MAX];
};

/* Return 0 to run, 1 after --help, or -1 for invalid settings. */
int config_parse(int argc, char **argv, struct server_config *config);

#endif
