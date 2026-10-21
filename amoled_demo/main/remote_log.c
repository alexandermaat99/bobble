#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "remote_log.h"
#include "spotify.h"
#include "secrets.h"
#include "agent.h"

#define RING (16 * 1024)
static char *ring;
static size_t head, used;
static vprintf_like_t serial_out;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Every log line still goes to the USB serial port, and a copy lands in the ring buffer
static int tee(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (ring && n > 0) {
        taskENTER_CRITICAL(&mux);
        for (int i = 0; i < n; i++) {
            if (line[i] == '\033') continue;          // drop colour codes
            ring[head] = line[i];
            head = (head + 1) % RING;
            if (used < RING) used++;
        }
        taskEXIT_CRITICAL(&mux);
    }
    return serial_out(fmt, ap);
}

void remote_log_start(void)
{
    ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM);
    serial_out = esp_log_set_vprintf(tee);
}

static bool token_ok(httpd_req_t *req)
{
    if (agent_token_ok(req)) return true;            // (the paired Mac, or the development token)
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "bad token\n");
    return false;
}

static esp_err_t get_log(httpd_req_t *req)
{
    if (!token_ok(req)) return ESP_OK;
    EXT_RAM_BSS_ATTR static char out[RING];
    taskENTER_CRITICAL(&mux);
    size_t n = used, start = (head + RING - used) % RING;
    for (size_t i = 0; i < n; i++) out[i] = ring[(start + i) % RING];
    taskEXIT_CRITICAL(&mux);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, out, n);
}

// For testing Spotify from the Mac: POST /spotify/cmd?c=<number>
static esp_err_t post_cmd(httpd_req_t *req)
{
    if (!token_ok(req)) return ESP_OK;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "c", v, sizeof v) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?c=");
    spotify_command(atoi(v));
    return httpd_resp_sendstr(req, "queued\n");
}

void remote_log_register(httpd_handle_t server)
{
    const httpd_uri_t a = {.uri = "/log", .method = HTTP_GET, .handler = get_log};
    const httpd_uri_t b = {.uri = "/spotify/cmd", .method = HTTP_POST, .handler = post_cmd};
    httpd_register_uri_handler(server, &a);
    httpd_register_uri_handler(server, &b);
}
