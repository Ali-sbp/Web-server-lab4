#include "config.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --port NUMBER     TCP port (default: 8080)\n"
           "  --root DIRECTORY  Document root (default: ./www)\n"
           "  --log FILE        Log file (default: ./server.log)\n"
           "  --timeout SECONDS Inactivity timeout (default: 30)\n"
           "  --foreground      Stay attached to the terminal\n"
           "  --help            Show this help\n", program);
}

static int parse_number(const char *text, long maximum, int *result)
{
    char *end;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > maximum)
        return -1;
    *result = (int)value;
    return 0;
}

int config_parse(int argc, char **argv, struct server_config *config)
{
    const char *root = "./www";
    const char *log_path = "./server.log";
    const struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"root", required_argument, NULL, 'r'},
        {"log", required_argument, NULL, 'l'},
        {"timeout", required_argument, NULL, 't'},
        {"foreground", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    *config = (struct server_config){.port = 8080, .timeout = 30};

    /* 1. Apply command-line overrides to the defaults. */
    int option;
    while ((option = getopt_long(argc, argv, "p:r:l:t:fh", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_number(optarg, 65535, &config->port) == -1) {
                fprintf(stderr, "Port must be between 1 and 65535\n");
                return -1;
            }
            break;
        case 'r': root = optarg; break;
        case 'l': log_path = optarg; break;
        case 't':
            if (parse_number(optarg, INT_MAX, &config->timeout) == -1) {
                fprintf(stderr, "Timeout must be a positive integer in seconds\n");
                return -1;
            }
            break;
        case 'f': config->foreground = 1; break;
        case 'h': usage(argv[0]); return 1;
        default: return -1;
        }
    }
    if (optind != argc || *log_path == '\0') {
        fprintf(stderr, "Unexpected argument or empty log path; use --help\n");
        return -1;
    }

    /* 2. Resolve paths before daemonization changes the working directory. */
    if (realpath(root, config->root) == NULL) {
        perror("document root");
        return -1;
    }
    int length;
    if (log_path[0] == '/') {
        length = snprintf(config->log_path, sizeof(config->log_path),
                          "%s", log_path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd");
            return -1;
        }
        length = snprintf(config->log_path, sizeof(config->log_path),
                          "%s/%s", cwd, log_path);
    }
    if (length < 0 || (size_t)length >= sizeof(config->log_path)) {
        fprintf(stderr, "Log path is too long\n");
        return -1;
    }
    return 0;
}
