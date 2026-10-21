// ESP-IDF's web server on a real socket. Like the board's, one thread serves one request at a time and runs the
// handlers itself. Each connection carries one request; the response is collected and sent when the handler returns.
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <pthread.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "sim_port.h"

static const char *TAG = "httpd";

#define MAX_URIS 40
#define MAX_HDRS 32

typedef struct {
    int listen_fd, port;
    httpd_uri_t uris[MAX_URIS];
    int n_uris;
    httpd_err_handler_func_t err[HTTPD_ERR_CODE_MAX];
} server_t;

// What a request carries besides httpd_req_t's own fields (req->aux)
typedef struct {
    int fd;
    char *hdr_names[MAX_HDRS], *hdr_vals[MAX_HDRS];
    int n_hdrs;
    char query[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char pre[8192];                                   // body bytes that came in with the headers
    int pre_len, pre_at;
    size_t body_left;
    char status[48], type[64];
    char *extra_hdrs;                                 // "Name: value\r\n"...
    char *out;                                        // the response body so far
    size_t out_len, out_cap;
    bool chunked, sent;
} aux_t;

static int http_port = 8466;
int sim_http_port(void) { return http_port; }
void sim_set_http_port(int p) { http_port = p; }

// ---- the request
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    aux_t *a = r->aux;
    if (!a->body_left) return 0;
    if (len > a->body_left) len = a->body_left;
    int n;
    if (a->pre_at < a->pre_len) {
        n = a->pre_len - a->pre_at < (int)len ? a->pre_len - a->pre_at : (int)len;
        memcpy(buf, a->pre + a->pre_at, n);
        a->pre_at += n;
    } else {
        n = (int)recv(a->fd, buf, len, 0);
        if (n < 0) return HTTPD_SOCK_ERR_TIMEOUT;
        if (n == 0) return HTTPD_SOCK_ERR_FAIL;
    }
    a->body_left -= n;
    return n;
}

static const char *hdr(httpd_req_t *r, const char *field)
{
    aux_t *a = r->aux;
    for (int i = 0; i < a->n_hdrs; i++) if (!strcasecmp(a->hdr_names[i], field)) return a->hdr_vals[i];
    return NULL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    const char *v = hdr(r, field);
    return v ? strlen(v) : 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t len)
{
    const char *v = hdr(r, field);
    if (!v) return ESP_ERR_NOT_FOUND;
    strlcpy(val, v, len);
    return strlen(v) >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r) { return strlen(((aux_t *)r->aux)->query); }

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    aux_t *a = r->aux;
    if (!a->query[0]) return ESP_ERR_NOT_FOUND;
    strlcpy(buf, a->query, len);
    return strlen(a->query) >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

// key=value&... (no decoding, as on the board)
esp_err_t httpd_query_key_value(const char *q, const char *key, char *val, size_t len)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = end - (p + kl + 1);
            strlcpy(val, p + kl + 1, n + 1 < len ? n + 1 : len);
            return n >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
        }
        p = amp ? amp + 1 : NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_to_sockfd(httpd_req_t *r) { return ((aux_t *)r->aux)->fd; }

// ---- the response
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { strlcpy(((aux_t *)r->aux)->status, s, 48); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { strlcpy(((aux_t *)r->aux)->type, t, 64); return ESP_OK; }

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    aux_t *a = r->aux;
    size_t old = a->extra_hdrs ? strlen(a->extra_hdrs) : 0, add = strlen(field) + strlen(value) + 5;
    a->extra_hdrs = realloc(a->extra_hdrs, old + add);
    snprintf(a->extra_hdrs + old, add, "%s: %s\r\n", field, value);
    return ESP_OK;
}

static void append(aux_t *a, const char *buf, size_t len)
{
    if (a->out_len + len > a->out_cap) {
        a->out_cap = (a->out_len + len) * 2 + 256;
        a->out = realloc(a->out, a->out_cap);
    }
    memcpy(a->out + a->out_len, buf, len);
    a->out_len += len;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    aux_t *a = r->aux;
    if (buf) append(a, buf, len == HTTPD_RESP_USE_STRLEN ? strlen(buf) : (size_t)len);
    a->sent = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    aux_t *a = r->aux;
    a->chunked = true;
    if (!buf || !len) { a->sent = true; return ESP_OK; }   // (the last chunk)
    append(a, buf, len == HTTPD_RESP_USE_STRLEN ? strlen(buf) : (size_t)len);
    return ESP_OK;
}

static const char *const err_status[] = {
    "500 Internal Server Error", "501 Method Not Implemented", "505 Version Not Supported", "400 Bad Request", "401 Unauthorized",
    "403 Forbidden", "404 Not Found", "405 Method Not Allowed", "408 Request Timeout", "411 Length Required", "414 URI Too Long",
    "431 Request Header Fields Too Large",
};

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *msg)
{
    httpd_resp_set_status(r, err_status[e < HTTPD_ERR_CODE_MAX ? e : 0]);
    httpd_resp_set_type(r, "text/plain");
    aux_t *a = r->aux;
    a->out_len = 0;
    return httpd_resp_send(r, msg ? msg : err_status[e] + 4, HTTPD_RESP_USE_STRLEN);
}

static void flush(aux_t *a)
{
    char head[1024];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
                     a->status, a->type, a->out_len, a->extra_hdrs ? a->extra_hdrs : "");
    send(a->fd, head, n, 0);
    for (size_t at = 0; at < a->out_len;) {
        ssize_t k = send(a->fd, a->out + at, a->out_len - at, 0);
        if (k <= 0) break;
        at += k;
    }
}

// ---- serving
static bool read_head(int fd, char *buf, int cap, int *len, char **body)
{
    *len = 0;
    while (*len < cap - 1) {
        ssize_t n = recv(fd, buf + *len, cap - 1 - *len, 0);
        if (n <= 0) return false;
        *len += n;
        buf[*len] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) { *body = e + 4; return true; }
    }
    return false;
}

static void serve(server_t *s, int fd)
{
    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    static char buf[16384];
    int len;
    char *body;
    if (!read_head(fd, buf, sizeof buf, &len, &body)) return;

    aux_t *a = calloc(1, sizeof *a);
    a->fd = fd;
    strcpy(a->status, "200 OK");
    strcpy(a->type, "text/html");
    httpd_req_t *r = calloc(1, sizeof *r);
    r->handle = s;
    r->aux = a;

    char *line_end = strstr(buf, "\r\n");
    *line_end = 0;
    char method[12] = "", target[CONFIG_HTTPD_MAX_URI_LEN + 1] = "";
    sscanf(buf, "%11s %512s", method, target);
    for (char *p = line_end + 2; p < body - 2;) {      // headers
        char *e = strstr(p, "\r\n");
        if (!e) break;
        *e = 0;
        char *colon = strchr(p, ':');
        if (colon && a->n_hdrs < MAX_HDRS) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ') v++;
            a->hdr_names[a->n_hdrs] = strdup(p);
            a->hdr_vals[a->n_hdrs++] = strdup(v);
        }
        p = e + 2;
    }
    const char *cl = hdr(r, "Content-Length");
    r->content_len = cl ? strtoul(cl, NULL, 10) : 0;
    a->body_left = r->content_len;
    a->pre_len = (int)(buf + len - body);
    if (a->pre_len > (int)sizeof a->pre) a->pre_len = sizeof a->pre;
    memcpy(a->pre, body, a->pre_len);

    char *q = strchr(target, '?');
    if (q) { strlcpy(a->query, q + 1, sizeof a->query); *q = 0; }
    strlcpy((char *)r->uri, target, sizeof r->uri);
    static const struct { const char *name; int m; } methods[] = {
        {"GET", HTTP_GET}, {"POST", HTTP_POST}, {"PUT", HTTP_PUT}, {"DELETE", HTTP_DELETE}, {"HEAD", HTTP_HEAD}, {"OPTIONS", HTTP_OPTIONS}, {"PATCH", HTTP_PATCH},
    };
    r->method = -1;
    for (int i = 0; i < 7; i++) if (!strcmp(method, methods[i].name)) r->method = methods[i].m;

    const httpd_uri_t *u = NULL;
    bool path_known = false;
    for (int i = 0; i < s->n_uris; i++) {
        if (strcmp(s->uris[i].uri, r->uri)) continue;
        path_known = true;
        if ((int)s->uris[i].method == r->method) u = &s->uris[i];
    }
    if (u) {
        r->user_ctx = u->user_ctx;
        if (u->handler(r) != ESP_OK && !a->sent) httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    } else {
        httpd_err_code_t e = path_known ? HTTPD_405_METHOD_NOT_ALLOWED : HTTPD_404_NOT_FOUND;
        if (s->err[e]) s->err[e](r, e);
        else httpd_resp_send_err(r, e, NULL);
    }
    flush(a);
    for (int i = 0; i < a->n_hdrs; i++) { free(a->hdr_names[i]); free(a->hdr_vals[i]); }
    free(a->extra_hdrs);
    free(a->out);
    free(a);
    free(r);
}

static void *server_main(void *p)
{
    server_t *s = p;
    pthread_setname_np("httpd");
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        serve(s, fd);
        shutdown(fd, SHUT_WR);
        close(fd);
    }
    return NULL;
}

// Listens on the emulator's port (8466, or the next free one) on this Mac only (no one else on the network should reach
// a development copy that takes the development token)
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg)
{
    server_t *s = calloc(1, sizeof *s);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    bool bound = false;
    for (int p = http_port; p < http_port + 20 && !bound; p++) {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(p), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(s->listen_fd, (struct sockaddr *)&sa, sizeof sa) == 0) { s->port = p; bound = true; }
    }
    if (!bound || listen(s->listen_fd, 8)) { ESP_LOGE(TAG, "can't listen"); close(s->listen_fd); free(s); return ESP_FAIL; }
    http_port = s->port;
    ESP_LOGI(TAG, "listening on port %d", s->port);
    pthread_t th;
    pthread_create(&th, NULL, server_main, s);
    *out = s;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t h) { return ESP_OK; }

esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u)
{
    server_t *s = h;
    if (!s || s->n_uris == MAX_URIS) return ESP_FAIL;
    s->uris[s->n_uris++] = *u;
    return ESP_OK;
}

esp_err_t httpd_register_err_handler(httpd_handle_t h, httpd_err_code_t e, httpd_err_handler_func_t f)
{
    server_t *s = h;
    if (!s || e >= HTTPD_ERR_CODE_MAX) return ESP_FAIL;
    s->err[e] = f;
    return ESP_OK;
}
