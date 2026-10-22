#include "http.h"
#include "files.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

/* Supply the text after the numeric status, for example "200 OK". */
static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

int http_send_all(int sock, const void *data, size_t length)
{
    const char *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        /* Connected socket, remaining data, remaining size, no flags.
           SIGPIPE is ignored by server.c, so disconnects return errors. */
        ssize_t n = send(sock, bytes + sent, length - sent, 0);
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1)
            return -1;
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int http_send_headers(int sock, int status, const char *type,
                      uintmax_t length, const char *extra_headers)
{
    char date[64], headers[1024];
    time_t now = time(NULL);
    struct tm utc;
    /* HTTP dates use GMT, regardless of the server's local time zone. */
    gmtime_r(&now, &utc);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc);

    /* HTTP lines end with CRLF; an empty line ends the header section. */
    int size = snprintf(headers, sizeof(headers),
                        "HTTP/1.1 %d %s\r\n"
                        "Date: %s\r\n"
                        "Server: unix-lab4\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %" PRIuMAX "\r\n"
                        "Connection: close\r\n"
                        "%s\r\n",
                        status, reason_phrase(status), date, type, length,
                        extra_headers == NULL ? "" : extra_headers);
    if (size < 0 || (size_t)size >= sizeof(headers)) {
        errno = EOVERFLOW;
        return -1;
    }
    return http_send_all(sock, headers, (size_t)size);
}

int http_send_error(int sock, int status, int head_only)
{
    /* A small error body uses the same header-building code as successful replies. */
    char body[128];
    int length = snprintf(body, sizeof(body), "%d %s\n",
                          status, reason_phrase(status));
    const char *extra = status == 405 ? "Allow: GET, HEAD, POST\r\n" : NULL;
    if (http_send_headers(sock, status, "text/plain; charset=utf-8",
                          (uintmax_t)length, extra) == -1)
        return -1;
    /* HEAD reports the body length that GET would return, but sends no body. */
    if (!head_only)
        return http_send_all(sock, body, (size_t)length);
    return 0;
}

static int parse_length(const char *value, size_t *length)
{
    if (*value == '\0')
        return -1;
    size_t result = 0;
    /* Accept decimal digits only; check for overflow before multiplying by ten. */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (SIZE_MAX - (*p - '0')) / 10)
            return -1;
        result = result * 10 + (*p - '0');
    }
    *length = result;
    return 0;
}

/* Methods and header names use HTTP token characters, without spaces or colons. */
static int is_token(const char *text)
{
    if (*text == '\0')
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p)))
            return 0;
    }
    return 1;
}

/* Parse in place: replace separators with '\0' to create individual C strings.
   Fill request for the dispatcher; return 0 on success or an HTTP error status. */
static int parse_headers(char *buffer, struct http_request *request)
{
    /* 1. Isolate the request line, for example "GET /sample.txt HTTP/1.1". */
    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL)
        return 400;
    *line_end = '\0';

    /* 2. End the method at the first space; target advances to the URL. */
    char *target = strchr(buffer, ' ');
    if (target == NULL)
        return 400;
    *target++ = '\0';
    /* Remember HEAD early so even a later parsing error omits the response body. */
    request->head_only = strcmp(buffer, "HEAD") == 0;
    if (!is_token(buffer) || strlen(buffer) >= sizeof(request->method))
        return 400;
    /* Copy the method (e.g. "GET") into request; its size was checked above. */
    strcpy(request->method, buffer);

    /* End the target at the next space; version advances to "HTTP/1.1". */
    char *version = strchr(target, ' ');
    if (version == NULL)
        return 400;
    *version++ = '\0';
    if (strlen(target) >= sizeof(request->target))
        return 414;
    /* Preserve the original target, including any query string, for logging. */
    strcpy(request->target, target);

    /* 3. Accept the supported HTTP versions and a path starting with '/'. */
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        return strncmp(version, "HTTP/", 5) == 0 ? 505 : 400;
    if (target[0] != '/')
        return 400;
    /* A URL target cannot contain raw whitespace or control characters. */
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p <= 32 || *p == 127)
            return 400;
    }

    /* 4. Scan header lines after the request line; +2 skips its CRLF. */
    int hosts = 0, has_transfer_encoding = 0, has_expect = 0;
    char *line = line_end + 2;
    while (*line != '\0') {
        line_end = strstr(line, "\r\n");
        if (line_end == NULL)
            return 400;
        *line_end = '\0';

        /* Split "Host: lab4.test:8080" at its first colon into name and value. */
        char *colon = strchr(line, ':');
        if (colon == NULL)
            return 400;
        *colon = '\0';
        if (!is_token(line))
            return 400;
        char *value = colon + 1;

        /* Trim spaces/tabs around the value; reject other control characters. */
        while (*value == ' ' || *value == '\t')
            ++value;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
            if ((*p < 32 && *p != '\t') || *p == 127)
                return 400;
        }

        /* Header names are case-insensitive; retain only settings we need. */
        if (strcasecmp(line, "Host") == 0) {
            /* Reject an empty or repeated Host header. */
            if (++hosts > 1 || *value == '\0')
                return 400;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            /* Convert the byte count and reject repeated/invalid lengths. */
            if (request->has_length || parse_length(value, &request->content_length))
                return 400;
            request->has_length = 1;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            /* Remember this so unsupported body framing can be rejected below. */
            has_transfer_encoding = 1;
        } else if (strcasecmp(line, "Expect") == 0) {
            /* Let the upload handler know curl may be waiting for 100 Continue. */
            if (has_expect++ || strcasecmp(value, "100-continue") != 0)
                return 417;
            request->expect_continue = 1;
        }
        line = line_end + 2;  /* Advance past this line's CRLF to the next header. */
    }

    /* 5. HTTP/1.1 requires Host. We do not decode Transfer-Encoding bodies;
       combining Transfer-Encoding and Content-Length is ambiguous and rejected. */
    if (strcmp(version, "HTTP/1.1") == 0 && hosts != 1)
        return 400;
    if (has_transfer_encoding)
        return request->has_length ? 400 : 501;

    /* 6. Allow our three methods, then decode and validate the URL path. */
    if (strcmp(request->method, "GET") != 0 &&
        strcmp(request->method, "HEAD") != 0 &&
        strcmp(request->method, "POST") != 0)
        return 405;
    /* Keep target unchanged; path gets URL decoding and query-string removal. */
    int status = files_decode_path(request->target, request->path,
                                   sizeof(request->path));
    if (status != 0)
        return status;

    /* POST needs a known, bounded file length. GET/HEAD bodies are not supported. */
    if (strcmp(request->method, "POST") == 0) {
        if (!request->has_length)
            return 411;
        if (request->content_length > MAX_UPLOAD_SIZE)
            return 413;
    } else if (request->content_length != 0) {
        return 400;
    }
    return 0;  /* The dispatcher can now use the populated request structure. */
}

void http_handle_client(int sock, struct files_context *files,
                        struct logger *log, const char *peer)
{
    struct http_request request = {0};
    /* Keep useful log placeholders if the client sends an incomplete request. */
    strcpy(request.method, "-");
    strcpy(request.target, "-");
    char buffer[MAX_HEADER_SIZE + 1];
    size_t used = 0;
    int status = 0;
    char *header_end = NULL;

    /* 1. Accumulate headers: TCP may split a request across many reads. */
    while (header_end == NULL) {
        if (used == MAX_HEADER_SIZE) {
            status = 431;
            break;
        }
        /* Connected socket, unused buffer space, available size, no flags. */
        ssize_t n = recv(sock, buffer + used, MAX_HEADER_SIZE - used, 0);
        if (n == -1 && errno == EINTR)
            continue;
        if (n <= 0) {
            status = n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)
                     ? 408 : 400;
            if (n == -1)
                logger_write(log, "ERROR", "peer=%s reading request: %s",
                             peer, strerror(errno));
            break;
        }
        used += (size_t)n;
        /* Terminate the accumulated text so strstr() can find the empty line. */
        buffer[used] = '\0';
        header_end = strstr(buffer, "\r\n\r\n");
        /* Reject embedded null bytes in incomplete headers, not binary bodies. */
        if (header_end == NULL && memchr(buffer, '\0', used) != NULL) {
            status = 400;
            break;
        }
    }

    if (status == 0) {
        /* Include the four delimiter bytes when finding where the body begins. */
        size_t header_size = (size_t)(header_end - buffer) + 4;
        /* Preserve any upload bytes received together with the headers. */
        request.body = buffer + header_size;
        request.body_size = used - header_size;
        /* Headers must be text, but the preserved upload body may contain zeros. */
        if (memchr(buffer, '\0', header_size) != NULL) {
            status = 400;
        } else {
            /* End the header text before parsing; keep the last header's CRLF.
               request.body still points beyond the full delimiter, at file data. */
            header_end[2] = '\0';
            status = parse_headers(buffer, &request);
        }
    }

    /* 2. Dispatch one request, then let server.c close the connection. */
    if (status != 0) {
        if (http_send_error(sock, status, request.head_only) == -1)
            logger_write(log, "ERROR", "peer=%s sending error: %s",
                         peer, strerror(errno));
    } else if (strcmp(request.method, "POST") == 0) {
        status = files_upload(sock, files, &request, log);
    } else {
        status = files_serve(sock, files, &request, log);
    }

    /* 3. One access record per request, including its worker PID. */
    logger_write(log, "INFO", "peer=%s method=%s target=%s status=%d",
                 peer, request.method, request.target, status);
}
