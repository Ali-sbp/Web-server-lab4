#ifndef LOGGER_H
#define LOGGER_H

struct logger {
    int fd;
};

int logger_open(struct logger *log, const char *path);
void logger_write(struct logger *log, const char *level,
                  const char *format, ...);
void logger_close(struct logger *log);

#endif
