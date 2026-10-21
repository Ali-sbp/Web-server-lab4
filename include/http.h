#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "logger.h"

struct files_context;

struct http_request {
    char method[16];
    char target[MAX_TARGET_SIZE];
    char path[MAX_TARGET_SIZE];
    size_t content_length;
    int has_length;
    int expect_continue;
    int head_only;
    const char *body;
    size_t body_size;
};

void http_handle_client(int sock, struct files_context *files,
                        struct logger *log, const char *peer);
int http_send_all(int sock, const void *data, size_t length);
int http_send_headers(int sock, int status, const char *type,
                      uintmax_t length, const char *extra_headers);
int http_send_error(int sock, int status, int head_only);

#endif
