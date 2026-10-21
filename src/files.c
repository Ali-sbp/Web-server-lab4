#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int files_open(struct files_context *files, const char *root,
                struct logger *log)
{
    *files = (struct files_context){.root_fd = -1, .uploads_fd = -1};
    /* 1. Retain directory descriptors so daemon chdir() cannot break paths. */
    files->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (files->root_fd == -1)
        goto failure;

    /* Directory descriptor, child name, permissions for a new directory. */
    if (mkdirat(files->root_fd, "uploads", 0750) == -1 && errno != EEXIST)
        goto failure;
    files->uploads_fd = openat(files->root_fd, "uploads",
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (files->uploads_fd == -1)
        goto failure;
    return 0;

failure:
    logger_write(log, "ERROR", "opening document/upload directories: %s",
                 strerror(errno));
    files_close(files);
    return -1;
}

void files_close(struct files_context *files)
{
    if (files->uploads_fd != -1)
        close(files->uploads_fd);
    if (files->root_fd != -1)
        close(files->root_fd);
    files->uploads_fd = files->root_fd = -1;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int files_decode_path(const char *target, char *path, size_t capacity)
{
    /* 2. Decode URL escapes once; a query string is not part of the filename. */
    size_t used = 0;
    for (size_t i = 0; target[i] != '\0' && target[i] != '?'; ++i) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            if (target[i + 1] == '\0' || target[i + 2] == '\0')
                return 400;
            int high = hex_digit((unsigned char)target[i + 1]);
            int low = hex_digit((unsigned char)target[i + 2]);
            if (high < 0 || low < 0)
                return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || c == '#')
            return 400;
        if (used + 1 >= capacity)
            return 414;
        path[used++] = (char)c;
    }
    path[used] = '\0';
    if (used == 0 || path[0] != '/')
        return 400;

    /* Hide dotfiles, including temporary uploads, and reject traversal. */
    for (const char *p = path; *p; ++p) {
        if (*p == '/' && p[1] == '.')
            return 403;
    }
    return 0;
}

static int filesystem_status(int error)
{
    switch (error) {
    case ENOENT: case ENOTDIR: return 404;
    case EACCES: case EPERM: case ELOOP: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

static int fail_request(int sock, int status, int head_only,
                        struct logger *log)
{
    if (http_send_error(sock, status, head_only) == -1)
        logger_write(log, "ERROR", "sending HTTP %d: %s", status, strerror(errno));
    return status;
}

static const char *content_type(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (extension == NULL) return "application/octet-stream";
    if (!strcasecmp(extension, ".html") || !strcasecmp(extension, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(extension, ".txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(extension, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(extension, ".js")) return "text/javascript; charset=utf-8";
    if (!strcasecmp(extension, ".json")) return "application/json";
    if (!strcasecmp(extension, ".png")) return "image/png";
    if (!strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(extension, ".gif")) return "image/gif";
    if (!strcasecmp(extension, ".svg")) return "image/svg+xml";
    if (!strcasecmp(extension, ".pdf")) return "application/pdf";
    return "application/octet-stream";
}

static int open_static(int root_fd, const char *path)
{
    char relative[MAX_TARGET_SIZE];
    if (strcmp(path, "/") == 0)
        strcpy(relative, "index.html");
    else
        strcpy(relative, path + 1);

    /* 3. Walk beneath the root one component at a time.
       O_NOFOLLOW rejects symlinks, including those in parent directories. */
    int directory = dup(root_fd);
    if (directory == -1)
        return -1;
    char *component = relative;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash != NULL)
            *slash = '\0';
        if (*component == '\0') {
            close(directory);
            errno = EACCES;
            return -1;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (slash != NULL)
            flags |= O_DIRECTORY;
        /* Parent directory, component name, open flags. */
        int next = openat(directory, component, flags);
        int error = errno;
        close(directory);
        errno = error;
        if (next == -1 || slash == NULL)
            return next;
        directory = next;
        component = slash + 1;
    }
}

int files_serve(int sock, struct files_context *files,
                const struct http_request *request, struct logger *log)
{
    int fd = open_static(files->root_fd, request->path);
    if (fd == -1) {
        int error = errno;
        logger_write(log, "ERROR", "open %s: %s", request->path, strerror(error));
        return fail_request(sock, filesystem_status(error), request->head_only, log);
    }

    struct stat info;
    if (fstat(fd, &info) == -1) {
        logger_write(log, "ERROR", "fstat %s: %s", request->path, strerror(errno));
        close(fd);
        return fail_request(sock, 500, request->head_only, log);
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        close(fd);
        return fail_request(sock, 403, request->head_only, log);
    }

    /* 4. GET and HEAD share headers; HEAD deliberately omits file bytes. */
    const char *name = strcmp(request->path, "/") == 0 ? "index.html" : request->path;
    if (http_send_headers(sock, 200, content_type(name),
                          (uintmax_t)info.st_size, NULL) == -1) {
        logger_write(log, "ERROR", "sending file headers: %s", strerror(errno));
        close(fd);
        return 200;
    }

    if (!request->head_only) {
        char buffer[16384];
        off_t remaining = info.st_size;
        while (remaining > 0) {
            size_t amount = remaining < (off_t)sizeof(buffer)
                            ? (size_t)remaining : sizeof(buffer);
            /* File descriptor, destination buffer, maximum byte count. */
            ssize_t n = read(fd, buffer, amount);
            if (n == -1 && errno == EINTR)
                continue;
            if (n <= 0) {
                logger_write(log, "ERROR", "reading %s: %s", request->path,
                             n == 0 ? "file shortened during transfer" : strerror(errno));
                break;
            }
            if (http_send_all(sock, buffer, (size_t)n) == -1) {
                logger_write(log, "ERROR", "sending %s: %s", request->path, strerror(errno));
                break;
            }
            remaining -= n;
        }
    }
    close(fd);
    return 200;
}

static int valid_upload_name(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length > 255 || name[0] == '.')
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static int write_all(int fd, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
        if (n == -1 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

int files_upload(int sock, struct files_context *files,
                 const struct http_request *request, struct logger *log)
{
    /* 5. POST accepts a raw file body at /upload/<simple-filename>. */
    if (strncmp(request->path, "/upload/", 8) != 0)
        return fail_request(sock, 404, 0, log);
    const char *name = request->path + 8;
    if (!valid_upload_name(name))
        return fail_request(sock, 400, 0, log);

    struct stat existing;
    if (fstatat(files->uploads_fd, name, &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return fail_request(sock, 409, 0, log);
    if (errno != ENOENT) {
        logger_write(log, "ERROR", "checking upload %s: %s", name, strerror(errno));
        return fail_request(sock, 500, 0, log);
    }

    /* 6. Receive into a hidden file; incomplete data is never downloadable. */
    char temporary[64];
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".upload-%ld-%u",
                 (long)getpid(), attempt);
        /* Directory, temporary name, exclusive creation flags, permissions. */
        fd = openat(files->uploads_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd != -1 || errno != EEXIST)
            break;
    }
    if (fd == -1) {
        logger_write(log, "ERROR", "creating upload %s: %s", name, strerror(errno));
        return fail_request(sock, 500, 0, log);
    }

    int status = 201;
    /* curl may wait for permission before transmitting a larger body. */
    if (request->expect_continue && request->body_size < request->content_length) {
        const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (http_send_all(sock, interim, sizeof(interim) - 1) == -1) {
            logger_write(log, "ERROR", "sending 100 Continue: %s", strerror(errno));
            status = 400;
            goto cleanup;
        }
    }

    size_t received = request->body_size < request->content_length
                      ? request->body_size : request->content_length;
    if (write_all(fd, request->body, received) == -1) {
        logger_write(log, "ERROR", "writing upload %s: %s", name, strerror(errno));
        status = 500;
        goto cleanup;
    }

    char buffer[16384];
    while (received < request->content_length) {
        size_t remaining = request->content_length - received;
        size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        /* Socket, file-data buffer, bytes still needed, no flags. */
        ssize_t n = recv(sock, buffer, amount, 0);
        if (n == -1 && errno == EINTR)
            continue;
        if (n <= 0) {
            status = n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)
                     ? 408 : 400;
            logger_write(log, "ERROR", "incomplete upload %s: %s", name,
                         n == 0 ? "client disconnected" : strerror(errno));
            goto cleanup;
        }
        if (write_all(fd, buffer, (size_t)n) == -1) {
            logger_write(log, "ERROR", "writing upload %s: %s", name, strerror(errno));
            status = 500;
            goto cleanup;
        }
        received += (size_t)n;
    }

    if (close(fd) == -1) {
        fd = -1;
        logger_write(log, "ERROR", "closing upload %s: %s", name, strerror(errno));
        status = 500;
        goto cleanup;
    }
    fd = -1;

    /* 7. Publish the completed file without replacing an existing name.
       Source directory/name, destination directory/name, no extra flags.
       linkat() also protects against simultaneous uploads of the same name. */
    if (linkat(files->uploads_fd, temporary, files->uploads_fd, name, 0) == -1) {
        int error = errno;
        status = error == EEXIST ? 409 : 500;
        logger_write(log, "ERROR", "publishing upload %s: %s", name, strerror(error));
    }

cleanup:
    if (fd != -1)
        close(fd);
    if (unlinkat(files->uploads_fd, temporary, 0) == -1)
        logger_write(log, "ERROR", "removing temporary upload: %s", strerror(errno));
    if (status != 201)
        return fail_request(sock, status, 0, log);

    /* 8. Return the URL where the completed upload can be downloaded. */
    char extra[320], body[320];
    snprintf(extra, sizeof(extra), "Location: /uploads/%s\r\n", name);
    int length = snprintf(body, sizeof(body), "Uploaded: /uploads/%s\n", name);
    if (http_send_headers(sock, 201, "text/plain; charset=utf-8",
                          (uintmax_t)length, extra) == -1 ||
        http_send_all(sock, body, (size_t)length) == -1)
        logger_write(log, "ERROR", "sending upload response: %s", strerror(errno));
    logger_write(log, "INFO", "uploaded name=%s bytes=%zu", name, received);
    return 201;
}
