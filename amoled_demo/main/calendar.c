#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs.h"
#include "cJSON.h"
#include "wifi_setup.h"
#include "agent.h"
#include "calendar.h"
#include "secrets.h"

static const char *TAG = "cal";

#define MAX_ITEMS 96                          // merged week, all calendars
#define MAX_PER_CAL 64

// The calendars' private .ics links (NVS "cal"/"urls", one per line), set from the Mac app
#define MAX_CAL 6
#define URL_LEN 320
EXT_RAM_BSS_ATTR static char urls[MAX_CAL][URL_LEN];
static int n_cal;
#define N_CAL n_cal

// last good download of each calendar, so one failing doesn't blank the others
EXT_RAM_BSS_ATTR static ical_item_t cache[MAX_CAL][MAX_PER_CAL];
static int cache_n[MAX_CAL];
static bool cache_ok[MAX_CAL];
#define REFRESH_US (5 * 60 * 1000000LL)
#define ASLEEP_REFRESH_US (30 * 60 * 1000000LL)   // nobody's looking: each fetch is ~375 KB of downloading and parsing
static volatile bool awake = true;

static SemaphoreHandle_t lock;
EXT_RAM_BSS_ATTR static ical_item_t items[MAX_ITEMS];
static int n_items;
static time_t window_start;
static char error[64] = "Loading calendar...";
static volatile bool loaded, refresh_now;
static TaskHandle_t task;

int calendar_items(ical_item_t *out, int max)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = n_items < max ? n_items : max;
    memcpy(out, items, n * sizeof items[0]);
    xSemaphoreGive(lock);
    return n;
}

time_t calendar_window_start(void) { return window_start; }
const char *calendar_error(void) { return error; }
bool calendar_loaded(void) { return loaded; }
int calendar_count(void) { return N_CAL; }

// Split newline-separated links into urls[] (lock held, or before the task starts)
static void set_urls(const char *list)
{
    n_cal = 0;
    for (const char *p = list; *p && n_cal < MAX_CAL;) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len > 8 && len < URL_LEN) { memcpy(urls[n_cal], p, len); urls[n_cal][len] = 0; n_cal++; }
        p += len + (e ? 1 : 0);
    }
    memset(cache_ok, 0, sizeof cache_ok);
    memset(cache_n, 0, sizeof cache_n);
}

static void load_urls(void)
{
    static char list[MAX_CAL * URL_LEN];
    size_t len = sizeof list;
    nvs_handle_t h;
    list[0] = 0;
    if (nvs_open("cal", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "urls", list, &len) != ESP_OK) list[0] = 0;
        nvs_close(h);
    }
#ifdef CAL_ICS_URLS                                    // (a development build with links in secrets.h: copied in once)
    if (!list[0]) {
        static const char *const seed[] = CAL_ICS_URLS;
        for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? "\n" : "", seed[i]);
        if (nvs_open("cal", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "urls", list); nvs_commit(h); nvs_close(h); }
    }
#endif
    set_urls(list);
}

// GET: how many calendars (and their links, for the app to show); POST {"urls": [...]}: replace them
static esp_err_t cal_get(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    cJSON *j = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(j, "urls");
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < n_cal; i++) cJSON_AddItemToArray(a, cJSON_CreateString(urls[i]));
    xSemaphoreGive(lock);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    return r;
}

static esp_err_t cal_post(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    static char body[MAX_CAL * URL_LEN + 256], list[MAX_CAL * URL_LEN];
    int got = 0;
    if (req->content_len <= 0 || req->content_len >= (int)sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        got += n;
    }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body), *a = cJSON_GetObjectItem(j, "urls"), *u;
    if (!cJSON_IsArray(a)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need urls"); }
    list[0] = 0;
    int n = 0;
    cJSON_ArrayForEach(u, a) {
        if (!cJSON_IsString(u) || n == MAX_CAL || strlen(u->valuestring) >= URL_LEN) continue;
        const char *v = u->valuestring;
        if (!strncmp(v, "webcal://", 9)) {            // (Apple's share links: the same feed over https)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%shttps://%s", n ? "\n" : "", v + 9);
        } else snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", n ? "\n" : "", v);
        n++;
    }
    cJSON_Delete(j);
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "urls", list); nvs_commit(h); nvs_close(h); }
    xSemaphoreTake(lock, portMAX_DELAY);
    set_urls(list);
    n_items = 0;
    loaded = false;
    snprintf(error, sizeof error, n_cal ? "Loading calendar..." : "No calendars yet");
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%d calendar(s) set", n_cal);
    calendar_refresh();
    httpd_resp_set_type(req, "application/json");
    char reply[32];
    snprintf(reply, sizeof reply, "{\"count\":%d}", n_cal);
    return httpd_resp_sendstr(req, reply);
}

void calendar_register(httpd_handle_t server)
{
    const httpd_uri_t u[] = {
        {.uri = "/calendars", .method = HTTP_GET, .handler = cal_get},
        {.uri = "/calendars", .method = HTTP_POST, .handler = cal_post},
    };
    for (int i = 0; i < 2; i++) httpd_register_uri_handler(server, &u[i]);
}

void calendar_refresh(void)
{
    refresh_now = true;
    if (task) xTaskNotifyGive(task);
}

static time_t today_midnight(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// Download and expand calendar k into its cache slot. Returns false (leaving the old copy) on failure.
static bool fetch_one(int k, time_t from, time_t to, char *why, size_t why_len)
{
    esp_http_client_config_t cfg = {
        .url = urls[k],
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(why, why_len, "out of memory"); return false; }

    bool ok = false;
    ical_parser_t *p = NULL;
    static char buf[2048];
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { snprintf(why, why_len, "can't connect (%s)", esp_err_to_name(err)); goto done; }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { snprintf(why, why_len, status == 404 ? "link not found" : "HTTP %d", status); goto done; }
    p = ical_new();
    if (!p) { snprintf(why, why_len, "out of memory"); goto done; }
    int total = 0, n;
    while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) {
        ical_feed(p, buf, n);
        total += n;
    }
    ical_finish(p);
    if (n < 0 || !esp_http_client_is_complete_data_received(c)) { snprintf(why, why_len, "download interrupted"); goto done; }

    EXT_RAM_BSS_ATTR static ical_item_t fresh[MAX_PER_CAL];
    int count = ical_expand(p, from, to, fresh, MAX_PER_CAL);
    for (int i = 0; i < count; i++) fresh[i].cal = k;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(cache[k], fresh, count * sizeof fresh[0]);
    cache_n[k] = count;
    cache_ok[k] = true;
    xSemaphoreGive(lock);
    ok = true;
    ESP_LOGI(TAG, "calendar %d: %d bytes, %d events, %d this week", k + 1, total, ical_event_count(p), count);
done:
    if (!ok) ESP_LOGW(TAG, "calendar %d failed: %s", k + 1, why);
    ical_free(p);
    esp_http_client_cleanup(c);
    return ok;
}

static int cmp_items(const void *a, const void *b)
{
    const ical_item_t *x = a, *y = b;
    if (x->all_day != y->all_day) return x->all_day ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->title, y->title);
}

// Fetch every calendar, then merge the cached copies into one sorted, de-duplicated list
static bool fetch(void)
{
    time_t from = today_midnight();
    struct tm tm;
    localtime_r(&from, &tm);
    tm.tm_mday += 7;                                 // a week ahead for the agenda
    tm.tm_isdst = -1;
    time_t to = mktime(&tm);

    int failed = 0, first_failed = -1;
    char why[48] = "";
    for (int k = 0; k < N_CAL; k++)
        if (!fetch_one(k, from, to, why, sizeof why)) { failed++; if (first_failed < 0) first_failed = k; }

    EXT_RAM_BSS_ATTR static ical_item_t merged[MAX_ITEMS];
    int n = 0, have = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int k = 0; k < N_CAL; k++) {
        if (!cache_ok[k]) continue;
        have++;
        for (int i = 0; i < cache_n[k] && n < MAX_ITEMS; i++) {
            bool dup = false;                        // same meeting on two calendars (shared invites)
            for (int j = 0; j < n && !dup; j++)
                dup = merged[j].start == cache[k][i].start && merged[j].end == cache[k][i].end &&
                      !strcmp(merged[j].title, cache[k][i].title);
            if (!dup) merged[n++] = cache[k][i];
        }
    }
    qsort(merged, n, sizeof merged[0], cmp_items);
    if (have) {
        memcpy(items, merged, n * sizeof merged[0]);
        n_items = n;
        window_start = from;
        loaded = true;
    }
    xSemaphoreGive(lock);

    if (!failed) error[0] = 0;
    else if (N_CAL == 1) snprintf(error, sizeof error, "Calendar %s", why);
    else snprintf(error, sizeof error, "Calendar %d: %s", first_failed + 1, why);
    return failed == 0;
}

static void cal_task(void *arg)
{
    int64_t last_ok = 0;
    time_t last_day = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool online = wifi_state() == WS_CONNECTED && wifi_time_synced();
        bool new_day = loaded && today_midnight() != last_day;
        if (online && (refresh_now || !loaded || new_day || now - last_ok > (awake ? REFRESH_US : ASLEEP_REFRESH_US))) {
            refresh_now = false;
            if (fetch()) { last_ok = now; last_day = today_midnight(); }
            else last_ok = now - REFRESH_US + 60 * 1000000LL;   // retry in a minute
        } else if (!online && !loaded) {
            snprintf(error, sizeof error, wifi_state() == WS_CONNECTED ? "Syncing time..." : "Connect Wi-Fi to load calendar");
        }
        if (!n_cal) {                                 // nothing to fetch: say where calendars come from
            snprintf(error, sizeof error, "No calendars yet: add them in the Bobble app");
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

// Screen on/off: sync less while asleep; on waking, anything older than 5 minutes is fetched straight away
void calendar_set_awake(bool on)
{
    awake = on;
    if (on && task) xTaskNotifyGive(task);
}

void calendar_start(void)
{
    lock = xSemaphoreCreateMutex();
    load_urls();
    xTaskCreate(cal_task, "calendar", 8192, NULL, 3, &task);
}
