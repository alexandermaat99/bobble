// Sim racing: the lap delta screen, fed by POST /race (see race.h)
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "gfx.h"
#include "agent.h"
#include "race.h"

static const char *TAG = "race";
extern const font_t font_race;   // (font_race.c: Inter 600 at 128 px, sign, digits and the point)

#define ACTIVE_US 4000000LL       // the screen stays up this long after the last post
#define BAR_FULL 1.0f             // seconds of delta that fill the bar to one side

typedef struct {
    float delta, last, best;      // s (last / best 0: none yet)
    bool valid, has_delta;        // the lap still counts; a delta came in at all
    int lap, laps, pos;           // 0: unknown
    char track[40];
} race_t;

static SemaphoreHandle_t lock;
static race_t in, shown;          // the latest post; what's on the glass (copied once a frame, in race_dirty)
static int64_t heard;
static unsigned posts, posts_shown;

static int read_body(httpd_req_t *req, char *body, int size)
{
    if (req->content_len <= 0 || req->content_len >= size) return -1;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return -1;
        got += n;
    }
    body[got] = 0;
    return got;
}

static esp_err_t race_post(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    char body[512];
    if (read_body(req, body, sizeof body) < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    cJSON *j = cJSON_Parse(body);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); }
    race_t r = {.valid = true};
    cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "delta"))) { r.delta = (float)v->valuedouble; r.has_delta = true; }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "valid"))) r.valid = cJSON_IsTrue(v);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "lap"))) r.lap = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "laps"))) r.laps = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "pos"))) r.pos = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "last"))) r.last = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "best"))) r.best = (float)v->valuedouble;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "track"))) strlcpy(r.track, v->valuestring, sizeof r.track);
    cJSON_Delete(j);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!heard || esp_timer_get_time() - heard > ACTIVE_US) ESP_LOGI(TAG, "telemetry: on track%s%s", r.track[0] ? " at " : "", r.track);
    in = r;
    heard = esp_timer_get_time();
    posts++;
    xSemaphoreGive(lock);
    return httpd_resp_sendstr(req, "ok\n");
}

void race_register(httpd_handle_t server)
{
    if (!lock) lock = xSemaphoreCreateMutex();
    static const httpd_uri_t u = {.uri = "/race", .method = HTTP_POST, .handler = race_post};
    httpd_register_uri_handler(server, &u);
}

bool race_active(void)
{
    int64_t h = heard;
    return h && esp_timer_get_time() - h < ACTIVE_US;
}

bool race_dirty(void)
{
    if (!lock || posts == posts_shown) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    race_t r = in;
    posts_shown = posts;
    xSemaphoreGive(lock);
    bool changed = memcmp(&r, &shown, sizeof r) != 0;
    shown = r;
    return changed;
}

// ---- drawing

#define C_GREEN rgb(48, 209, 88)
#define C_RED rgb(255, 69, 58)
#define C_AMBER rgb(255, 176, 32)
#define C_GREY rgb(142, 142, 150)
#define C_DIM rgb(70, 70, 76)

static void text_mid(uint16_t *buf, int y0, int h, int cx, int y, const font_t *f, const char *s, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, s) / 2, y, f, s, col);
}

static void lap_time(char *out, int n, float s)
{
    if (s <= 0) { snprintf(out, n, "-:--.---"); return; }
    int ms = (int)lroundf(s * 1000);
    snprintf(out, n, "%d:%02d.%03d", ms / 60000, ms / 1000 % 60, ms % 1000);
}

// The big number, with every digit in a cell as wide as a "0" (so it doesn't wobble as it counts)
static void big_number(uint16_t *buf, int y0, int h, int y, const char *s, uint16_t col)
{
    int cell = ftext_width(&font_race, "0"), w = 0;
    for (const char *p = s; *p; p++) w += (*p >= '0' && *p <= '9') ? cell : ftext_width_n(&font_race, p, 1);
    int x = CX - w / 2;
    for (const char *p = s; *p; p++) {
        int cw = ftext_width_n(&font_race, p, 1), adv = (*p >= '0' && *p <= '9') ? cell : cw;
        draw_ftext_n(buf, y0, h, x + (adv - cw) / 2, y, &font_race, p, 1, col);
        x += adv;
    }
}

// Where things sit: the square fills its corners; the round glass keeps everything inside the circle
#if W == 466
enum { Y_TRACK = 46, Y_LAP = 74, Y_BAR = 140, BAR_HALF = 175, Y_BIG = 160, Y_NOTE = 312, Y_LBL = 350, Y_VAL = 372, COL_DX = 92 };
#define F_VAL font_body
#else
enum { Y_TRACK = 50, Y_LAP = 80, Y_BAR = 152, BAR_HALF = 190, Y_BIG = 178, Y_NOTE = 336, Y_LBL = 386, Y_VAL = 410, COL_DX = 110 };
#define F_VAL font_big
#endif

void race_draw(uint16_t *buf, int y0, int h)
{
    const race_t *r = &shown;
    char s[40];

    if (r->track[0]) text_mid(buf, y0, h, CX, Y_TRACK, &font_label, r->track, C_GREY);
    s[0] = 0;
    if (r->lap > 0 && r->laps > 0) snprintf(s, sizeof s, "Lap %d/%d", r->lap, r->laps);
    else if (r->lap > 0) snprintf(s, sizeof s, "Lap %d", r->lap);
    if (r->pos > 0) snprintf(s + strlen(s), sizeof s - strlen(s), "%sP%d", s[0] ? "   " : "", r->pos);
    if (s[0]) text_mid(buf, y0, h, CX, Y_LAP, &font_body, s, rgb(255, 255, 255));

    bool have = r->has_delta && r->best > 0;           // (no best lap yet: nothing to compare with)
    float d = have ? r->delta : 0;
    bool even = fabsf(d) < 0.05f;
    uint16_t col = !have ? C_DIM : !r->valid ? C_GREY : even ? rgb(255, 255, 255) : d < 0 ? C_GREEN : C_RED;

    // the bar: from the middle, left when you're up on your best, right when you're down
    const int bx0 = CX - BAR_HALF, bx1 = CX + BAR_HALF, by = Y_BAR, bh = 12;
    aa_rrect(buf, y0, h, bx0, by, bx1, by + bh, bh / 2, rgb(36, 36, 40));
    if (have && !even) {
        float k = fminf(fabsf(d) / BAR_FULL, 1.0f) * (bx1 - CX);
        if (d < 0) aa_rrect(buf, y0, h, CX - k, by, CX + bh / 2, by + bh, bh / 2, col);
        else aa_rrect(buf, y0, h, CX - bh / 2, by, CX + k, by + bh, bh / 2, col);
    }
    fill_rect(buf, y0, h, CX - 1, by - 6, CX + 1, by + bh + 6, rgb(255, 255, 255));

    if (!have) snprintf(s, sizeof s, "-.---");
    else if (fabsf(d) < 0.0005f) snprintf(s, sizeof s, "0.000");
    else if (fabsf(d) < 10) snprintf(s, sizeof s, "%+.3f", d);
    else snprintf(s, sizeof s, "%+.1f", d);
    big_number(buf, y0, h, Y_BIG, s, col);

    const char *note = !r->best ? "Set a lap to compare" : !r->valid ? "Lap invalid" : "to best lap";
    text_mid(buf, y0, h, CX, Y_NOTE, &font_label, note, !r->valid && r->best ? C_AMBER : C_GREY);

    // last and best laps along the bottom
    text_mid(buf, y0, h, CX - COL_DX, Y_LBL, &font_label, "LAST", C_GREY);
    text_mid(buf, y0, h, CX + COL_DX, Y_LBL, &font_label, "BEST", C_GREY);
    lap_time(s, sizeof s, r->last);
    text_mid(buf, y0, h, CX - COL_DX, Y_VAL, &F_VAL, s, r->last > 0 ? rgb(255, 255, 255) : C_DIM);
    lap_time(s, sizeof s, r->best);
    text_mid(buf, y0, h, CX + COL_DX, Y_VAL, &F_VAL, s, r->best > 0 ? rgb(170, 120, 255) : C_DIM);   // (purple, like a best)
}
