#pragma once
// ESP-IDF's HTTP client on libcurl (TLS from macOS)
#include "esp_err.h"
typedef struct sim_http_client *esp_http_client_handle_t;
typedef enum {
    HTTP_EVENT_ERROR = 0, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT
} esp_http_client_event_id_t;
typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *e);
typedef enum {
    HTTP_METHOD_GET = 0, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_PATCH, HTTP_METHOD_DELETE, HTTP_METHOD_HEAD, HTTP_METHOD_MAX
} esp_http_client_method_t;
typedef enum { HTTP_AUTH_TYPE_NONE = 0 } esp_http_client_auth_type_t;
typedef enum { HTTP_TRANSPORT_UNKNOWN = 0, HTTP_TRANSPORT_OVER_TCP, HTTP_TRANSPORT_OVER_SSL } esp_http_client_transport_t;
typedef struct {
    const char *url, *host, *path, *query, *username, *password, *cert_pem, *user_agent;
    int port;
    esp_http_client_auth_type_t auth_type;
    esp_http_client_method_t method;
    int timeout_ms;
    bool disable_auto_redirect;
    int max_redirection_count;
    int max_authorization_retries;
    http_event_handle_cb event_handler;
    esp_http_client_transport_t transport_type;
    int buffer_size;
    int buffer_size_tx;
    void *user_data;
    bool is_async;
    bool use_global_ca_store;
    bool skip_cert_common_name_check;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool keep_alive_enable;
    int keep_alive_idle, keep_alive_interval, keep_alive_count;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
