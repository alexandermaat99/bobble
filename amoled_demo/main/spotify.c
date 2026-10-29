#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "jpeg_decoder.h"
#include "esp_attr.h"
#include "gfx.h"
#include "wifi_setup.h"
#include "spotify.h"
#include "agent.h"
#include "secrets.h"

static const char *TAG = "spotify";

static SemaphoreHandle_t lock;
static QueueHandle_t cmds;
static spotify_state_t st;
static char client_id[40], refresh_token[256], access_token[512];
static int64_t access_expires;
static volatile bool fast, awake = true;
static char art_url[200];
static uint16_t *art[2];              // double buffer: screen reads one while the other is filled
static int art_cur = -1;
static uint32_t art_version;

typedef struct { int cmd; char arg[96]; } sp_cmd_t;
#define MAX_PL 51
#define MAX_DEV 10
EXT_RAM_BSS_ATTR static sp_playlist_t playlists[MAX_PL];
static sp_device_t devices[MAX_DEV];
static int n_playlists = -1, n_devices;          // -1: not loaded yet
#define MAX_SHOWS 30
#define MAX_EPS 20
EXT_RAM_BSS_ATTR static sp_show_t shows[MAX_SHOWS];
EXT_RAM_BSS_ATTR static sp_episode_t eps[MAX_EPS];
static int n_shows = -1, n_eps = -1;
static char eps_show[32];
static volatile uint32_t lib_version;
static volatile bool lib_visible;
static int64_t lib_loaded_us;
static char user_id[64];
static int64_t shuffle_hold_until;   // after a toggle, trust our value until Spotify's reports catch up

void spotify_get(spotify_state_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = st;
    xSemaphoreGive(lock);
}

void spotify_set_poll_fast(bool f) { fast = f; }

const uint16_t *spotify_art(uint32_t *version)
{
    *version = art_version;
    return art_cur >= 0 ? art[art_cur] : NULL;
}

void spotify_command_arg(int cmd, const char *arg)
{
    if (!cmds) return;
    sp_cmd_t c = {.cmd = cmd};
    if (arg) strlcpy(c.arg, arg, sizeof c.arg);
    xQueueSend(cmds, &c, 0);
}

int spotify_playlists(sp_playlist_t *out, int max)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = n_playlists < max ? n_playlists : max;
    if (n > 0) memcpy(out, playlists, n * sizeof out[0]);
    xSemaphoreGive(lock);
    return n;
}

int spotify_shows(sp_show_t *out, int max)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = n_shows < max ? n_shows : max;
    if (n > 0) memcpy(out, shows, n * sizeof out[0]);
    xSemaphoreGive(lock);
    return n;
}

int spotify_episodes(sp_episode_t *out, int max, char *show_id, int id_len)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = n_eps < max ? n_eps : max;
    if (n > 0) memcpy(out, eps, n * sizeof out[0]);
    strlcpy(show_id, eps_show, id_len);
    xSemaphoreGive(lock);
    return n;
}

int spotify_devices(sp_device_t *out, int max)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = n_devices < max ? n_devices : max;
    memcpy(out, devices, n * sizeof out[0]);
    xSemaphoreGive(lock);
    return n;
}

uint32_t spotify_library_version(void) { return lib_version; }
void spotify_set_library_visible(bool v) { lib_visible = v; }

// Screen off: check every 30 s instead of every 1-5 s. On waking, check straight away.
void spotify_set_awake(bool on)
{
    if (on && !awake) spotify_command_arg(SP_POLL, NULL);
    awake = on;
}

static int64_t hold_until;              // a poll answered before Spotify caught up mustn't undo a press

void spotify_command(int cmd)
{
    if (!cmds) return;
    if (cmd == SP_SEEK_BACK || cmd == SP_SEEK_FWD) {
        xSemaphoreTake(lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        int pos = st.progress_ms + (st.playing ? (int)((now - st.fetched_us) / 1000) : 0);
        pos += cmd == SP_SEEK_FWD ? 10000 : -10000;
        if (pos < 0) pos = 0;
        if (st.duration_ms > 0 && pos > st.duration_ms - 1000) pos = st.duration_ms - 1000;
        st.progress_ms = pos;                         // the ring moves right away
        st.fetched_us = now;
        hold_until = now + 3000000;
        xSemaphoreGive(lock);
        char arg[16];
        snprintf(arg, sizeof arg, "%d", pos);
        spotify_command_arg(cmd, arg);
        return;
    }
    if (cmd != SP_PLAY_PAUSE) { spotify_command_arg(cmd, NULL); return; }
    // Decide play or pause from what's showing *now* and send that explicitly (the Spotify task may run
    // before this function returns, so it must not have to guess from a flag we haven't flipped yet)
    xSemaphoreTake(lock, portMAX_DELAY);
    bool want_play = !st.playing;
    st.playing = want_play;                            // show it right away; the next poll confirms
    st.fetched_us = esp_timer_get_time();
    hold_until = st.fetched_us + 3000000;
    xSemaphoreGive(lock);
    spotify_command_arg(cmd, want_play ? "play" : "pause");
}

static void set_note(const char *n)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(st.note, n, sizeof st.note);
    xSemaphoreGive(lock);
}

// ---------------------------------------------------------------- credentials
static void creds_load(void)
{
    nvs_handle_t h;
    size_t a = sizeof client_id, b = sizeof refresh_token;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return;
    bool ok = nvs_get_str(h, "sp_client", client_id, &a) == ESP_OK && nvs_get_str(h, "sp_refresh", refresh_token, &b) == ESP_OK;
    nvs_close(h);
    st.configured = ok && client_id[0] && refresh_token[0];
}

static void creds_save(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "sp_client", client_id);
    nvs_set_str(h, "sp_refresh", refresh_token);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- HTTP helpers
typedef struct { char *buf; int len, cap; } body_t;
static int retry_after_s;              // from the last 429 reply

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Retry-After")) retry_after_s = atoi(e->header_value);
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        int n = e->data_len < b->cap - 1 - b->len ? e->data_len : b->cap - 1 - b->len;
        if (n > 0) { memcpy(b->buf + b->len, e->data, n); b->len += n; b->buf[b->len] = 0; }
    }
    return ESP_OK;
}

static esp_http_client_handle_t api;   // kept open: one TLS handshake, then requests reuse the connection
static body_t resp;

// Returns the HTTP status (0 on network failure); response text lands in resp.buf
static int api_call_once(esp_http_client_method_t method, const char *path, const char *body)
{
    char url[256], auth[600];
    snprintf(url, sizeof url, "https://api.spotify.com/v1%s", path);
    if (!api) {
        esp_http_client_config_t cfg = {
            .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 8000,
            .keep_alive_enable = true, .event_handler = on_http, .user_data = &resp, .buffer_size = 2048,
        };
        api = esp_http_client_init(&cfg);
        if (!api) return 0;
    }
    resp.len = 0;
    resp.buf[0] = 0;
    snprintf(auth, sizeof auth, "Bearer %s", access_token);
    esp_http_client_set_url(api, url);
    esp_http_client_set_method(api, method);
    esp_http_client_set_header(api, "Authorization", auth);
    if (method == HTTP_METHOD_GET) esp_http_client_set_post_field(api, NULL, 0);
    else {
        esp_http_client_set_header(api, "Content-Type", "application/json");
        esp_http_client_set_post_field(api, body ? body : "{}", body ? strlen(body) : 2);
    }
    if (esp_http_client_perform(api) != ESP_OK) {      // connection went stale: start fresh next time
        esp_http_client_cleanup(api);
        api = NULL;
        return 0;
    }
    return esp_http_client_get_status_code(api);
}

// A kept-alive connection the server (or a Wi-Fi blip) has closed only shows up as a failed request:
// try once more on a fresh connection, so a button press isn't lost
static int api_call(esp_http_client_method_t method, const char *path, const char *body)
{
    bool reused = api != NULL;
    int status = api_call_once(method, path, body);
    if (status == 0 && reused) status = api_call_once(method, path, body);
    return status;
}

// Exchange the refresh token for an access token (Spotify may hand back a new refresh token too)
static bool refresh_access(void)
{
    char form[400];
    snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s&client_id=%s", refresh_token, client_id);
    static char buf[2048];
    body_t b = {buf, 0, sizeof buf};
    buf[0] = 0;
    esp_http_client_config_t cfg = {
        .url = "https://accounts.spotify.com/api/token", .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000, .event_handler = on_http, .user_data = &b,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, form, strlen(form));
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    if (status != 200) {
        ESP_LOGW(TAG, "token refresh failed (%d): %.120s", status, buf);
        if (status == 400) {                          // refresh token revoked or expired: needs a new sign-in
            set_note("Sign in again: run spotify_auth.py");
            xSemaphoreTake(lock, portMAX_DELAY);
            st.configured = false;
            xSemaphoreGive(lock);
        }
        return false;
    }
    cJSON *j = cJSON_Parse(buf);
    cJSON *at = cJSON_GetObjectItem(j, "access_token"), *rt = cJSON_GetObjectItem(j, "refresh_token");
    cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
    bool ok = cJSON_IsString(at);
    if (ok) {
        strlcpy(access_token, at->valuestring, sizeof access_token);
        access_expires = esp_timer_get_time() + (int64_t)((cJSON_IsNumber(ex) ? ex->valueint : 3600) - 120) * 1000000;
        if (cJSON_IsString(rt) && strcmp(rt->valuestring, refresh_token)) {   // rotated: keep the new one
            strlcpy(refresh_token, rt->valuestring, sizeof refresh_token);
            creds_save();
        }
    }
    cJSON_Delete(j);
    return ok;
}

// ---------------------------------------------------------------- album art
// ---------------------------------------------------------------- covers
// The next song's cover is downloaded ahead (while this one plays), so a skip or the song ending shows it at once
#define PRE_CAP (160 * 1024)
static uint8_t *pre_jpg;
static int pre_len;
static char pre_url[200];
static bool want_prefetch;
static const char *cover_url(cJSON *item);
static bool authed_call(esp_http_client_method_t m, const char *path, const char *body, int *status);

// Download url into buf: returns its length, -1 on failure, or -2 if it gave way to a waiting command (yield)
static int download(const char *url, uint8_t *buf, int cap, bool yield)
{
    esp_http_client_config_t cfg = {.url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000, .buffer_size = 4096};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    int len = -1;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 && esp_http_client_get_status_code(c) == 200) {
        len = 0;
        for (;;) {
            if (yield && uxQueueMessagesWaiting(cmds)) { len = -2; break; }   // a button press comes first
            if (len == cap) { len = -1; break; }      // too big
            int n = esp_http_client_read(c, (char *)buf + len, cap - len < 4096 ? cap - len : 4096);
            if (n < 0) { len = -1; break; }
            if (n == 0) break;
            len += n;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return len;
}

static void prefetch_next(void)
{
    want_prefetch = false;
    int status;
    if (!authed_call(HTTP_METHOD_GET, "/me/player/queue", NULL, &status) || status != 200) return;
    char next[200] = "";
    cJSON *j = cJSON_Parse(resp.buf), *q = cJSON_GetObjectItem(j, "queue");
    const char *u = cJSON_IsArray(q) ? cover_url(cJSON_GetArrayItem(q, 0)) : NULL;
    if (u) strlcpy(next, u, sizeof next);
    cJSON_Delete(j);
    if (!next[0] || !strcmp(next, art_url) || !strcmp(next, pre_url)) return;   // same album, or already have it
    if (!pre_jpg) pre_jpg = heap_caps_malloc(PRE_CAP, MALLOC_CAP_SPIRAM);
    if (!pre_jpg) return;
    pre_len = 0;
    pre_url[0] = 0;
    int64_t t0 = esp_timer_get_time();
    int n = download(next, pre_jpg, PRE_CAP, true);
    if (n == -2) { want_prefetch = true; return; }    // try again once the command is done
    if (n < 100) return;
    pre_len = n;
    strlcpy(pre_url, next, sizeof pre_url);
    ESP_LOGI(TAG, "preloaded next cover (%d bytes, %d ms)", n, (int)((esp_timer_get_time() - t0) / 1000));
}

// A downloaded JPEG and its decoding (640x640 or smaller), shared by the covers and the Library's thumbnails
#define JPG_CAP (400 * 1024)
static uint8_t *jpg;
static uint16_t *raw;

static bool jpeg_bufs(void)
{
    if (!jpg) jpg = heap_caps_malloc(JPG_CAP, MALLOC_CAP_SPIRAM);
    if (!raw) raw = heap_caps_malloc(640 * 640 * 2, MALLOC_CAP_SPIRAM);
    return jpg && raw;
}

static void load_art(const char *url)
{
    for (int i = 0; i < 2; i++)
        if (!art[i]) art[i] = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!jpeg_bufs() || !art[0] || !art[1]) return;

    int64_t t0 = esp_timer_get_time();
    bool preloaded = pre_len > 0 && !strcmp(url, pre_url);
    int len;
    if (preloaded) memcpy(jpg, pre_jpg, len = pre_len);
    else len = download(url, jpg, JPG_CAP, false);
    if (len < 100) { ESP_LOGW(TAG, "art download failed (%d)", len); return; }

    esp_jpeg_image_cfg_t jc = {
        .indata = jpg, .indata_size = len, .outbuf = (uint8_t *)raw, .outbuf_size = 640 * 640 * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0, .flags = {.swap_color_bytes = 1},
    };
    int64_t t1 = esp_timer_get_time();
    esp_jpeg_image_output_t out;
    if (esp_jpeg_decode(&jc, &out) != ESP_OK || !out.width) { ESP_LOGW(TAG, "art decode failed"); return; }
    int64_t t2 = esp_timer_get_time();

    // scale to fill the round screen, darken so text and controls stay readable, and fade the edge of
    // the circle to black so the square cover never shows corners (e.g. mid-swipe). Done once per album.
    uint16_t *dst = art[art_cur == 0 ? 1 : 0];
#if !SCREEN_ROUND
    // the square: the cover as it is, a rounded tile at the top on black (corners anti-aliased)
    memset(dst, 0, W * H * 2);
    const float rad = 18;
    for (int y = 0; y < ART_SZ; y++) {
        int sy = y * out.height / ART_SZ;
        for (int x = 0; x < ART_SZ; x++) {
            float cx = fminf(x + 0.5f, ART_SZ - x - 0.5f), cy = fminf(y + 0.5f, ART_SZ - y - 0.5f);
            int kk = 256;
            if (cx < rad && cy < rad) {
                float a = rad - sqrtf((rad - cx) * (rad - cx) + (rad - cy) * (rad - cy)) + 0.5f;
                if (a <= 0) continue;
                if (a < 1) kk = (int)(a * 256);
            }
            uint16_t c = SWAP16(raw[sy * out.width + x * out.width / ART_SZ]);
            int r = (c >> 11) * kk >> 8, g = ((c >> 5) & 63) * kk >> 8, bl = (c & 31) * kk >> 8;
            dst[(ART_Y + y) * W + ART_X + x] = SWAP16((uint16_t)(r << 11 | g << 5 | bl));
        }
    }
#else
    const float fade_in = 165, fade_out = 233;        // radius where the fade starts / reaches black
    for (int y = 0; y < H; y++) {
        int sy = y * out.height / H;
        int k = (y > 90 && y < 400) ? 90 : 140;       // out of 256: darker behind the text and buttons
        float dy = y - CY + 0.5f;
        for (int x = 0; x < W; x++) {
            float dx = x - CX + 0.5f, rr = sqrtf(dx * dx + dy * dy);
            int kk = k;
            if (rr >= fade_out) kk = 0;
            else if (rr > fade_in) { float t = (fade_out - rr) / (fade_out - fade_in); kk = (int)(k * t * t * (3 - 2 * t)); }
            uint16_t c = SWAP16(raw[sy * out.width + x * out.width / W]);
            int r = (c >> 11) * kk >> 8, g = ((c >> 5) & 63) * kk >> 8, bl = (c & 31) * kk >> 8;
            dst[y * W + x] = SWAP16((uint16_t)(r << 11 | g << 5 | bl));
        }
    }
#endif
    art_cur = art_cur == 0 ? 1 : 0;
    art_version++;
    want_prefetch = true;                             // new song: look up the one after it
    ESP_LOGI(TAG, "album art %dx%d (%d bytes): %s %d ms, decode %d ms, shade %d ms", out.width, out.height, len,
             preloaded ? "preloaded" : "download", (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000), (int)((esp_timer_get_time() - t2) / 1000));
}

// ---------------------------------------------------------------- the Library's thumbnails
// Each playlist's and show's cover, shrunk to SP_THUMB px and kept by its URL, so a reload of the lists (every
// minute while the page is up) only fetches the new ones. Only the Spotify task writes them; a slot's pixels are in
// place before it's marked ok, so the screen can read them as they are. (The round screen's rows have no artwork.)
#define MAX_THUMBS (MAX_PL + MAX_SHOWS)
#define URL_LEN 200
typedef struct { char url[URL_LEN]; uint16_t px[SP_THUMB * SP_THUMB]; volatile bool ok; bool tried; } thumb_t;
static thumb_t *thumbs;                               // MAX_THUMBS of them, in PSRAM

const uint16_t *spotify_thumb(int slot)
{
    return thumbs && slot >= 0 && slot < MAX_THUMBS && thumbs[slot].ok ? thumbs[slot].px : NULL;
}

// The image to shrink: the smallest at least SP_THUMB px across (Spotify lists them largest first; a cover you
// uploaded yourself has no size given, and is taken as large)
static void thumb_url(cJSON *imgs, char *out)
{
    cJSON *im;
    int best = 1 << 30;
    out[0] = 0;
    cJSON_ArrayForEach(im, imgs) {
        cJSON *u = cJSON_GetObjectItem(im, "url"), *w = cJSON_GetObjectItem(im, "width");
        if (!cJSON_IsString(u)) continue;
        int wd = cJSON_IsNumber(w) && w->valueint > 0 ? w->valueint : 640;
        if ((wd >= SP_THUMB && wd < best) || !out[0]) { strlcpy(out, u->valuestring, URL_LEN); if (wd >= SP_THUMB) best = wd; }
    }
}

// A cover's URL past its host: Spotify hands out the same picture from image-cdn-ak one time and image-cdn-fa
// the next, so it's this that says whether a cover is one we have
static const char *url_path(const char *u)
{
    const char *p = strstr(u, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    return p ? p : u;
}

// Give each of n covers (url[i], "" for none) its slot, into *slot[i]: the one it already has, or else one no list
// uses (an empty one first). keep: slots the lists not being replaced still use.
static void assign_thumbs(int n, const char *const *url, int16_t *const *slot, const bool *keep)
{
    if (!thumbs && !SCREEN_ROUND) thumbs = heap_caps_calloc(MAX_THUMBS, sizeof *thumbs, MALLOC_CAP_SPIRAM);
    bool used[MAX_THUMBS];
    memcpy(used, keep, sizeof used);
    for (int i = 0; i < n; i++) {
        *slot[i] = -1;
        for (int k = 0; thumbs && url[i][0] && k < MAX_THUMBS; k++)
            if (thumbs[k].url[0] && !strcmp(url_path(thumbs[k].url), url_path(url[i]))) {
                *slot[i] = k;
                used[k] = true;
                thumbs[k].tried = false;              // (one that failed gets another go)
                break;
            }
    }
    for (int i = 0; i < n; i++) {
        if (*slot[i] >= 0 || !thumbs || !url[i][0]) continue;
        int k = -1;
        for (int e = 0; e < MAX_THUMBS && k < 0; e++) if (!used[e] && !thumbs[e].url[0]) k = e;
        for (int e = 0; e < MAX_THUMBS && k < 0; e++) if (!used[e]) k = e;
        if (k < 0) break;
        thumbs[k].ok = thumbs[k].tried = false;
        strlcpy(thumbs[k].url, url[i], URL_LEN);
        *slot[i] = k;
        used[k] = true;
    }
}

// Download into jpg over a kept-open connection (the covers mostly come from one host): its length, or -1
static esp_http_client_handle_t img_http;
static body_t img_body;

static int img_get(const char *url)
{
    img_body = (body_t){(char *)jpg, 0, JPG_CAP};
    if (!img_http) {
        esp_http_client_config_t cfg = {
            .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 8000,
            .keep_alive_enable = true, .event_handler = on_http, .user_data = &img_body, .buffer_size = 4096,
        };
        if (!(img_http = esp_http_client_init(&cfg))) return -1;
    }
    esp_http_client_set_url(img_http, url);
    if (esp_http_client_perform(img_http) != ESP_OK) {
        esp_http_client_cleanup(img_http);
        img_http = NULL;
        return -1;
    }
    return esp_http_client_get_status_code(img_http) == 200 ? img_body.len : -1;
}

// Decode jpg (len bytes) and shrink its middle square to SP_THUMB px into out, averaging the pixels each one covers
static bool shrink_thumb(int len, uint16_t *out)
{
    esp_jpeg_image_cfg_t jc = {
        .indata = jpg, .indata_size = len, .outbuf = (uint8_t *)raw, .outbuf_size = 640 * 640 * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0, .flags = {.swap_color_bytes = 1},
    };
    esp_jpeg_image_output_t im;
    if (esp_jpeg_get_image_info(&jc, &im) != ESP_OK || !im.width || !im.height) return false;
    int sc = 0, side = im.width < im.height ? im.width : im.height;
    while (sc < 3 && side >> (sc + 1) >= SP_THUMB) sc++;   // let the decoder do most of the shrinking
    if ((im.width >> sc) * (im.height >> sc) > 640 * 640) return false;
    jc.out_scale = sc;
    if (esp_jpeg_decode(&jc, &im) != ESP_OK || !im.width) return false;
    side = im.width < im.height ? im.width : im.height;
    int ox = (im.width - side) / 2, oy = (im.height - side) / 2;
    for (int y = 0; y < SP_THUMB; y++) {
        int ya = oy + y * side / SP_THUMB, yb = oy + (y + 1) * side / SP_THUMB;
        if (yb <= ya) yb = ya + 1;
        for (int x = 0; x < SP_THUMB; x++) {
            int xa = ox + x * side / SP_THUMB, xb = ox + (x + 1) * side / SP_THUMB, r = 0, g = 0, b = 0, n = 0;
            if (xb <= xa) xb = xa + 1;
            for (int sy = ya; sy < yb; sy++)
                for (int sx = xa; sx < xb; sx++) {
                    uint16_t c = SWAP16(raw[sy * im.width + sx]);
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++;
                }
            out[y * SP_THUMB + x] = SWAP16((uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n)));
        }
    }
    return true;
}

// Fetch covers not loaded yet, for up to budget_us, giving way to a waiting command
static void load_thumbs(int64_t budget_us)
{
    static int fetched;                               // since the last time they were all in, for the log
    static int64_t busy_us;
    if (!thumbs) return;
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < MAX_THUMBS; k++) {
        thumb_t *t = &thumbs[k];
        if (!t->url[0] || t->ok || t->tried) continue;
        if (uxQueueMessagesWaiting(cmds) || esp_timer_get_time() - t0 > budget_us || !jpeg_bufs()) {
            busy_us += esp_timer_get_time() - t0;
            return;
        }
        t->tried = true;
        fetched++;
        EXT_RAM_BSS_ATTR static uint16_t px[SP_THUMB * SP_THUMB];
        int len = img_get(t->url);
        if (len < 100 || !shrink_thumb(len, px)) { ESP_LOGW(TAG, "thumbnail failed (%d): %.80s", len, t->url); continue; }
        memcpy(t->px, px, sizeof px);
        __sync_synchronize();                         // (the pixels before the flag)
        t->ok = true;
        lib_version++;
    }
    if (fetched) {
        int ok = 0;
        for (int k = 0; k < MAX_THUMBS; k++) ok += thumbs[k].ok;
        ESP_LOGI(TAG, "library covers: fetched %d (%d ms), %d in all", fetched,
                 (int)((busy_us + esp_timer_get_time() - t0) / 1000), ok);
        fetched = 0;
        busy_us = 0;
    }
}

// ---------------------------------------------------------------- polling
// A track's or episode's cover. Spotify lists sizes largest first (640, 300, 64 px): take the smallest that's at
// least 300 px. It's shown dimmed behind the controls, and a quarter of the bytes loads much faster than 640 px.
static const char *cover_url(cJSON *item)
{
    cJSON *album = cJSON_GetObjectItem(item, "album");
    cJSON *imgs = cJSON_GetObjectItem(album ? album : item, "images"), *im;
    const char *url = NULL;
    cJSON_ArrayForEach(im, imgs) {
        cJSON *u = cJSON_GetObjectItem(im, "url"), *w = cJSON_GetObjectItem(im, "width");
        if (!cJSON_IsString(u)) continue;
        if (!url || (cJSON_IsNumber(w) && w->valueint >= 300)) url = u->valuestring;
    }
    return url;
}

// The paused track, kept in NVS so a reboot during a long pause (when Spotify reports no session) still shows it
typedef struct {
    uint32_t magic;
    char title[96], artist[64], device[48], context_uri[96], art[200];
    int progress_ms, duration_ms;
    bool episode;
} last_track_t;
#define LAST_MAGIC 0x1A57B001

static void save_last_track(const spotify_state_t *s)
{
    static last_track_t t;
    t = (last_track_t){.magic = LAST_MAGIC, .progress_ms = s->progress_ms, .duration_ms = s->duration_ms, .episode = s->episode};
    strlcpy(t.title, s->title, sizeof t.title);
    strlcpy(t.artist, s->artist, sizeof t.artist);
    strlcpy(t.device, s->device, sizeof t.device);
    strlcpy(t.context_uri, s->context_uri, sizeof t.context_uri);
    strlcpy(t.art, art_url, sizeof t.art);
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "sp_last", &t, sizeof t);
    nvs_commit(h);
    nvs_close(h);
}

// Fill st from the saved track; returns its cover URL, or NULL if nothing was saved
static const char *restore_last_track(void)
{
    static last_track_t t;
    size_t len = sizeof t;
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return NULL;
    bool ok = nvs_get_blob(h, "sp_last", &t, &len) == ESP_OK && len == sizeof t && t.magic == LAST_MAGIC && t.title[0];
    nvs_close(h);
    if (!ok) return NULL;
    strlcpy(st.title, t.title, sizeof st.title);
    strlcpy(st.artist, t.artist, sizeof st.artist);
    strlcpy(st.device, t.device, sizeof st.device);
    strlcpy(st.context_uri, t.context_uri, sizeof st.context_uri);
    st.progress_ms = t.progress_ms;
    st.duration_ms = t.duration_ms;
    st.episode = t.episode;
    st.fetched_us = esp_timer_get_time();
    st.active = true;
    return t.art[0] ? t.art : NULL;
}

static void parse_player(const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return;
    spotify_state_t s = st;
    cJSON *item = cJSON_GetObjectItem(j, "item"), *dev = cJSON_GetObjectItem(j, "device");
    s.active = true;
    s.playing = cJSON_IsTrue(cJSON_GetObjectItem(j, "is_playing"));
    s.shuffle = cJSON_IsTrue(cJSON_GetObjectItem(j, "shuffle_state"));
    cJSON *ct = cJSON_GetObjectItem(j, "currently_playing_type");
    s.episode = cJSON_IsString(ct) && !strcmp(ct->valuestring, "episode");
    cJSON *ctx = cJSON_GetObjectItem(j, "context"), *cu = ctx ? cJSON_GetObjectItem(ctx, "uri") : NULL;
    strlcpy(s.context_uri, cJSON_IsString(cu) ? cu->valuestring : "", sizeof s.context_uri);
    cJSON *pr = cJSON_GetObjectItem(j, "progress_ms");
    s.progress_ms = cJSON_IsNumber(pr) ? pr->valueint : 0;
    s.fetched_us = esp_timer_get_time();
    s.title[0] = s.artist[0] = 0;
    s.duration_ms = 0;
    const char *url = NULL;
    if (cJSON_IsObject(item)) {
        cJSON *n = cJSON_GetObjectItem(item, "name"), *d = cJSON_GetObjectItem(item, "duration_ms");
        if (cJSON_IsString(n)) strlcpy(s.title, n->valuestring, sizeof s.title);
        if (cJSON_IsNumber(d)) s.duration_ms = d->valueint;
        cJSON *artists = cJSON_GetObjectItem(item, "artists"), *show = cJSON_GetObjectItem(item, "show");
        if (cJSON_IsArray(artists)) {
            int na = cJSON_GetArraySize(artists);
            for (int i = 0; i < na && i < 3; i++) {
                cJSON *an = cJSON_GetObjectItem(cJSON_GetArrayItem(artists, i), "name");
                if (!cJSON_IsString(an)) continue;
                if (i) strlcat(s.artist, ", ", sizeof s.artist);
                strlcat(s.artist, an->valuestring, sizeof s.artist);
            }
        } else if (cJSON_IsObject(show)) {           // podcast episode
            cJSON *sn = cJSON_GetObjectItem(show, "name");
            if (cJSON_IsString(sn)) strlcpy(s.artist, sn->valuestring, sizeof s.artist);
        }
        url = cover_url(item);
    }
    s.device[0] = 0;
    s.volume = -1;
    if (cJSON_IsObject(dev)) {
        cJSON *dn = cJSON_GetObjectItem(dev, "name"), *v = cJSON_GetObjectItem(dev, "volume_percent");
        cJSON *sv = cJSON_GetObjectItem(dev, "supports_volume");
        if (cJSON_IsString(dn)) strlcpy(s.device, dn->valuestring, sizeof s.device);
        if (cJSON_IsNumber(v) && !cJSON_IsFalse(sv)) s.volume = v->valueint;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (esp_timer_get_time() < hold_until && !strcmp(s.title, st.title)) {   // same song: keep what was pressed
        s.playing = st.playing;
        s.progress_ms = st.progress_ms;
        s.fetched_us = st.fetched_us;
    }
    s.note[0] = 0;
    strlcpy(s.note, st.note, sizeof s.note);
    if (s.playing != st.playing) ESP_LOGI(TAG, "Spotify reports %s", s.playing ? "playing" : "paused");
    if (strcmp(s.device, st.device) || s.episode != st.episode) {
        cJSON *dv = cJSON_GetObjectItem(j, "device"), *dt = dv ? cJSON_GetObjectItem(dv, "type") : NULL;
        cJSON *sm = cJSON_GetObjectItem(j, "smart_shuffle");
        ESP_LOGI(TAG, "now: %s on \"%s\" (%s)%s", s.episode ? "podcast" : "music", s.device,
                 cJSON_IsString(dt) ? dt->valuestring : "?", cJSON_IsTrue(sm) ? ", smart shuffle on" : "");
    }
    if (esp_timer_get_time() < shuffle_hold_until) s.shuffle = st.shuffle;   // stale report right after a toggle
    else if (s.shuffle != st.shuffle) ESP_LOGI(TAG, "Spotify reports shuffle %s", s.shuffle ? "on" : "off");
    bool paused_now = st.playing && !s.playing;
    st = s;
    xSemaphoreGive(lock);
    if (paused_now) save_last_track(&s);
    if (url && strcmp(url, art_url)) {                // new album: fetch its cover
        strlcpy(art_url, url, sizeof art_url);
        load_art(url);
    }
    cJSON_Delete(j);
}

// Backs off when Spotify can't be reached (5 s doubling to 5 min) or rate-limits us (as long as it asks)
static int64_t quiet_until;
static int fail_streak;

static bool authed_call(esp_http_client_method_t m, const char *path, const char *body, int *status)
{
    *status = 0;
    int64_t now = esp_timer_get_time();
    if (now < quiet_until) return false;
    bool ok = (access_token[0] && now < access_expires) || refresh_access();
    if (ok) {
        retry_after_s = 0;
        *status = api_call(m, path, body);
        if (*status == 401 && refresh_access()) *status = api_call(m, path, body);   // token expired early
        ok = *status != 0;
    }
    if (*status == 429) {
        int s = retry_after_s > 0 ? (retry_after_s < 600 ? retry_after_s : 600) : 5;
        quiet_until = esp_timer_get_time() + s * 1000000LL;
        ESP_LOGW(TAG, "rate limited: quiet for %d s", s);
    } else if (!ok) {
        int s = fail_streak < 6 ? 5 << fail_streak : 300;
        fail_streak++;
        quiet_until = esp_timer_get_time() + s * 1000000LL;
        if (fail_streak > 1) ESP_LOGW(TAG, "Spotify unreachable %d times: next try in %d s", fail_streak, s);
    } else fail_streak = 0;
    return ok;
}

// Spotify Connect devices the account can play on
static int fetch_devices(sp_device_t *dv)
{
    int status;
    int nd = 0;
    if (authed_call(HTTP_METHOD_GET, "/me/player/devices", NULL, &status) && status == 200) {
        cJSON *j = cJSON_Parse(resp.buf), *arr = cJSON_GetObjectItem(j, "devices"), *it;
        cJSON_ArrayForEach(it, arr) {
            if (nd == MAX_DEV) break;
            cJSON *id = cJSON_GetObjectItem(it, "id"), *n = cJSON_GetObjectItem(it, "name"), *t = cJSON_GetObjectItem(it, "type");
            if (!cJSON_IsString(id) || !cJSON_IsString(n)) continue;
            strlcpy(dv[nd].id, id->valuestring, sizeof dv[nd].id);
            strlcpy(dv[nd].name, n->valuestring, sizeof dv[nd].name);
            strlcpy(dv[nd].type, cJSON_IsString(t) ? t->valuestring : "", sizeof dv[nd].type);
            dv[nd].active = cJSON_IsTrue(cJSON_GetObjectItem(it, "is_active"));
            nd++;
        }
        cJSON_Delete(j);
    }
    return nd;
}

static void load_library(void);
static void refresh_devices(void);
static void load_episodes(const char *show_id);

// After a while paused Spotify forgets the session (no active device), though the app still shows the song.
// Commands then answer 404; send them again aimed at the device we last saw playing (or the first one).
static void retry_on_device(esp_http_client_method_t m, const char *path, int *status)
{
    if (*status != 404) return;
    EXT_RAM_BSS_ATTR static sp_device_t dv[MAX_DEV];   // off the task stack
    int nd = fetch_devices(dv), pick = -1;
    for (int i = 0; i < nd; i++)
        if (pick < 0 || !strcmp(dv[i].name, st.device)) pick = i;
    if (pick < 0) return;
    char p[200];
    snprintf(p, sizeof p, "%s%cdevice_id=%.64s", path, strchr(path, '?') ? '&' : '?', dv[pick].id);
    ESP_LOGI(TAG, "no active device: retrying on \"%s\"", dv[pick].name);
    authed_call(m, p, NULL, status);
}

static void run_command(int cmd, const char *arg)
{
    spotify_state_t s;
    spotify_get(&s);
    int status = 0;
    char path[160], body[200];
    switch (cmd) {
    case SP_POLL: return;
    case SP_LOAD_LIBRARY:
        // swiping towards the Library asks for this every time; a full reload takes seconds and holds up
        // play/skip queued behind it, so reuse one from the last minute (the device list is quick: always fresh)
        if (!strcmp(arg, "devices")) refresh_devices();
        else if (n_playlists < 0 || esp_timer_get_time() - lib_loaded_us > 60000000) load_library();
        return;
    case SP_LOAD_EPISODES: load_episodes(arg); return;
    case SP_SEEK_BACK:
    case SP_SEEK_FWD:
        snprintf(path, sizeof path, "/me/player/seek?position_ms=%d", atoi(arg));
        authed_call(HTTP_METHOD_PUT, path, NULL, &status);
        break;
    case SP_PLAY_EPISODE: {                           // "<show id> <episode id> <position ms>": resume where you left off
        char show[32] = "", ep[32] = "";
        long pos = 0;
        sscanf(arg, "%31s %31s %ld", show, ep, &pos);
        const char *dev = NULL;
        if (!s.active) for (int i = 0; i < n_devices && !dev; i++) dev = devices[i].id;
        if (dev) snprintf(path, sizeof path, "/me/player/play?device_id=%.64s", dev);
        else strlcpy(path, "/me/player/play", sizeof path);
        snprintf(body, sizeof body, "{\"context_uri\":\"spotify:show:%s\",\"offset\":{\"uri\":\"spotify:episode:%s\"},\"position_ms\":%ld}", show, ep, pos);
        authed_call(HTTP_METHOD_PUT, path, body, &status);
        break;
    }
    case SP_SHUFFLE:
        snprintf(path, sizeof path, "/me/player/shuffle?state=%s", s.shuffle ? "false" : "true");
        ESP_LOGI(TAG, "shuffle %s requested", s.shuffle ? "off" : "on");
        if (authed_call(HTTP_METHOD_PUT, path, NULL, &status)) ESP_LOGI(TAG, "shuffle reply %d: %.160s", status, resp.buf);
        if (status >= 200 && status < 300) {
            xSemaphoreTake(lock, portMAX_DELAY);
            st.shuffle = !s.shuffle;
            shuffle_hold_until = esp_timer_get_time() + 3000000;
            xSemaphoreGive(lock);
        }
        break;
    case SP_PLAY_CONTEXT: {
        // play on the current device, or the first one Spotify knows about if nothing is active
        const char *dev = NULL;
        if (!s.active) for (int i = 0; i < n_devices && !dev; i++) dev = devices[i].id;
        if (dev) snprintf(path, sizeof path, "/me/player/play?device_id=%.64s", dev);
        else strlcpy(path, "/me/player/play", sizeof path);
        snprintf(body, sizeof body, "{\"context_uri\":\"%.96s\"}", arg);
        authed_call(HTTP_METHOD_PUT, path, body, &status);
        break;
    }
    case SP_TRANSFER:
        snprintf(body, sizeof body, "{\"device_ids\":[\"%.64s\"],\"play\":true}", arg);
        authed_call(HTTP_METHOD_PUT, "/me/player", body, &status);
        if (status < 300) refresh_devices();          // which device is active now
        break;
    case SP_PLAY_PAUSE: {
        bool play = !strcmp(arg, "play");
        ESP_LOGI(TAG, "%s requested", play ? "play" : "pause");
        authed_call(HTTP_METHOD_PUT, play ? "/me/player/play" : "/me/player/pause", NULL, &status);
        if (play) retry_on_device(HTTP_METHOD_PUT, "/me/player/play", &status);
        break;
    }
    case SP_NEXT:
        authed_call(HTTP_METHOD_POST, "/me/player/next", NULL, &status);
        retry_on_device(HTTP_METHOD_POST, "/me/player/next", &status);
        break;
    case SP_PREV:
        authed_call(HTTP_METHOD_POST, "/me/player/previous", NULL, &status);
        retry_on_device(HTTP_METHOD_POST, "/me/player/previous", &status);
        break;
    case SP_VOL_UP:
    case SP_VOL_DOWN: {
        if (s.volume < 0) { set_note("This device has no volume control"); return; }
        int v = s.volume + (cmd == SP_VOL_UP ? 10 : -10);
        v = v < 0 ? 0 : (v > 100 ? 100 : v);
        snprintf(path, sizeof path, "/me/player/volume?volume_percent=%d", v);
        if (authed_call(HTTP_METHOD_PUT, path, NULL, &status) && status < 300) {
            xSemaphoreTake(lock, portMAX_DELAY);
            st.volume = v;
            xSemaphoreGive(lock);
        }
        break;
    }
    }
    if (status == 403) set_note("Controls need Spotify Premium");
    else if (status == 404) set_note("No active Spotify device");
    else if (status >= 200 && status < 300) set_note("");
    if (status >= 300) ESP_LOGW(TAG, "command %d -> %d: %.200s", cmd, status, resp.buf);
    else ESP_LOGI(TAG, "command %d -> %d", cmd, status);
}

// Liked Songs + playlists + devices, for the Library page
static void refresh_devices(void)
{
    EXT_RAM_BSS_ATTR static sp_device_t dv[MAX_DEV];
    int nd = fetch_devices(dv);
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(devices, dv, nd * sizeof dv[0]);
    n_devices = nd;
    xSemaphoreGive(lock);
    lib_version++;
}

static void load_library(void)
{
    int status;
    if (!user_id[0] && authed_call(HTTP_METHOD_GET, "/me", NULL, &status) && status == 200) {
        cJSON *j = cJSON_Parse(resp.buf), *id = cJSON_GetObjectItem(j, "id");
        if (cJSON_IsString(id)) strlcpy(user_id, id->valuestring, sizeof user_id);
        cJSON_Delete(j);
    }
    EXT_RAM_BSS_ATTR static sp_playlist_t pl[MAX_PL];
    EXT_RAM_BSS_ATTR static char img[MAX_THUMBS][URL_LEN];   // each one's cover: the playlists', then the shows'
    int np = 0;
    if (user_id[0]) {
        strlcpy(pl[0].name, "Liked Songs", sizeof pl[0].name);
        snprintf(pl[0].uri, sizeof pl[0].uri, "spotify:user:%s:collection", user_id);
        pl[0].tracks = -1;
        img[0][0] = 0;                                // (drawn as Spotify's heart tile)
        np = 1;
    }
    bool pl_ok = authed_call(HTTP_METHOD_GET, "/me/playlists?limit=50", NULL, &status) && status == 200;
    if (pl_ok) {
        cJSON *j = cJSON_Parse(resp.buf), *items = cJSON_GetObjectItem(j, "items"), *it;
        cJSON_ArrayForEach(it, items) {
            if (np == MAX_PL) break;
            cJSON *n = cJSON_GetObjectItem(it, "name"), *u = cJSON_GetObjectItem(it, "uri");
            cJSON *tr = cJSON_GetObjectItem(it, "tracks"), *tot = tr ? cJSON_GetObjectItem(tr, "total") : NULL;
            if (!cJSON_IsString(n) || !cJSON_IsString(u)) continue;
            strlcpy(pl[np].name, n->valuestring, sizeof pl[np].name);
            strlcpy(pl[np].uri, u->valuestring, sizeof pl[np].uri);
            pl[np].tracks = cJSON_IsNumber(tot) ? tot->valueint : -1;
            thumb_url(cJSON_GetObjectItem(it, "images"), img[np]);
            np++;
        }
        cJSON_Delete(j);
    } else if (status == 401 || status == 403) {
        set_note("Run spotify_auth.py again for playlists");
    }
    EXT_RAM_BSS_ATTR static sp_show_t sh[MAX_SHOWS];
    int ns = -1;
    if (authed_call(HTTP_METHOD_GET, "/me/shows?limit=30", NULL, &status) && status == 200) {
        cJSON *j = cJSON_Parse(resp.buf), *items = cJSON_GetObjectItem(j, "items"), *it;
        ns = 0;
        cJSON_ArrayForEach(it, items) {
            if (ns == MAX_SHOWS) break;
            cJSON *show = cJSON_GetObjectItem(it, "show");
            cJSON *id = cJSON_GetObjectItem(show, "id"), *n = cJSON_GetObjectItem(show, "name"), *p = cJSON_GetObjectItem(show, "publisher");
            if (!cJSON_IsString(id) || !cJSON_IsString(n)) continue;
            strlcpy(sh[ns].id, id->valuestring, sizeof sh[ns].id);
            strlcpy(sh[ns].name, n->valuestring, sizeof sh[ns].name);
            strlcpy(sh[ns].publisher, cJSON_IsString(p) ? p->valuestring : "", sizeof sh[ns].publisher);
            thumb_url(cJSON_GetObjectItem(show, "images"), img[MAX_PL + ns]);
            ns++;
        }
        cJSON_Delete(j);
    } else if (status == 401 || status == 403) {
        set_note("Run spotify_auth.py again for podcasts");
    }
    {                                                 // their covers' slots (a list that failed to load keeps its own)
        static const char *url[MAX_THUMBS];
        static int16_t *slot[MAX_THUMBS];
        bool keep[MAX_THUMBS] = {0}, new_pl = pl_ok || n_playlists < 0;
        int n = 0;
        if (!new_pl) for (int i = 0; i < n_playlists; i++) if (playlists[i].thumb >= 0) keep[playlists[i].thumb] = true;
        if (ns < 0) for (int i = 0; i < n_shows; i++) if (shows[i].thumb >= 0) keep[shows[i].thumb] = true;
        for (int i = 0; new_pl && i < np; i++, n++) { url[n] = SCREEN_ROUND ? "" : img[i]; slot[n] = &pl[i].thumb; }
        for (int i = 0; i < ns; i++, n++) { url[n] = SCREEN_ROUND ? "" : img[MAX_PL + i]; slot[n] = &sh[i].thumb; }
        assign_thumbs(n, url, slot, keep);            // (the round screen's rows have no artwork: none)
    }
    EXT_RAM_BSS_ATTR static sp_device_t dv[MAX_DEV];   // off the task stack
    int nd = fetch_devices(dv);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (pl_ok || n_playlists < 0) {                    // a failed request keeps the list we had
        memcpy(playlists, pl, np * sizeof pl[0]);
        n_playlists = np;
    }
    memcpy(devices, dv, nd * sizeof dv[0]);
    n_devices = nd;
    if (ns >= 0) { memcpy(shows, sh, ns * sizeof sh[0]); n_shows = ns; }
    xSemaphoreGive(lock);
    lib_version++;
    lib_loaded_us = esp_timer_get_time();
    ESP_LOGI(TAG, "library: %d playlists, %d podcasts, %d devices", np, ns, nd);
}

// Latest episodes of one show, with how far you got in each
static void load_episodes(const char *show_id)
{
    int status;
    char path[96];
    snprintf(path, sizeof path, "/shows/%.31s/episodes?limit=20", show_id);
    xSemaphoreTake(lock, portMAX_DELAY);
    n_eps = -1;                                       // "loading" until this one arrives
    strlcpy(eps_show, show_id, sizeof eps_show);
    xSemaphoreGive(lock);
    lib_version++;
    EXT_RAM_BSS_ATTR static sp_episode_t ep[MAX_EPS];
    int n = 0;
    if (authed_call(HTTP_METHOD_GET, path, NULL, &status) && status == 200) {
        cJSON *j = cJSON_Parse(resp.buf), *items = cJSON_GetObjectItem(j, "items"), *it;
        cJSON_ArrayForEach(it, items) {
            if (n == MAX_EPS) break;
            cJSON *id = cJSON_GetObjectItem(it, "id"), *nm = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(id) || !cJSON_IsString(nm)) continue;
            cJSON *d = cJSON_GetObjectItem(it, "release_date"), *du = cJSON_GetObjectItem(it, "duration_ms");
            cJSON *rp = cJSON_GetObjectItem(it, "resume_point");
            cJSON *pos = rp ? cJSON_GetObjectItem(rp, "resume_position_ms") : NULL, *fp = rp ? cJSON_GetObjectItem(rp, "fully_played") : NULL;
            strlcpy(ep[n].id, id->valuestring, sizeof ep[n].id);
            strlcpy(ep[n].name, nm->valuestring, sizeof ep[n].name);
            strlcpy(ep[n].date, cJSON_IsString(d) ? d->valuestring : "", sizeof ep[n].date);
            ep[n].duration_ms = cJSON_IsNumber(du) ? du->valueint : 0;
            ep[n].resume_ms = cJSON_IsNumber(pos) ? pos->valueint : 0;
            ep[n].played = cJSON_IsTrue(fp);
            n++;
        }
        cJSON_Delete(j);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!strcmp(eps_show, show_id)) {                 // still the show being looked at
        memcpy(eps, ep, n * sizeof ep[0]);
        n_eps = n;
    }
    xSemaphoreGive(lock);
    lib_version++;
    ESP_LOGI(TAG, "%d episodes (HTTP %d)", n, status);
}

static bool dormant;                                  // Spotify reported no session; showing the last track
static int64_t woke_us;

// Spotify forgets a paused session after a few minutes, though the app (say on the Mac) still has the song
// loaded. Handing playback to that device with play:false wakes the session without starting any sound,
// so /me/player reports the paused song again.
static void wake_session(void)
{
    int64_t now = esp_timer_get_time();
    if (!awake || (woke_us && now - woke_us < 300000000)) return;   // at most every 5 minutes, and only if someone's looking
    woke_us = now;
    EXT_RAM_BSS_ATTR static sp_device_t dv[MAX_DEV];   // off the task stack
    int nd = fetch_devices(dv), pick = -1;
    for (int i = 0; i < nd && pick < 0; i++) if (!strcmp(dv[i].name, st.device)) pick = i;   // where it was playing,
    for (int i = 0; i < nd && pick < 0; i++) if (!strcmp(dv[i].type, "Computer")) pick = i;  // else a computer
    if (pick < 0) return;                             // phones and speakers can't be woken: leave them be
    char body[120];
    snprintf(body, sizeof body, "{\"device_ids\":[\"%.64s\"],\"play\":false}", dv[pick].id);
    int status = 0;
    authed_call(HTTP_METHOD_PUT, "/me/player", body, &status);
    ESP_LOGI(TAG, "woke the paused session on \"%s\" (HTTP %d)", dv[pick].name, status);
}

static void poll_player(void)
{
    int status;
    if (!authed_call(HTTP_METHOD_GET, "/me/player?additional_types=track,episode&market=from_token", NULL, &status)) return;
    if (status == 200 && resp.len > 0) { parse_player(resp.buf); dormant = false; }
    else if (status == 204) {                         // no active session on any device
        // Spotify drops a paused session after a few minutes even though the app still shows the song:
        // keep showing the last track, paused, rather than "Nothing playing"
        xSemaphoreTake(lock, portMAX_DELAY);
        const char *art = st.title[0] ? NULL : restore_last_track();
        if (!dormant) ESP_LOGI(TAG, "Spotify has no active session%s", st.title[0] ? ": showing last track paused" : "");
        bool was_playing = st.playing;
        dormant = true;
        if (!st.title[0]) st.active = false;
        st.playing = false;
        spotify_state_t snap = st;
        xSemaphoreGive(lock);
        if (was_playing) save_last_track(&snap);
        wake_session();
        if (art && strcmp(art, art_url)) {
            strlcpy(art_url, art, sizeof art_url);
            load_art(art_url);
        }
    }
}

static void sp_task(void *arg)
{
    resp.cap = 192 * 1024;                            // a page of 50 playlists is ~100 KB of JSON
    resp.buf = heap_caps_malloc(resp.cap, MALLOC_CAP_SPIRAM);
    for (;;) {
        sp_cmd_t c;
        int wait_ms = !awake ? 180000 : fast || lib_visible ? 1000 : 5000;   // screen off: every 3 minutes
        bool got = xQueueReceive(cmds, &c, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
        if (!st.configured || wifi_state() != WS_CONNECTED || !wifi_time_synced()) continue;
        if (got && c.cmd != SP_POLL) {
            char before[96];
            strlcpy(before, st.title, sizeof before);
            run_command(c.cmd, c.arg);
            vTaskDelay(pdMS_TO_TICKS(250));            // give Spotify a moment before re-reading
            if (c.cmd == SP_NEXT || c.cmd == SP_PREV || c.cmd == SP_PLAY_CONTEXT)
                for (int i = 0; i < 8 && !strcmp(before, st.title); i++) {   // Spotify takes a moment to switch songs:
                    poll_player();                                           // look again every 300 ms, so the new
                    vTaskDelay(pdMS_TO_TICKS(300));                          // title and cover start loading at once
                }
        }
        if (lib_visible && esp_timer_get_time() - lib_loaded_us > 60000000) load_library();   // devices come and go
        poll_player();
        if (want_prefetch && awake && st.active && !uxQueueMessagesWaiting(cmds)) prefetch_next();
        if (awake) load_thumbs(lib_visible ? 2000000 : 500000);   // the Library's covers: quickly while it's up
        static int64_t stack_log;
        if (esp_timer_get_time() - stack_log > 600000000) {
            stack_log = esp_timer_get_time();
            ESP_LOGI(TAG, "stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

// cJSON makes many small allocations, and small ones go to scarce internal RAM first: send them to PSRAM
static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

void spotify_start(void)
{
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = psram_malloc, .free_fn = free});
    lock = xSemaphoreCreateMutex();
    cmds = xQueueCreate(8, sizeof(sp_cmd_t));
    st.volume = -1;
    creds_load();
    ESP_LOGI(TAG, "%s", st.configured ? "signed in" : "not signed in (run spotify_auth.py)");
#if CONFIG_SPIRAM_XIP_FROM_PSRAM   // (stack in PSRAM where code runs from there, as on the square: flash writes don't pause it, and internal RAM is short)
    xTaskCreateWithCaps(sp_task, "spotify", 12288, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
#else
    xTaskCreate(sp_task, "spotify", 12288, NULL, 3, NULL);
#endif
}

// ---------------------------------------------------------------- sign-in hand-off from the Mac's Bobble app
// POST {"client_id", "refresh_token"}: connect; GET: {"connected": bool}; DELETE: disconnect. All with the display key.
static esp_err_t spotify_post(httpd_req_t *req)
{
    char body[600] = {0};
    if (!agent_token_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "bad token\n");
    }
    int n = httpd_req_recv(req, body, sizeof body - 1);
    cJSON *j = n > 0 ? cJSON_Parse(body) : NULL;
    cJSON *cid = cJSON_GetObjectItem(j, "client_id"), *rt = cJSON_GetObjectItem(j, "refresh_token");
    if (!cJSON_IsString(cid) || !cJSON_IsString(rt)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need client_id and refresh_token");
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(client_id, cid->valuestring, sizeof client_id);
    strlcpy(refresh_token, rt->valuestring, sizeof refresh_token);
    access_token[0] = 0;
    st.configured = true;
    st.note[0] = 0;
    xSemaphoreGive(lock);
    cJSON_Delete(j);
    creds_save();
    ESP_LOGI(TAG, "signed in from the Mac");
    spotify_command(-1);                              // poll right away
    return httpd_resp_sendstr(req, "{\"connected\":true}");
}

static esp_err_t spotify_get_status(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, st.configured ? "{\"connected\":true}" : "{\"connected\":false}");
}

static esp_err_t spotify_delete(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    xSemaphoreTake(lock, portMAX_DELAY);
    client_id[0] = refresh_token[0] = access_token[0] = 0;
    st.configured = st.playing = st.active = false;
    xSemaphoreGive(lock);
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "sp_client");
        nvs_erase_key(h, "sp_refresh");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "disconnected from the Mac");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"connected\":false}");
}

void spotify_register(httpd_handle_t server)
{
    const httpd_uri_t u[] = {
        {.uri = "/spotify", .method = HTTP_POST, .handler = spotify_post},
        {.uri = "/spotify", .method = HTTP_GET, .handler = spotify_get_status},
        {.uri = "/spotify", .method = HTTP_DELETE, .handler = spotify_delete},
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(server, &u[i]);
}
