#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int logger_open(struct logger *log, const char *path)
{
    /* Path, append/create flags, permissions used for a newly created file. */
    log->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    return log->fd == -1 ? -1 : 0;
}

void logger_write(struct logger *log, const char *level,
                  const char *format, ...)
{
    int saved_errno = errno;
    char message[3072], line[3328], timestamp[64];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local);

    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    /* Keep client-supplied text from creating extra log lines. */
    for (char *p = message; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127)
            *p = ' ';
    }
    int length = snprintf(line, sizeof(line), "%s pid=%ld %s %s\n",
                          timestamp, (long)getpid(), level, message);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        errno = saved_errno;
        return;
    }

    /* Serialize complete records from the parent and all children.
       Descriptor, blocking record-lock operation, whole-file lock. */
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int result;
    do {
        result = fcntl(log->fd, F_SETLKW, &lock);
    } while (result == -1 && errno == EINTR);

    if (result != -1) {
        size_t written = 0;
        while (written < (size_t)length) {
            ssize_t n = write(log->fd, line + written,
                              (size_t)length - written);
            if (n == -1 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            written += (size_t)n;
        }
        lock.l_type = F_UNLCK;
        fcntl(log->fd, F_SETLK, &lock);
    }
    errno = saved_errno;
}

void logger_close(struct logger *log)
{
    if (log->fd != -1)
        close(log->fd);
    log->fd = -1;
}
