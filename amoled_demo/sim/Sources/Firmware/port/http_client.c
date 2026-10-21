// ESP-IDF's HTTP client on libcurl. perform() streams headers and data to the event handler as they come, as on the
// board. open()/fetch_headers()/read() fetch the whole response at open and hand it out in pieces (the firmware only
// reads downloads that way).
#include <curl/curl.h>
#include <pthread.h>
#include <strings.h>
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "http_client";

struct sim_http_client {
    CURL *curl;
    char *url;
    esp_http_client_method_t method;
    struct curl_slist *headers;
    char *post;
    int post_len;
    int timeout_ms;
    bool follow;
    http_event_handle_cb handler;
    void *user_data;
    int status;
    char *body;                                       // open/read: the whole response
    size_t body_len, body_cap, read_at;
    bool buffering, complete;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    pthread_once(&once, global_init);
    struct sim_http_client *c = calloc(1, sizeof *c);
    c->curl = curl_easy_init();
    c->url = strdup(cfg->url ? cfg->url : "");
    c->method = cfg->method;
    c->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 5000;
    c->follow = !cfg->disable_auto_redirect;
    c->handler = cfg->event_handler;
    c->user_data = cfg->user_data;
    return c;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    free(c->url);
    c->url = strdup(url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) { c->method = m; return ESP_OK; }

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key)
{
    struct curl_slist *kept = NULL;
    size_t kl = strlen(key);
    for (struct curl_slist *h = c->headers; h; h = h->next)
        if (strncasecmp(h->data, key, kl) || h->data[kl] != ':') kept = curl_slist_append(kept, h->data);
    curl_slist_free_all(c->headers);
    c->headers = kept;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    esp_http_client_delete_header(c, key);            // (setting one again replaces it)
    char line[2048];
    snprintf(line, sizeof line, "%s: %s", key, value);
    c->headers = curl_slist_append(c->headers, line);
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len)
{
    free(c->post);
    c->post = NULL;
    c->post_len = 0;
    if (data && len > 0) {
        c->post = malloc(len);
        memcpy(c->post, data, len);
        c->post_len = len;
    }
    return ESP_OK;
}

static void event(esp_http_client_handle_t c, esp_http_client_event_id_t id, void *data, int len, char *k, char *v)
{
    if (!c->handler) return;
    esp_http_client_event_t e = {.event_id = id, .client = c, .data = data, .data_len = len, .user_data = c->user_data, .header_key = k, .header_value = v};
    c->handler(&e);
}

static size_t on_data(char *p, size_t size, size_t n, void *ud)
{
    esp_http_client_handle_t c = ud;
    size_t len = size * n;
    if (c->buffering) {
        if (c->body_len + len > c->body_cap) {
            c->body_cap = (c->body_len + len) * 2;
            c->body = realloc(c->body, c->body_cap);
        }
        memcpy(c->body + c->body_len, p, len);
        c->body_len += len;
    } else event(c, HTTP_EVENT_ON_DATA, p, (int)len, NULL, NULL);
    return len;
}

static size_t on_header(char *p, size_t size, size_t n, void *ud)
{
    esp_http_client_handle_t c = ud;
    size_t len = size * n;
    char line[2048];
    if (len >= sizeof line) return len;
    memcpy(line, p, len);
    line[len] = 0;
    if (!strncmp(line, "HTTP/", 5)) return len;       // (a status line: a redirect's or the final one)
    char *colon = strchr(line, ':');
    if (!colon) return len;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ') v++;
    for (char *e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
    event(c, HTTP_EVENT_ON_HEADER, NULL, 0, line, v);
    return len;
}

static esp_err_t run(esp_http_client_handle_t c)
{
    static const char *const names[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    CURL *k = c->curl;
    curl_easy_setopt(k, CURLOPT_URL, c->url);
    curl_easy_setopt(k, CURLOPT_HTTPHEADER, c->headers);
    curl_easy_setopt(k, CURLOPT_TIMEOUT_MS, (long)c->timeout_ms);
    curl_easy_setopt(k, CURLOPT_FOLLOWLOCATION, c->follow ? 1L : 0L);
    curl_easy_setopt(k, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(k, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(k, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(k, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(k, CURLOPT_HEADERDATA, c);
    curl_easy_setopt(k, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(k, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(k, CURLOPT_NOBODY, c->method == HTTP_METHOD_HEAD ? 1L : 0L);
    if (c->post || c->method == HTTP_METHOD_POST || c->method == HTTP_METHOD_PUT || c->method == HTTP_METHOD_PATCH) {
        curl_easy_setopt(k, CURLOPT_POSTFIELDS, c->post ? c->post : "");
        curl_easy_setopt(k, CURLOPT_POSTFIELDSIZE, (long)c->post_len);
    } else curl_easy_setopt(k, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(k, CURLOPT_CUSTOMREQUEST, c->method == HTTP_METHOD_GET || c->method == HTTP_METHOD_HEAD ? NULL : names[c->method]);
    CURLcode r = curl_easy_perform(k);
    long code = 0;
    curl_easy_getinfo(k, CURLINFO_RESPONSE_CODE, &code);
    c->status = (int)code;
    if (r != CURLE_OK) {
        ESP_LOGW(TAG, "%s: %s", c->url, curl_easy_strerror(r));
        event(c, HTTP_EVENT_ERROR, NULL, 0, NULL, NULL);
        return ESP_FAIL;
    }
    event(c, HTTP_EVENT_ON_FINISH, NULL, 0, NULL, NULL);
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    c->buffering = false;
    return run(c);
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    c->buffering = true;
    c->body_len = c->read_at = 0;
    c->complete = false;
    esp_err_t r = run(c);
    c->complete = r == ESP_OK;
    return r;
}

int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len) { return -1; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { return (int64_t)c->body_len; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { return (int64_t)c->body_len; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->complete && c->read_at == c->body_len; }

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    size_t n = c->body_len - c->read_at < (size_t)len ? c->body_len - c->read_at : (size_t)len;
    memcpy(buf, c->body + c->read_at, n);
    c->read_at += n;
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c) { return ESP_OK; }

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    if (!c) return ESP_OK;
    curl_easy_cleanup(c->curl);
    curl_slist_free_all(c->headers);
    free(c->url);
    free(c->post);
    free(c->body);
    free(c);
    return ESP_OK;
}
