#ifndef FILES_H
#define FILES_H

#include <stddef.h>
#include "http.h"
#include "logger.h"

struct files_context {
    int root_fd;
    int uploads_fd;
};

int files_open(struct files_context *files, const char *root,
                struct logger *log);
void files_close(struct files_context *files);
int files_decode_path(const char *target, char *path, size_t capacity);
int files_serve(int sock, struct files_context *files,
                const struct http_request *request, struct logger *log);
int files_upload(int sock, struct files_context *files,
                 const struct http_request *request, struct logger *log);

#endif
