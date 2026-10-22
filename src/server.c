#include "server.h"
#include "files.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// These flags can be updated safely by the termination signal handler.
static volatile sig_atomic_t stopping = 0;
static volatile sig_atomic_t listening_fd = -1;

static void request_stop(int signal_number)
{
    (void)signal_number;
    int saved_errno = errno;
    stopping = 1;
    // close() is signal-safe and also handles a signal just before accept().
    if (listening_fd != -1) {
        close((int)listening_fd);
        listening_fd = -1;
    }
    errno = saved_errno;
}

void server_close(int listener)
{
    // A termination signal may already have closed the listener.
    if (listening_fd == listener) {
        listening_fd = -1;
        close(listener);
    }
}

static int configure_signals(void)
{
    // Stop accepting clients on SIGTERM or terminal Ctrl+C.
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = request_stop;
    if (sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1)
        return -1;

    // Reap finished children automatically; prevent zombie processes.
    action.sa_handler = SIG_DFL;
    action.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &action, NULL) == -1)
        return -1;

    // Make send() return an error if the client disconnects.
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    return sigaction(SIGPIPE, &action, NULL);
}

int server_listen(const struct server_config *config, struct logger *log)
{
    // 1. Create a socket: IPv4, byte stream, default protocol (TCP).
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        logger_write(log, "ERROR", "socket: %s", strerror(errno));
        return -1;
    }

    /* 2. Allow rebinding after restart.
       Socket, option level, option, value pointer, value size. */
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1)
        goto failure;

    // Convert the configured port to network byte order; accept any local IPv4 IP.
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)config->port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    /* 3. Bind all local IPv4 interfaces, then listen.
       bind: socket, local address, address size.
       listen: socket, pending connection queue limit. */
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 16) == -1)
        goto failure;
    listening_fd = listener;
    if (configure_signals() == -1)
        goto failure;
    // main() passes this listening descriptor to the parent's accept/fork loop.
    return listener;

failure:
    logger_write(log, "ERROR", "listener setup: %s", strerror(errno));
    close(listener);
    listening_fd = -1;
    return -1;
}

int server_run(int listener, const struct server_config *config,
               struct files_context *files, struct logger *log)
{
    // 4. The parent only accepts clients and creates workers; it sends no HTTP data.
    while (!stopping) {
        struct sockaddr_in address;
        socklen_t address_size = sizeof(address);

        /* Wait for a connection and obtain a new socket for that client.
           Parameters: listener, output peer address, pointer to address capacity. */
        int client = accept(listener, (struct sockaddr *)&address, &address_size);
        if (client == -1) {
            // A shutdown signal closes the listener; an unrelated signal can retry.
            if (stopping)
                break;
            if (errno == EINTR)
                continue;
            logger_write(log, "ERROR", "accept: %s", strerror(errno));
            return -1;
        }
        if (stopping) {
            close(client);
            break;
        }

        /* 5. Bound blocking reads/writes by the inactivity timeout.
           Set it before fork() so the child's socket already has these settings.
           Socket, option level, option, timeout pointer, timeout size. */
        struct timeval timeout = {.tv_sec = config->timeout, .tv_usec = 0};
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) == -1 ||
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout)) == -1) {
            logger_write(log, "ERROR", "client timeout setup: %s", strerror(errno));
            close(client);
            continue;
        }

        /* 6. Create a worker; both processes inherit descriptors for this client.
           fork(): -1 on failure, 0 in child, child PID in parent. */
        pid_t pid = fork();
        if (pid == -1) {
            logger_write(log, "ERROR", "fork: %s", strerror(errno));
            close(client);
            continue;
        }
        if (pid == 0) {
            // CHILD: serve one request and exit; never accept clients.
            listening_fd = -1;
            close(listener);
            // Format the peer's IP and port for this request's log records.
            char ip[INET_ADDRSTRLEN] = "unknown";
            inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
            char peer[64];
            snprintf(peer, sizeof(peer), "%s:%u", ip,
                     (unsigned int)ntohs(address.sin_port));
            /* Hand the connected socket to HTTP parsing and file handling.
               This call reads the request, sends the response, and logs its status. */
            http_handle_client(client, files, log, peer);

            // The response is finished; release this child's socket and file copies.
            close(client);
            files_close(files);
            logger_close(log);
            _exit(0);  // End the child without flushing inherited stdio buffers.
        }

        /* PARENT: close our copy; the child's descriptor keeps the connection open.
           Return to accept() while the child independently serves its request. */
        close(client);
    }
    return 0;
}
