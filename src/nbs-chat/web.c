/*
 * web.c — HTTP server for nbs-chat web viewer
 *
 * Serves a read-only browser interface to nbs-chat files.
 * Single-threaded with poll() for multiplexing SSE connections.
 *
 * Usage: nbs-chat-web <file> [--port=N] [--bind=ADDR] [--last=N]
 *
 * Endpoints:
 *   GET /              HTML page (embedded, single request)
 *   GET /api/messages   JSON messages (query: ?since=N&last=M)
 *   GET /events         SSE stream (live updates)
 *
 * Exit codes:
 *   0 - Clean exit (SIGINT/SIGTERM)
 *   1 - General error
 *   2 - Chat file not found
 *   4 - Invalid arguments
 */

/* strcasestr requires _GNU_SOURCE on Linux */
#define _GNU_SOURCE

#include "chat_file.h"
#include "bus_bridge.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "web_assets.h"

/* --- Configuration --- */

#define DEFAULT_PORT       8080
#define DEFAULT_BIND       "::1"
#define MAX_SSE_CLIENTS    16
#define POLL_TIMEOUT_MS    1500     /* Match terminal.c */
#define HEARTBEAT_INTERVAL 15      /* Seconds between SSE heartbeats */
#define MAX_REQUEST_SIZE   8192
#define MAX_JSON_BUF       (4 * 1024 * 1024)  /* 4 MB response buffer */
#define LISTEN_BACKLOG     8

/* --- Globals --- */

static const char *g_chat_path = NULL;
static int g_initial_last = -1;   /* --last=N, -1 = all */
static volatile sig_atomic_t g_quit = 0;

/* --- Signal handling --- */

static void signal_handler(int sig) {
    (void)sig;
    g_quit = 1;
}

/* --- HTTP request --- */

typedef struct {
    char method[8];
    char path[1024];
    char query[1024];         /* Everything after ? in path */
    int last_event_id;        /* -1 if not present */
    int content_length;       /* -1 if not present */
    char body[MAX_REQUEST_SIZE + 1]; /* Request body (for POST) */
    int body_len;             /* Bytes in body */
} http_request_t;

/* --- SSE client --- */

typedef struct {
    int fd;
    int last_sent_index;
    int alive;
} sse_client_t;

/* ================================================================
 * JSON Serialisation
 * ================================================================ */

/*
 * json_escape — Escape a string for JSON embedding.
 *
 * Preconditions:
 *   - input != NULL
 *   - output != NULL
 *   - output_size > 0
 *
 * Postconditions:
 *   - output is NUL-terminated
 *   - Returns bytes written (excluding NUL), or -1 on truncation
 *
 * Escapes: " \ \b \f \n \r \t and control chars 0x00-0x1F as \uXXXX
 */
static int json_escape(const char *input, char *output, size_t output_size) {
    ASSERT_MSG(input != NULL, "json_escape: input is NULL");
    ASSERT_MSG(output != NULL, "json_escape: output is NULL");
    ASSERT_MSG(output_size > 0, "json_escape: output_size is 0");

    size_t pos = 0;
    const size_t limit = output_size - 1;  /* Reserve space for NUL */

    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; p++) {
        const char *esc = NULL;
        char ubuf[7];

        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (*p < 0x20) {
                    snprintf(ubuf, sizeof(ubuf), "\\u%04x", *p);
                    esc = ubuf;
                }
                break;
        }

        if (esc) {
            size_t elen = strlen(esc);
            if (pos + elen > limit) {
                output[pos] = '\0';
                return -1;
            }
            memcpy(output + pos, esc, elen);
            pos += elen;
        } else {
            if (pos >= limit) {
                output[pos] = '\0';
                return -1;
            }
            output[pos++] = (char)*p;
        }
    }

    output[pos] = '\0';

    /* Postcondition: output is NUL-terminated */
    ASSERT_MSG(output[pos] == '\0', "json_escape: missing NUL terminator");
    return (int)pos;
}

/*
 * json_message — Serialise a chat_message_t to JSON object string.
 *
 * Format: {"index":N,"handle":"...","content":"...","timestamp":T}
 *
 * Returns bytes written (excluding NUL), or -1 on truncation.
 */
int json_message(const chat_message_t *msg, int index,
                 char *buf, size_t buf_size) {
    ASSERT_MSG(msg != NULL, "json_message: msg is NULL");
    ASSERT_MSG(buf != NULL, "json_message: buf is NULL");
    ASSERT_MSG(buf_size > 0, "json_message: buf_size is 0");
    ASSERT_MSG(index >= 0, "json_message: negative index %d", index);

    /* Escape handle and content */
    char handle_esc[MAX_HANDLE_LEN * 6 + 1];
    char *content_esc = NULL;
    int result = -1;

    int hlen = json_escape(msg->handle, handle_esc, sizeof(handle_esc));
    if (hlen < 0) goto done;

    /* Content can be up to MAX_MESSAGE_LEN; worst case 6x expansion */
    size_t content_esc_size = (msg->content_len + 1) * 6 + 1;
    if (content_esc_size > MAX_JSON_BUF) content_esc_size = MAX_JSON_BUF;
    content_esc = malloc(content_esc_size);
    if (!content_esc) goto done;

    int clen = json_escape(msg->content ? msg->content : "",
                           content_esc, content_esc_size);
    if (clen < 0) goto done;

    int written = snprintf(buf, buf_size,
        "{\"index\":%d,\"handle\":\"%s\",\"content\":\"%s\",\"timestamp\":%lld}",
        index, handle_esc, content_esc, (long long)msg->timestamp);

    if (written < 0 || (size_t)written >= buf_size) {
        result = -1;
    } else {
        result = written;
    }

done:
    free(content_esc);
    return result;
}

/* ================================================================
 * HTTP Parsing
 * ================================================================ */

/*
 * parse_query_int — Extract integer parameter from query string.
 *
 * Looks for key=VALUE in a &-separated query string.
 * Returns default_val if key not found or invalid.
 */
static int parse_query_int(const char *query, const char *key, int default_val) {
    if (!query || !key || query[0] == '\0') return default_val;

    size_t klen = strlen(key);
    const char *p = query;

    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char *endptr;
            errno = 0;
            long val = strtol(p + klen + 1, &endptr, 10);
            if (errno != 0 || endptr == p + klen + 1) return default_val;
            if (val > INT32_MAX) val = INT32_MAX;
            if (val < 0) val = 0;
            return (int)val;
        }
        /* Advance to next & */
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }

    return default_val;
}

/*
 * parse_request — Read and parse an HTTP request from a socket.
 *
 * Reads up to MAX_REQUEST_SIZE bytes. Extracts method, path, query.
 * Scans for Last-Event-ID header.
 *
 * Returns 0 on success, -1 on error (connection closed, malformed, too large).
 */
static int parse_request(int fd, http_request_t *req) {
    ASSERT_MSG(req != NULL, "parse_request: req is NULL");
    ASSERT_MSG(fd >= 0, "parse_request: invalid fd %d", fd);

    memset(req, 0, sizeof(*req));
    req->last_event_id = -1;
    req->content_length = -1;

    char buf[MAX_REQUEST_SIZE + 1];
    ssize_t total = 0;

    /* Read until we have \r\n\r\n or buffer is full */
    while (total < MAX_REQUEST_SIZE) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int poll_rc = poll(&pfd, 1, 5000);
        if (poll_rc <= 0) {
            /* Timeout or error — drop this client */
            return -1;
        }

        ssize_t n = read(fd, buf + total, (size_t)(MAX_REQUEST_SIZE - total));
        if (n <= 0) return -1;  /* Connection closed or error */
        total += n;
        buf[total] = '\0';

        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (total >= MAX_REQUEST_SIZE && !strstr(buf, "\r\n\r\n")) {
        return -1;  /* Request too large */
    }

    /* Parse request line: METHOD /path HTTP/1.x\r\n */
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;

    char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1) return -1;

    size_t method_len = (size_t)(sp1 - buf);
    if (method_len >= sizeof(req->method)) return -1;
    memcpy(req->method, buf, method_len);
    req->method[method_len] = '\0';

    char *path_start = sp1 + 1;
    char *sp2 = memchr(path_start, ' ', (size_t)(line_end - path_start));
    if (!sp2) return -1;

    size_t path_len = (size_t)(sp2 - path_start);
    if (path_len >= sizeof(req->path)) return -1;
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';

    /* Split path and query */
    char *qmark = strchr(req->path, '?');
    if (qmark) {
        *qmark = '\0';
        snprintf(req->query, sizeof(req->query), "%s", qmark + 1);
    }

    /* Scan for Last-Event-ID header */
    const char *hdr = "Last-Event-ID:";
    char *found = strcasestr(buf, hdr);
    if (found && found < buf + total) {
        found += strlen(hdr);
        while (*found == ' ') found++;
        char *endptr;
        errno = 0;
        long val = strtol(found, &endptr, 10);
        if (errno == 0 && endptr != found) req->last_event_id = (int)val;
    }

    /* Scan for Content-Length header */
    const char *cl_hdr = "Content-Length:";
    char *cl_found = strcasestr(buf, cl_hdr);
    if (cl_found && cl_found < buf + total) {
        cl_found += strlen(cl_hdr);
        while (*cl_found == ' ') cl_found++;
        char *endptr;
        errno = 0;
        long val = strtol(cl_found, &endptr, 10);
        if (errno == 0 && endptr != cl_found) req->content_length = (int)val;
        if (req->content_length < 0) req->content_length = 0;
        if (req->content_length > MAX_REQUEST_SIZE) req->content_length = MAX_REQUEST_SIZE;
    }

    /* Extract body (data after \r\n\r\n) */
    req->body_len = 0;
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int body_available = (int)(total - (body_start - buf));
        if (body_available > 0) {
            int to_copy = body_available;
            if (to_copy > MAX_REQUEST_SIZE) to_copy = MAX_REQUEST_SIZE;
            memcpy(req->body, body_start, (size_t)to_copy);
            req->body[to_copy] = '\0';
            req->body_len = to_copy;
        }

        /* If Content-Length says there's more body to read */
        if (req->content_length > 0 && req->body_len < req->content_length) {
            int remaining = req->content_length - req->body_len;
            if (req->body_len + remaining > MAX_REQUEST_SIZE)
                remaining = MAX_REQUEST_SIZE - req->body_len;
            while (remaining > 0) {
                struct pollfd pfd = {.fd = fd, .events = POLLIN};
                int poll_rc = poll(&pfd, 1, 5000);
                if (poll_rc <= 0) break;  /* Timeout or error */

                ssize_t n = read(fd, req->body + req->body_len,
                                 (size_t)remaining);
                if (n <= 0) break;
                req->body_len += (int)n;
                remaining -= (int)n;
            }
            req->body[req->body_len] = '\0';
        }
    }

    return 0;
}

/* ================================================================
 * HTTP Response Helpers
 * ================================================================ */

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type,
                          const char *body, size_t body_len) {
    int hdr_n = dprintf(fd,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    if (hdr_n < 0) return;

    if (body && body_len > 0) {
        /* Write in chunks to handle partial writes */
        size_t written = 0;
        while (written < body_len) {
            ssize_t n = write(fd, body + written, body_len - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
    }
}

static void send_404(int fd) {
    const char *body = "{\"error\":\"Not Found\"}";
    send_response(fd, 404, "Not Found", "application/json",
                  body, strlen(body));
}

static void send_503(int fd) {
    const char *body = "{\"error\":\"Too Many SSE Clients\"}";
    send_response(fd, 503, "Service Unavailable", "application/json",
                  body, strlen(body));
}

/* ================================================================
 * Route: GET /
 * ================================================================ */

static void serve_html(int fd) {
    send_response(fd, 200, "OK", "text/html; charset=utf-8",
                  asset_index_html, strlen(asset_index_html));
}

/* ================================================================
 * Route: GET /api/messages
 * ================================================================ */

static void serve_json(int fd, const http_request_t *req) {
    int since = parse_query_int(req->query, "since", -1);
    int last = parse_query_int(req->query, "last", -1);

    chat_state_t state;
    if (chat_read(g_chat_path, &state) != 0) {
        const char *err = "{\"error\":\"Failed to read chat file\"}";
        send_response(fd, 500, "Internal Server Error", "application/json",
                      err, strlen(err));
        return;
    }

    /* Determine message range */
    int start = 0;
    if (since >= 0 && since < state.message_count) {
        start = since + 1;
    }
    if (last > 0 && (state.message_count - last) > start) {
        start = state.message_count - last;
    }

    /* Build JSON response */
    char *json = malloc(MAX_JSON_BUF);
    if (!json) {
        chat_state_free(&state);
        const char *err = "{\"error\":\"Out of memory\"}";
        send_response(fd, 500, "Internal Server Error", "application/json",
                      err, strlen(err));
        return;
    }

    int pos = 0;
    pos += snprintf(json + pos, MAX_JSON_BUF - (size_t)pos,
                    "{\"messages\":[");

    int first = 1;
    for (int i = start; i < state.message_count; i++) {
        if (!first) {
            if (pos < MAX_JSON_BUF - 1) json[pos++] = ',';
        }
        first = 0;

        /* Write directly into remaining space in json buffer */
        size_t remaining = MAX_JSON_BUF - (size_t)pos - 1;
        int mlen = json_message(&state.messages[i], i,
                                json + pos, remaining);
        if (mlen > 0) {
            pos += mlen;
        }
    }

    pos += snprintf(json + pos, MAX_JSON_BUF - (size_t)pos,
                    "],\"total_count\":%d,\"participants\":[",
                    state.message_count);

    for (int i = 0; i < state.participant_count; i++) {
        char handle_esc[MAX_HANDLE_LEN * 6 + 1];
        int esc_rc = json_escape(state.participants[i].handle,
                    handle_esc, sizeof(handle_esc));
        if (esc_rc < 0) continue; /* skip participant with too-long handle */
        pos += snprintf(json + pos, MAX_JSON_BUF - (size_t)pos,
                        "%s{\"handle\":\"%s\",\"count\":%d}",
                        i > 0 ? "," : "",
                        handle_esc, state.participants[i].count);
    }

    pos += snprintf(json + pos, MAX_JSON_BUF - (size_t)pos, "]}");

    send_response(fd, 200, "OK", "application/json", json, (size_t)pos);

    free(json);
    chat_state_free(&state);
}

/* ================================================================
 * Route: POST /api/send
 * ================================================================ */

/*
 * json_extract_string — Extract a string value from a JSON object.
 *
 * Minimal JSON parser: finds "key":"value" and copies value to out_buf.
 * Handles \" escapes within values. Does NOT handle nested objects.
 * Returns 0 on success, -1 if key not found or value too long.
 */
static int json_extract_string(const char *json, const char *key,
                                char *out_buf, size_t out_size) {
    /* Build search pattern: "key":" */
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return -1;

    const char *start = strstr(json, pattern);
    if (!start) return -1;

    start += plen;

    /* Extract value until unescaped " */
    size_t pos = 0;
    const char *p = start;
    while (*p != '\0' && pos < out_size - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            /* Handle escape sequences */
            p++;
            switch (*p) {
                case '"':  out_buf[pos++] = '"'; break;
                case '\\': out_buf[pos++] = '\\'; break;
                case 'n':  out_buf[pos++] = '\n'; break;
                case 'r':  out_buf[pos++] = '\r'; break;
                case 't':  out_buf[pos++] = '\t'; break;
                default:   out_buf[pos++] = *p; break;
            }
            p++;
        } else if (*p == '"') {
            break;  /* End of value */
        } else {
            out_buf[pos++] = *p++;
        }
    }
    out_buf[pos] = '\0';
    return (*p == '"') ? 0 : -1;
}

static void serve_send(int fd, const http_request_t *req) {
    if (req->body_len <= 0) {
        const char *err = "{\"error\":\"Empty request body\"}";
        send_response(fd, 400, "Bad Request", "application/json",
                      err, strlen(err));
        return;
    }

    /* Extract handle and message from JSON body */
    char handle[MAX_HANDLE_LEN];
    char message[MAX_REQUEST_SIZE];

    if (json_extract_string(req->body, "handle", handle, sizeof(handle)) != 0) {
        const char *err = "{\"error\":\"Missing 'handle' field\"}";
        send_response(fd, 400, "Bad Request", "application/json",
                      err, strlen(err));
        return;
    }

    if (json_extract_string(req->body, "message", message, sizeof(message)) != 0) {
        const char *err = "{\"error\":\"Missing 'message' field\"}";
        send_response(fd, 400, "Bad Request", "application/json",
                      err, strlen(err));
        return;
    }

    if (handle[0] == '\0') {
        const char *err = "{\"error\":\"Handle must not be empty\"}";
        send_response(fd, 400, "Bad Request", "application/json",
                      err, strlen(err));
        return;
    }

    if (message[0] == '\0') {
        const char *err = "{\"error\":\"Message must not be empty\"}";
        send_response(fd, 400, "Bad Request", "application/json",
                      err, strlen(err));
        return;
    }

    /* Send via chat_send (with locking, header updates, etc.) */
    int rc = chat_send(g_chat_path, handle, message);
    if (rc < 0) {
        const char *err = "{\"error\":\"Failed to send message\"}";
        send_response(fd, 500, "Internal Server Error", "application/json",
                      err, strlen(err));
        return;
    }

    /* Publish bus events (non-fatal) */
    (void)bus_bridge_after_send(g_chat_path, handle, message);

    const char *ok = "{\"ok\":true}";
    send_response(fd, 200, "OK", "application/json", ok, strlen(ok));
}

/* ================================================================
 * SSE Helpers
 * ================================================================ */

static int send_sse_event(int fd, const char *event, int id,
                          const char *data) {
    int n = dprintf(fd, "id: %d\nevent: %s\ndata: %s\n\n", id, event, data);
    return (n > 0) ? 0 : -1;
}

static int send_sse_heartbeat(int fd) {
    int n = dprintf(fd, ": heartbeat\n\n");
    return (n > 0) ? 0 : -1;
}

static int send_sse_message_event(int fd, int index,
                                  const chat_message_t *msg) {
    /* Heap-allocate buffer to avoid stack overflow under ASan.
     * Actual messages are typically <4KB; MAX_MESSAGE_LEN is 1MB worst case. */
    size_t buf_size = (msg->content_len + 1) * 6 + 512;
    if (buf_size > MAX_JSON_BUF) buf_size = MAX_JSON_BUF;
    char *buf = malloc(buf_size);
    if (!buf) return -1;

    int mlen = json_message(msg, index, buf, buf_size);
    int result = -1;
    if (mlen >= 0) {
        result = send_sse_event(fd, "message", index, buf);
    }
    free(buf);
    return result;
}

/* ================================================================
 * Server Loop
 * ================================================================ */

static void server_loop(int listen_fd) {
    sse_client_t sse_clients[MAX_SSE_CLIENTS];
    int sse_count = 0;
    int last_known_count = 0;
    time_t last_heartbeat = time(NULL);

    /* Initial read to establish baseline */
    {
        chat_state_t state;
        if (chat_read(g_chat_path, &state) == 0) {
            last_known_count = state.message_count;
            chat_state_free(&state);
        }
    }

    while (!g_quit) {
        /* Build poll set: [0] = listen socket, [1..n] = SSE clients */
        struct pollfd fds[MAX_SSE_CLIENTS + 1];
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;

        for (int i = 0; i < sse_count; i++) {
            fds[i + 1].fd = sse_clients[i].fd;
            fds[i + 1].events = 0;  /* Only watch for errors/hangup */
        }

        int nfds = 1 + sse_count;
        int ready = poll(fds, (nfds_t)nfds, POLL_TIMEOUT_MS);

        if (ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "poll error: %s\n", strerror(errno));
            break;
        }

        /* Check for disconnected SSE clients */
        for (int i = sse_count - 1; i >= 0; i--) {
            if (fds[i + 1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                close(sse_clients[i].fd);
                /* Remove by swapping with last */
                sse_clients[i] = sse_clients[sse_count - 1];
                sse_count--;
            }
        }

        /* Handle new connections */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in addr;
            socklen_t addrlen = sizeof(addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&addr,
                                   &addrlen);
            if (client_fd < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    fprintf(stderr, "accept error: %s\n", strerror(errno));
            } else {
                http_request_t req;
                if (parse_request(client_fd, &req) == 0) {
                    if (strcmp(req.method, "POST") == 0 &&
                        strcmp(req.path, "/api/send") == 0) {
                        serve_send(client_fd, &req);
                        close(client_fd);
                    } else if (strcmp(req.method, "GET") != 0) {
                        const char *body = "{\"error\":\"Method Not Allowed\"}";
                        send_response(client_fd, 405, "Method Not Allowed",
                                      "application/json",
                                      body, strlen(body));
                        close(client_fd);
                    } else if (strcmp(req.path, "/") == 0) {
                        serve_html(client_fd);
                        close(client_fd);
                    } else if (strcmp(req.path, "/api/messages") == 0) {
                        serve_json(client_fd, &req);
                        close(client_fd);
                    } else if (strcmp(req.path, "/events") == 0) {
                        /* SSE connection */
                        if (sse_count >= MAX_SSE_CLIENTS) {
                            send_503(client_fd);
                            close(client_fd);
                        } else {
                            /* Send SSE headers */
                            dprintf(client_fd,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Cache-Control: no-cache\r\n"
                                "Connection: keep-alive\r\n"
                                "\r\n");

                            /* Send initial messages */
                            chat_state_t state;
                            int start_idx = 0;
                            if (req.last_event_id >= 0) {
                                start_idx = req.last_event_id + 1;
                            } else if (g_initial_last > 0) {
                                /* Use --last=N for initial view */
                                chat_state_t init_state;
                                if (chat_read(g_chat_path, &init_state) == 0) {
                                    if (init_state.message_count > g_initial_last)
                                        start_idx = init_state.message_count - g_initial_last;
                                    chat_state_free(&init_state);
                                }
                            }

                            int last_sent = -1;
                            if (chat_read(g_chat_path, &state) == 0) {
                                for (int i = start_idx;
                                     i < state.message_count; i++) {
                                    if (send_sse_message_event(
                                            client_fd, i,
                                            &state.messages[i]) < 0) {
                                        close(client_fd);
                                        chat_state_free(&state);
                                        goto next_poll;
                                    }
                                }
                                last_sent = state.message_count - 1;
                                chat_state_free(&state);
                            }

                            /* Register SSE client */
                            sse_clients[sse_count].fd = client_fd;
                            sse_clients[sse_count].last_sent_index = last_sent;
                            sse_clients[sse_count].alive = 1;
                            sse_count++;
                        }
                    } else {
                        send_404(client_fd);
                        close(client_fd);
                    }
                } else {
                    close(client_fd);
                }
            }
        }

next_poll:
        /* Check for new messages and push to SSE clients */
        if (sse_count > 0) {
            chat_state_t state;
            if (chat_read(g_chat_path, &state) == 0) {
                if (state.message_count > last_known_count) {
                    for (int i = last_known_count;
                         i < state.message_count; i++) {
                        for (int c = sse_count - 1; c >= 0; c--) {
                            if (sse_clients[c].alive &&
                                i > sse_clients[c].last_sent_index) {
                                if (send_sse_message_event(
                                        sse_clients[c].fd, i,
                                        &state.messages[i]) < 0) {
                                    sse_clients[c].alive = 0;
                                    close(sse_clients[c].fd);
                                    sse_clients[c] =
                                        sse_clients[sse_count - 1];
                                    sse_count--;
                                } else {
                                    sse_clients[c].last_sent_index = i;
                                }
                            }
                        }
                    }
                    last_known_count = state.message_count;
                }
                chat_state_free(&state);
            }
        }

        /* SSE heartbeat */
        time_t now = time(NULL);
        if (now - last_heartbeat >= HEARTBEAT_INTERVAL && sse_count > 0) {
            for (int c = sse_count - 1; c >= 0; c--) {
                if (send_sse_heartbeat(sse_clients[c].fd) < 0) {
                    sse_clients[c].alive = 0;
                    close(sse_clients[c].fd);
                    sse_clients[c] = sse_clients[sse_count - 1];
                    sse_count--;
                }
            }
            last_heartbeat = now;
        }
    }

    /* Cleanup: close all SSE clients */
    for (int i = 0; i < sse_count; i++) {
        close(sse_clients[i].fd);
    }
}

/* ================================================================
 * Argument Parsing
 * ================================================================ */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <file> [options]\n"
        "\n"
        "  <file>        Path to chat file (must exist)\n"
        "\n"
        "Options:\n"
        "  --port=N      Port to listen on (default: %d)\n"
        "  --bind=ADDR   Address to bind to (default: %s)\n"
        "  --last=N      Initial messages to show (default: all)\n"
        "  --help        Show this help\n"
        "\n"
        "Example:\n"
        "  %s /path/to/chat.chat --port=3000\n"
        "  # Then open http://localhost:3000 in browser\n",
        prog, DEFAULT_PORT, DEFAULT_BIND, prog);
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *bind_addr = DEFAULT_BIND;

    if (argc < 2) {
        print_usage(argv[0]);
        return 4;
    }

    /* First non-option argument is the chat file */
    g_chat_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 7, &endptr, 10);
            if (errno != 0 || *endptr != '\0') {
                fprintf(stderr, "Invalid --port value: %s\n", argv[i] + 7);
                return 4;
            }
            port = (int)val;
            if (port < 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i] + 7);
                return 4;
            }
        } else if (strncmp(argv[i], "--bind=", 7) == 0) {
            bind_addr = argv[i] + 7;
        } else if (strncmp(argv[i], "--last=", 7) == 0) {
            char *endptr;
            errno = 0;
            long val = strtol(argv[i] + 7, &endptr, 10);
            if (errno != 0 || *endptr != '\0') {
                fprintf(stderr, "Invalid --last value: %s\n", argv[i] + 7);
                return 4;
            }
            g_initial_last = (int)val;
            if (g_initial_last < 0) g_initial_last = -1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 4;
        } else {
            if (g_chat_path == NULL) {
                g_chat_path = argv[i];
            } else {
                fprintf(stderr, "Multiple chat files specified\n");
                return 4;
            }
        }
    }

    if (g_chat_path == NULL) {
        fprintf(stderr, "No chat file specified\n");
        print_usage(argv[0]);
        return 4;
    }

    /* Verify chat file exists */
    struct stat st;
    if (stat(g_chat_path, &st) != 0) {
        fprintf(stderr, "Chat file not found: %s\n", g_chat_path);
        return 2;
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "warning: sigaction(SIGINT) failed: %s\n", strerror(errno));
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "warning: sigaction(SIGTERM) failed: %s\n", strerror(errno));
    }

    /* Ignore SIGPIPE (broken SSE connections) */
    signal(SIGPIPE, SIG_IGN);

    /* Create server socket — try IPv6 dual-stack first, fall back to IPv4 */
    int listen_fd = -1;
    int is_ipv6 = 0;

    /* Try IPv6 */
    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons((uint16_t)port);

    if (inet_pton(AF_INET6, bind_addr, &addr6.sin6_addr) == 1) {
        listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (listen_fd >= 0) {
            int opt = 1;
            if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
                fprintf(stderr, "warning: setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
            }
            /* IPV6_V6ONLY=1: only accept IPv6 connections (match bind address) */
            if (setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) != 0) {
                fprintf(stderr, "warning: setsockopt(IPV6_V6ONLY) failed: %s\n", strerror(errno));
            }

            if (bind(listen_fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0) {
                close(listen_fd);
                listen_fd = -1;
            } else {
                is_ipv6 = 1;
            }
        }
    }

    /* Fall back to IPv4 */
    if (listen_fd < 0) {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((uint16_t)port);

        if (inet_pton(AF_INET, bind_addr, &addr4.sin_addr) != 1) {
            fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
            return 4;
        }

        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            fprintf(stderr, "socket: %s\n", strerror(errno));
            return 1;
        }

        int opt = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
            fprintf(stderr, "warning: setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
        }

        if (bind(listen_fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0) {
            fprintf(stderr, "bind(%s:%d): %s\n", bind_addr, port,
                    strerror(errno));
            close(listen_fd);
            return 1;
        }
    }

    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        close(listen_fd);
        return 1;
    }

    /* Get actual port (for port=0 case) */
    int actual_port = port;
    if (is_ipv6) {
        socklen_t addrlen = sizeof(addr6);
        if (getsockname(listen_fd, (struct sockaddr *)&addr6, &addrlen) != 0) {
            fprintf(stderr, "warning: getsockname failed: %s\n", strerror(errno));
        } else {
            actual_port = ntohs(addr6.sin6_port);
        }
    } else {
        struct sockaddr_in addr4;
        socklen_t addrlen = sizeof(addr4);
        if (getsockname(listen_fd, (struct sockaddr *)&addr4, &addrlen) != 0) {
            fprintf(stderr, "warning: getsockname failed: %s\n", strerror(errno));
        } else {
            actual_port = ntohs(addr4.sin_port);
        }
    }

    /* Print URL with appropriate address format */
    if (is_ipv6) {
        fprintf(stdout, "nbs-chat-web serving %s on http://[%s]:%d/\n",
                g_chat_path, bind_addr, actual_port);
    } else {
        fprintf(stdout, "nbs-chat-web serving %s on http://%s:%d/\n",
                g_chat_path, bind_addr, actual_port);
    }
    fflush(stdout);

    server_loop(listen_fd);

    close(listen_fd);
    fprintf(stdout, "\nnbs-chat-web stopped.\n");
    return 0;
}
