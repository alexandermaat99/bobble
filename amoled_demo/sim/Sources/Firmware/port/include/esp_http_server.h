#pragma once
// ESP-IDF's web server on a real socket: one connection at a time, handlers run on the server's thread (as on the board)
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef void *httpd_handle_t;
typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4, HTTP_OPTIONS = 6, HTTP_PATCH = 28 } httpd_method_t;
typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[CONFIG_HTTPD_MAX_URI_LEN + 1];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;
typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 0, HTTPD_501_METHOD_NOT_IMPLEMENTED, HTTPD_505_VERSION_NOT_SUPPORTED, HTTPD_400_BAD_REQUEST,
    HTTPD_401_UNAUTHORIZED, HTTPD_403_FORBIDDEN, HTTPD_404_NOT_FOUND, HTTPD_405_METHOD_NOT_ALLOWED, HTTPD_408_REQ_TIMEOUT,
    HTTPD_411_LENGTH_REQUIRED, HTTPD_414_URI_TOO_LONG, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, HTTPD_ERR_CODE_MAX
} httpd_err_code_t;
typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *r, httpd_err_code_t e);
typedef struct {
    unsigned task_priority;
    size_t stack_size;
    BaseType_t core_id;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    void *uri_match_fn;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() { .task_priority = 5, .stack_size = 4096, .server_port = 80, .ctrl_port = 32768, \
    .max_open_sockets = 7, .max_uri_handlers = 8, .max_resp_headers = 8, .backlog_conn = 5, .recv_wait_timeout = 5, .send_wait_timeout = 5 }
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_200 "200 OK"
#define HTTPD_204 "204 No Content"
#define HTTPD_400 "400 Bad Request"
#define HTTPD_404 "404 Not Found"
#define HTTPD_500 "500 Internal Server Error"
#define HTTPD_TYPE_JSON "application/json"
#define HTTPD_TYPE_TEXT "text/html"
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg);
esp_err_t httpd_stop(httpd_handle_t h);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u);
esp_err_t httpd_register_err_handler(httpd_handle_t h, httpd_err_code_t e, httpd_err_handler_func_t f);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t len);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *q, const char *key, char *val, size_t len);
int httpd_req_to_sockfd(httpd_req_t *r);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send(r, s, s ? HTTPD_RESP_USE_STRLEN : 0); }
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s) { return httpd_resp_send_chunk(r, s, s ? HTTPD_RESP_USE_STRLEN : 0); }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *msg);
