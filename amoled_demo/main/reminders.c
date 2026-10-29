#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "reminders.h"
#include "secrets.h"
#include "agent.h"

static const char *TAG = "reminders";

#define MAX_REM 64
#define MAX_DONE 16
#define MAX_SAVED 16                          // kept in NVS (it's small) so a reboot doesn't blank the list until the Mac's next send
EXT_RAM_BSS_ATTR static reminder_t rems[MAX_REM];
static int n_rems;
EXT_RAM_BSS_ATTR static char done_ids[MAX_DONE][96];   // ticked on the board, waiting for the Mac to complete them
static int n_done;
static SemaphoreHandle_t lock;

// Ticked-off ids are kept in NVS until the Mac has completed them, so a reboot in between doesn't undo a tick
static void save_done(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "rdone", done_ids, n_done * sizeof done_ids[0]);
    nvs_commit(h);
    nvs_close(h);
}

static bool is_done(const char *id)
{
    for (int i = 0; i < n_done; i++)
        if (!strcmp(done_ids[i], id)) return true;
    return false;
}

int reminders_get(reminder_t *out, int max)
{
    if (!lock) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < n_rems && n < max; i++)
        if (!is_done(rems[i].id)) out[n++] = rems[i];
    xSemaphoreGive(lock);
    return n;
}

void reminders_complete(const char *id)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool added = !is_done(id) && n_done < MAX_DONE;
    if (added) strlcpy(done_ids[n_done++], id, sizeof done_ids[0]);
    xSemaphoreGive(lock);
    if (added) save_done();
    ESP_LOGI(TAG, "ticked off on the board");
}

static bool token_ok(httpd_req_t *req)
{
    if (agent_token_ok(req)) return true;            // (the paired Mac, or the development token)
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "bad token\n");
    return false;
}

static int by_due(const void *a, const void *b)
{
    const reminder_t *x = a, *y = b;
    return x->due < y->due ? -1 : x->due > y->due;
}

// The Mac sends the whole current list: [{"id","title","list","due","time","color":"#rrggbb"}, ...]
static esp_err_t post_reminders(httpd_req_t *req)
{
    if (!token_ok(req)) return ESP_OK;
    if (req->content_len <= 0 || req->content_len > 48 * 1024) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed"); }
        got += n;
    }
    body[got] = 0;
    cJSON *arr = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a JSON array"); }

    EXT_RAM_BSS_ATTR static reminder_t fresh[MAX_REM];
    int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n == MAX_REM) break;
        cJSON *id = cJSON_GetObjectItem(it, "id"), *t = cJSON_GetObjectItem(it, "title"), *due = cJSON_GetObjectItem(it, "due");
        if (!cJSON_IsString(id) || !cJSON_IsString(t) || !cJSON_IsNumber(due)) continue;
        reminder_t *r = &fresh[n++];
        memset(r, 0, sizeof *r);
        strlcpy(r->id, id->valuestring, sizeof r->id);
        strlcpy(r->title, t->valuestring, sizeof r->title);
        cJSON *l = cJSON_GetObjectItem(it, "list"), *c = cJSON_GetObjectItem(it, "color");
        if (cJSON_IsString(l)) strlcpy(r->list, l->valuestring, sizeof r->list);
        r->due = (time_t)due->valuedouble;
        r->has_time = cJSON_IsTrue(cJSON_GetObjectItem(it, "time"));
        unsigned int rgb = 0xFF9500;                  // Reminders' orange if no colour given
        if (cJSON_IsString(c) && c->valuestring[0] == '#') rgb = strtoul(c->valuestring + 1, NULL, 16);
        r->r = rgb >> 16; r->g = rgb >> 8; r->b = rgb;
    }
    cJSON_Delete(arr);
    qsort(fresh, n, sizeof fresh[0], by_due);

    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = n != n_rems || memcmp(rems, fresh, n * sizeof fresh[0]);
    memcpy(rems, fresh, n * sizeof fresh[0]);
    n_rems = n;
    // a ticked reminder that the Mac no longer sends has been completed there: stop tracking it
    int k = 0;
    for (int i = 0; i < n_done; i++) {
        bool still = false;
        for (int j = 0; j < n && !still; j++) still = !strcmp(rems[j].id, done_ids[i]);
        if (still) memmove(done_ids[k++], done_ids[i], sizeof done_ids[0]);
    }
    bool pruned = k != n_done;
    n_done = k;
    xSemaphoreGive(lock);
    if (pruned) save_done();
    if (changed) {
        nvs_handle_t h;
        if (nvs_open("demo", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "rems", fresh, (n < MAX_SAVED ? n : MAX_SAVED) * sizeof fresh[0]);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "%d open reminders from the Mac", n);
    return httpd_resp_sendstr(req, "ok\n");
}

// The Mac asks which reminders were ticked off on the board
static esp_err_t get_done(httpd_req_t *req)
{
    if (!token_ok(req)) return ESP_OK;
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < n_done; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(done_ids[i]));
    xSemaphoreGive(lock);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s);
    free(s);
    return e;
}

void reminders_register(httpd_handle_t server)
{
    lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t len = MAX_SAVED * sizeof rems[0];
    if (nvs_open("demo", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "rems", rems, &len) == ESP_OK && len % sizeof rems[0] == 0) n_rems = len / sizeof rems[0];
        nvs_close(h);
        if (n_rems) ESP_LOGI(TAG, "%d reminders from last time", n_rems);
        len = sizeof done_ids;
        if (nvs_open("demo", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, "rdone", done_ids, &len) == ESP_OK) n_done = len / sizeof done_ids[0];
            nvs_close(h);
        }
    }
    const httpd_uri_t a = {.uri = "/reminders", .method = HTTP_POST, .handler = post_reminders};
    const httpd_uri_t b = {.uri = "/reminders/done", .method = HTTP_GET, .handler = get_done};
    httpd_register_uri_handler(server, &a);
    httpd_register_uri_handler(server, &b);
}
