#include "config.h"
#include "daemon.h"
#include "files.h"
#include "logger.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    /* 1. Read defaults and command-line overrides. */
    struct server_config config;
    int result = config_parse(argc, argv, &config);
    if (result != 0)
        return result == 1 ? EXIT_SUCCESS : EXIT_FAILURE;

    /* 2. Prepare logging, directories, and the listening socket. */
    struct logger log;
    if (logger_open(&log, config.log_path) == -1) {
        perror("log file");
        return EXIT_FAILURE;
    }
    struct files_context files;
    if (files_open(&files, config.root, &log) == -1) {
        fprintf(stderr, "Cannot prepare document root; see %s\n", config.log_path);
        logger_close(&log);
        return EXIT_FAILURE;
    }
    int listener = server_listen(&config, &log);
    if (listener == -1) {
        fprintf(stderr, "Cannot start listener; see %s\n", config.log_path);
        files_close(&files);
        logger_close(&log);
        return EXIT_FAILURE;
    }

    /* 3. Detach unless foreground debugging was requested. */
    if (!config.foreground && daemon_detach() == -1) {
        logger_write(&log, "ERROR", "daemon setup: %s", strerror(errno));
        result = -1;
    } else {
        logger_write(&log, "INFO", "started port=%d root=%s timeout=%d mode=%s",
                     config.port, config.root, config.timeout,
                     config.foreground ? "foreground" : "daemon");
        if (config.foreground) {
            printf("HTTP server: http://127.0.0.1:%d/ (Ctrl+C to stop)\n", config.port);
            fflush(stdout);
        }

        /* 4. Run the accept/fork loop. */
        result = server_run(listener, &config, &files, &log);
        logger_write(&log, "INFO", "stopped accepting clients");
    }

    /* 5. Release the parent's resources; active children finish separately. */
    server_close(listener);
    files_close(&files);
    logger_close(&log);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
