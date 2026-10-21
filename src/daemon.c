#include "daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int report_status(int fd, char status)
{
    ssize_t n;
    do {
        n = write(fd, &status, 1);
    } while (n == -1 && errno == EINTR);
    close(fd);
    return n == 1 ? 0 : -1;
}

int daemon_detach(void)
{
    /* 1. Create a startup pipe so the launcher can report setup failures. */
    int status_pipe[2];
    if (pipe(status_pipe) == -1)
        return -1;

    pid_t pid = fork();
    if (pid == -1) {
        int error = errno;
        close(status_pipe[0]);
        close(status_pipe[1]);
        errno = error;
        return -1;
    }
    if (pid > 0) {
        close(status_pipe[1]);
        char status = 0;
        ssize_t n;
        do {
            n = read(status_pipe[0], &status, 1);
        } while (n == -1 && errno == EINTR);
        close(status_pipe[0]);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        _exit(n == 1 && status == 1 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    close(status_pipe[0]);

    /* 2. Leave the original session and controlling terminal. */
    if (setsid() == -1)
        goto failure;

    /* 3. Fork again so the daemon is not a session leader. */
    pid = fork();
    if (pid == -1)
        goto failure;
    if (pid > 0) {
        close(status_pipe[1]);
        _exit(EXIT_SUCCESS);
    }

    /* 4. Use a stable working directory and restrictive creation mask. */
    umask(027);
    if (chdir("/") == -1)
        goto failure;

    /* 5. Redirect stdin, stdout, and stderr to /dev/null.
       dup2 arguments: source descriptor, destination descriptor. */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd == -1)
        goto failure;
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (dup2(null_fd, fd) == -1) {
            int error = errno;
            if (null_fd > STDERR_FILENO)
                close(null_fd);
            errno = error;
            goto failure;
        }
    }
    if (null_fd > STDERR_FILENO)
        close(null_fd);

    /* Keep the listening, logging, and directory descriptors open. */
    return report_status(status_pipe[1], 1);

failure:
    {
        int error = errno;
        report_status(status_pipe[1], 0);
        errno = error;
        return -1;
    }
}
