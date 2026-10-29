// Agent status from the Mac: each Claude Code session posts its state on every hook event (see agent.h).
// A session that goes quiet is dropped after a while, so a crashed or closed one doesn't stay "working".
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "cJSON.h"
#include <ctype.h>
#include <time.h>
#include "esp_mac.h"
#include "wifi_setup.h"
#include "agent.h"
#include "chat.h"
#include "secrets.h"
#include "esp_attr.h"

static const char *TAG = "agent";

#define MAX_SESS AGENT_MAX
#define PIN_US (30 * 1000000LL)               // a session picked with a tap stays up this long
#define DONE_SHOW_US (90 * 1000000LL)         // "done" shows its bubble this long; then just its logo, greying, till it's closed
#define STALE_US (15 * 60 * 1000000LL)        // no word from a session this long: forget it (not a finished one: its
                                              // logo keeps its green dot till the chat starts again or closes)

typedef struct {
    char id[40];
    agent_state_t state;
    char detail[200], project[32], agent[12], model[24], title[48], tool[24], provider[16];
    int64_t since, heard;
    double sent;            // the Mac's clock (ms) on the newest post: an older one arriving late is ignored
    int nopt, picked;       // the choices it's waiting on (see agent_answer), and the one tapped (-1: none)
    char opt[AGENT_OPTS][32], qid[24];
} sess_t;

// Choices tapped on the display, till the Mac collects them (POST /agent/answers)
#define ANSWERS_MAX 6
typedef struct { char id[40], qid[24]; int choice; } answer_t;
EXT_RAM_BSS_ATTR static answer_t answers[ANSWERS_MAX];
static int n_answers;

EXT_RAM_BSS_ATTR static sess_t sess[MAX_SESS];
static uint32_t attention, activity;
static char pinned[40];                       // the session picked with a tap, until pin_until
static int64_t pin_until;
static SemaphoreHandle_t lock;

static agent_state_t parse_state(const char *s)
{
    if (!strcmp(s, "working")) return AG_WORKING;
    if (!strcmp(s, "waiting")) return AG_WAITING;
    if (!strcmp(s, "done")) return AG_DONE;
    if (!strcmp(s, "stopped")) return AG_STOPPED;
    return AG_IDLE;
}

// ---- pairing: up to MAX_MACS Macs at once (a laptop and a desktop), each with a key of its own. A Mac pairing again
// gets a new key in its own place (it says who it is: "client"); a new Mac past the limit takes the place of the one
// heard from longest ago. Kept in NVS ("agent"/"macs").
#define PAIR_US (120 * 1000000LL)             // a code is good for this long
#define PAIR_TRIES 5                           // wrong codes before it's cancelled
#define MAX_MACS 4
typedef struct { char key[33], client[17], name[32]; uint32_t used; } mac_t;
EXT_RAM_BSS_ATTR static mac_t macs[MAX_MACS];
static int n_macs;
static char pair_code[8];
static bool seen;                             // a chat has come in at some point (kept in NVS: "agent"/"seen")
static int64_t pair_until, paired_at;
static int pair_tries;

static void macs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("agent", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "macs", macs, n_macs * sizeof macs[0]);
    nvs_erase_key(h, "key");                          // (the one-Mac key from before: it's in the list now)
    nvs_commit(h);
    nvs_close(h);
}

// A paired Mac's key, or (development builds only) the development token. A release build
// (idf.py -B build-release -D BOBBLE_RELEASE=1 build) takes only paired keys: the development token is the same in
// every board built from this source, so it would let anyone who has it into a board that's left our hands.
bool agent_token_ok(httpd_req_t *req)
{
    char token[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof token) != ESP_OK) return false;
    return agent_key_ok(token);
}

bool agent_key_ok(const char *token)
{
#ifndef BOBBLE_RELEASE
    if (!strcmp(token, OTA_TOKEN)) return true;
#endif
    for (int i = 0; i < n_macs; i++)
        if (macs[i].key[0] && !strcmp(token, macs[i].key)) { macs[i].used = (uint32_t)time(NULL); return true; }
    return false;
}

// ---- who this board is: an id from its hardware address (the same on every start, different on every board), and
// a name ("Bobble 3F2A" until someone renames it), both in its Bonjour advert so the Mac can tell boards apart
static char dev_id[13], dev_name[32];

const char *agent_device_id(void)
{
    if (!dev_id[0]) {
        uint8_t m[6] = {0};
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        snprintf(dev_id, sizeof dev_id, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return dev_id;
}

const char *agent_device_name(void)
{
    if (!dev_name[0]) {
        nvs_handle_t h;
        size_t len = sizeof dev_name;
        if (nvs_open("agent", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "name", dev_name, &len) != ESP_OK) dev_name[0] = 0;
            nvs_close(h);
        }
        if (!dev_name[0]) {
            const char *id = agent_device_id();
            snprintf(dev_name, sizeof dev_name, "Bobble %c%c%c%c", toupper((int)id[8]), toupper((int)id[9]), toupper((int)id[10]),
                     toupper((int)id[11]));
        }
    }
    return dev_name;
}

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

static esp_err_t pair_start(httpd_req_t *req)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(pair_code, sizeof pair_code, "%06u", (unsigned)(esp_random() % 1000000));
    pair_until = esp_timer_get_time() + PAIR_US;
    pair_tries = 0;
    activity++;                                       // (wakes the screen to show it)
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "pairing: code on screen");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"expires\":120}");
}

static esp_err_t pair_finish(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof body) < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    cJSON *j = cJSON_Parse(body), *c = cJSON_GetObjectItem(j, "code");
    cJSON *cl = cJSON_GetObjectItem(j, "client"), *mn = cJSON_GetObjectItem(j, "mac");
    char code[12] = "", client[17] = "", mac_name[32] = "";
    if (cJSON_IsString(c)) strlcpy(code, c->valuestring, sizeof code);
    if (cJSON_IsString(cl)) strlcpy(client, cl->valuestring, sizeof client);   // (which Mac: older apps don't say)
    if (cJSON_IsString(mn)) strlcpy(mac_name, mn->valuestring, sizeof mac_name);
    cJSON_Delete(j);
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!pair_code[0] || now > pair_until) {
        pair_code[0] = 0;
        xSemaphoreGive(lock);
        httpd_resp_set_status(req, "410 Gone");
        return httpd_resp_sendstr(req, "{\"error\":\"no code is showing: start again\"}");
    }
    if (strcmp(code, pair_code)) {
        if (++pair_tries >= PAIR_TRIES) pair_code[0] = 0;   // too many guesses: that code's gone
        xSemaphoreGive(lock);
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"wrong code\"}");
    }
    int k = -1;                                       // this Mac's place: its old one, a free one, or the least used
    for (int i = 0; i < n_macs && client[0]; i++) if (!strcmp(macs[i].client, client)) k = i;
    if (k < 0 && n_macs < MAX_MACS) k = n_macs++;
    if (k < 0) { k = 0; for (int i = 1; i < n_macs; i++) if (macs[i].used < macs[k].used) k = i; }
    mac_t *m = &macs[k];
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 16; i++) snprintf(m->key + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xff));
    strlcpy(m->client, client, sizeof m->client);
    strlcpy(m->name, mac_name, sizeof m->name);
    m->used = (uint32_t)time(NULL);
    pair_code[0] = 0;
    paired_at = now;
    activity++;
    char reply[160];
    snprintf(reply, sizeof reply, "{\"key\":\"%.32s\",\"id\":\"%.12s\"}", m->key, agent_device_id());
    macs_save();
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "paired with %s (%d Mac%s)", mac_name[0] ? mac_name : "a Mac", n_macs, n_macs == 1 ? "" : "s");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

bool agent_pairing(char code[8])
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (pair_code[0] && esp_timer_get_time() > pair_until) pair_code[0] = 0;
    bool on = pair_code[0];
    if (on && code) strcpy(code, pair_code);
    xSemaphoreGive(lock);
    return on;
}

// The code card goes away: tapped on the display, or the app's dialog was cancelled (POST /pair/cancel)
void agent_pair_cancel(void)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    pair_code[0] = 0;
    xSemaphoreGive(lock);
}

static esp_err_t pair_cancel(httpd_req_t *req)
{
    agent_pair_cancel();
    ESP_LOGI(TAG, "pairing: cancelled");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

bool agent_just_paired(void) { return paired_at && esp_timer_get_time() - paired_at < 3000000; }

bool agent_known(void) { return seen || n_macs; }

#ifndef BOBBLE_BUILD
#define BOBBLE_BUILD 0LL
#endif

// GET /info: who this board is (anyone on the network may ask: it's what the Bonjour advert says anyway)
static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", agent_device_id());
    cJSON_AddStringToObject(j, "name", agent_device_name());
    cJSON_AddNumberToObject(j, "macs", n_macs);
    cJSON_AddNumberToObject(j, "fw", (double)BOBBLE_BUILD);   // which release build (0: an everyday build)
    if (chat_port()) cJSON_AddNumberToObject(j, "chat", chat_port());   // it can talk: where the Mac connects for that
#ifdef BOBBLE_RELEASE
    cJSON_AddBoolToObject(j, "release", true);
#else
    cJSON_AddBoolToObject(j, "release", false);
#endif
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    cJSON_Delete(j);
    return r;
}

// POST /name {"name": "Alex's Bobble"} from a paired Mac: what it's called (in the app, and its Bonjour advert)
static esp_err_t name_post(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    char body[160];
    if (read_body(req, body, sizeof body) < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    cJSON *j = cJSON_Parse(body), *n = cJSON_GetObjectItem(j, "name");
    agent_set_name(cJSON_IsString(n) ? n->valuestring : "");
    cJSON_Delete(j);
    return info_get(req);
}

// Renamed in the app (POST /name) or on the board (Settings > Battery)
void agent_set_name(const char *in)
{
    char name[32];
    int k = 0;
    for (const char *p = in; *p && k < (int)sizeof name - 1; p++) if (*p >= 32 && *p < 127) name[k++] = *p;   // (printable
    while (k && name[k - 1] == ' ') k--;                                                                      // ASCII: the
    name[k] = 0;                                                                                              // font's)
    nvs_handle_t h;
    if (nvs_open("agent", NVS_READWRITE, &h) == ESP_OK) {
        if (name[0]) nvs_set_str(h, "name", name);
        else nvs_erase_key(h, "name");               // (empty: back to "Bobble 3F2A")
        nvs_commit(h);
        nvs_close(h);
    }
    dev_name[0] = 0;
    wifi_set_name(agent_device_name());
    ESP_LOGI(TAG, "named \"%s\"", agent_device_name());
}

// POST /agent/answers {"sessions": ["…", …]}: the choices tapped for these chats (the asking Mac's own), as
// [{"session", "qid", "choice"}], handed over once
static esp_err_t answers_post(httpd_req_t *req)
{
    if (!agent_token_ok(req)) { httpd_resp_set_status(req, "403 Forbidden"); return httpd_resp_sendstr(req, "bad token\n"); }
    char body[1024];
    if (read_body(req, body, sizeof body) < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    cJSON *j = cJSON_Parse(body), *ids = cJSON_GetObjectItem(j, "sessions"), *out = cJSON_CreateArray();
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < n_answers; i++) {
        bool ours = false;
        cJSON *id;
        cJSON_ArrayForEach(id, ids) if (cJSON_IsString(id) && !strncmp(id->valuestring, answers[i].id, sizeof answers[i].id - 1)) ours = true;
        if (!ours) continue;
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "session", answers[i].id);
        cJSON_AddStringToObject(a, "qid", answers[i].qid);
        cJSON_AddNumberToObject(a, "choice", answers[i].choice);
        cJSON_AddItemToArray(out, a);
        answers[i--] = answers[--n_answers];
    }
    xSemaphoreGive(lock);
    cJSON_Delete(j);
    char *s = cJSON_PrintUnformatted(out);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    cJSON_Delete(out);
    return r;
}

static esp_err_t agent_post(httpd_req_t *req)
{
    char body[1024];
    if (!agent_token_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "bad token\n");
    }
    if (read_body(req, body, sizeof body) < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    if (!seen) {                                      // the first chat ever: set up (the welcome's done)
        seen = true;
        nvs_handle_t h;
        if (nvs_open("agent", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "seen", 1); nvs_commit(h); nvs_close(h); }
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *id = cJSON_GetObjectItem(j, "session"), *st = cJSON_GetObjectItem(j, "state"), *de = cJSON_GetObjectItem(j, "detail");
    cJSON *ts = cJSON_GetObjectItem(j, "t");          // optional, ms
    cJSON *pr = cJSON_GetObjectItem(j, "project");    // optional, and kept from earlier posts when left out:
    cJSON *ai = cJSON_GetObjectItem(j, "agent"), *mo = cJSON_GetObjectItem(j, "model"), *ti = cJSON_GetObjectItem(j, "title"),
          *to = cJSON_GetObjectItem(j, "tool"), *pv = cJSON_GetObjectItem(j, "provider");     // (sent every time: "" when it isn't using one)
    cJSON *op = cJSON_GetObjectItem(j, "options"), *qi = cJSON_GetObjectItem(j, "qid");   // (waiting on a choice you can tap)
    if (!cJSON_IsString(id) || !cJSON_IsString(st)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need session and state"); }

    int64_t now = esp_timer_get_time();
    agent_state_t state = parse_state(st->valuestring);
    xSemaphoreTake(lock, portMAX_DELAY);
    sess_t *s = NULL, *oldest = &sess[0];
    for (int i = 0; i < MAX_SESS; i++) {
        if (!strncmp(sess[i].id, id->valuestring, sizeof sess[i].id - 1)) { s = &sess[i]; break; }
        if (sess[i].heard < oldest->heard) oldest = &sess[i];
    }
    if (!s) {                                       // new session: take the free (or longest-quiet) slot
        s = oldest;
        memset(s, 0, sizeof *s);
        strlcpy(s->id, id->valuestring, sizeof s->id);
    }
    double sent = cJSON_IsNumber(ts) ? ts->valuedouble : 0;
    if (sent && sent < s->sent) {                   // overtaken by a newer post (each hook posts in the background)
        xSemaphoreGive(lock);
        cJSON_Delete(j);
        return httpd_resp_sendstr(req, "stale\n");
    }
    if (sent) s->sent = sent;
    if (state != s->state) {
        s->since = now;
        if (state == AG_WAITING || state == AG_DONE) attention++;
        if (state == AG_WAITING) pin_until = 0;      // needs you: comes to the front, whatever was picked
    }
    const char *words = cJSON_IsString(de) ? de->valuestring : "";
    if (state != AG_STOPPED && (state != s->state || strncmp(words, s->detail, sizeof s->detail - 1))) activity++;   // (you stopped it yourself: no need to wake the screen)
    s->state = state;
    s->heard = now;
    strlcpy(s->detail, words, sizeof s->detail);
    if (cJSON_IsString(pr)) strlcpy(s->project, pr->valuestring, sizeof s->project);
    if (cJSON_IsString(ai) && ai->valuestring[0]) strlcpy(s->agent, ai->valuestring, sizeof s->agent);
    if (cJSON_IsString(mo) && mo->valuestring[0]) strlcpy(s->model, mo->valuestring, sizeof s->model);
    if (cJSON_IsString(ti) && ti->valuestring[0]) strlcpy(s->title, ti->valuestring, sizeof s->title);
    if (cJSON_IsString(pv) && pv->valuestring[0]) strlcpy(s->provider, pv->valuestring, sizeof s->provider);
    strlcpy(s->tool, cJSON_IsString(to) ? to->valuestring : "", sizeof s->tool);
    const char *q = state == AG_WAITING && cJSON_IsString(qi) ? qi->valuestring : "";
    if (strncmp(q, s->qid, sizeof s->qid - 1)) s->picked = -1;   // a new question: nothing tapped yet
    strlcpy(s->qid, q, sizeof s->qid);
    s->nopt = 0;
    if (q[0] && cJSON_IsArray(op)) {
        cJSON *o;
        cJSON_ArrayForEach(o, op) if (cJSON_IsString(o) && s->nopt < AGENT_OPTS) strlcpy(s->opt[s->nopt++], o->valuestring, sizeof s->opt[0]);
    }
    if (!s->nopt) s->picked = -1;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "%.8s: %s %s", id->valuestring, st->valuestring, cJSON_IsString(de) ? de->valuestring : "");
    cJSON_Delete(j);
    return httpd_resp_sendstr(req, "ok\n");
}

void agent_register(httpd_handle_t server)
{
    if (!lock) lock = xSemaphoreCreateMutex();
    nvs_handle_t h;                                   // the paired Macs' keys
    if (nvs_open("agent", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof macs;
        if (nvs_get_blob(h, "macs", macs, &len) == ESP_OK) n_macs = (int)(len / sizeof macs[0]);
        len = sizeof macs[0].key;
        if (!n_macs && nvs_get_str(h, "key", macs[0].key, &len) == ESP_OK && macs[0].key[0]) n_macs = 1;   // (paired before there could be several)
        uint8_t v = 0;
        seen = nvs_get_u8(h, "seen", &v) == ESP_OK && v;
        nvs_close(h);
    }
    const httpd_uri_t u[] = {
        {.uri = "/agent", .method = HTTP_POST, .handler = agent_post},
        {.uri = "/agent/answers", .method = HTTP_POST, .handler = answers_post},
        {.uri = "/pair/start", .method = HTTP_POST, .handler = pair_start},
        {.uri = "/pair/finish", .method = HTTP_POST, .handler = pair_finish},
        {.uri = "/pair/cancel", .method = HTTP_POST, .handler = pair_cancel},
        {.uri = "/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/name", .method = HTTP_POST, .handler = name_post},
    };
    for (int i = 0; i < (int)(sizeof u / sizeof u[0]); i++) httpd_register_uri_handler(server, &u[i]);
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static const int rank[] = {[AG_IDLE] = 0, [AG_STOPPED] = 1, [AG_DONE] = 1, [AG_WORKING] = 2, [AG_WAITING] = 3};

// (lock held) Drop forgotten sessions, let "done" lapse into idle; the shown one: the picked one while its pin
// lasts, else the most urgent (among equals, the one that changed last)
static const sess_t *refresh(int64_t now)
{
    const sess_t *best = NULL, *pick = NULL;
    for (int i = 0; i < MAX_SESS; i++) {
        sess_t *s = &sess[i];
        if (!s->id[0]) continue;
        if (now - s->heard > STALE_US && s->state != AG_DONE) { s->id[0] = 0; continue; }
        if (s->state == AG_IDLE) continue;
        if (now < pin_until && !strcmp(s->id, pinned)) pick = s;   // (tapped: even a finished one's bubble comes back)
        if (s->state == AG_DONE && now - s->since > DONE_SHOW_US) continue;   // finished a while ago: logo only
        if (!best || rank[s->state] > rank[best->state] || (s->state == best->state && s->since > best->since)) best = s;
    }
    return pick ? pick : best;
}

void agent_get(agent_status_t *out)
{
    memset(out, 0, sizeof *out);
    if (!lock) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    const sess_t *shown = refresh(now);
    for (int i = 0; i < MAX_SESS; i++) {
        const sess_t *s = &sess[i];
        if (!s->id[0] || s->state == AG_IDLE) continue;
        if (s->state == AG_WORKING) out->working++;
        if (s == shown) out->dot_shown = out->n;
        out->dot_slot[out->n] = i;
        strlcpy(out->dot_agent[out->n], s->agent, sizeof out->dot_agent[0]);
        float fade;                                   // in use: none; quiet for minutes, or finished/stopped: more and more
        if (s->state == AG_WORKING || s->state == AG_WAITING) fade = 0.8f * clamp01(((now - s->heard) / 60e6f - 5) / 10);   // (after 5 quiet minutes)
        else fade = 0.45f + 0.55f * clamp01((now - s->since) / 900e6f);   // (over 15 minutes)
        out->dot_fade[out->n] = (uint8_t)(fade * 15 + 0.5f);
        int64_t done_for = now - s->since;               // (just finished: its logo bobs for a moment)
        if (s->state == AG_DONE && done_for < 5000000) out->dot_done[out->n] = 1 + (uint8_t)(done_for / 100000);
        out->dots[out->n++] = s->state;
    }
    if (shown) {
        out->state = shown->state;
        out->slot = (int)(shown - sess);
        strlcpy(out->detail, shown->detail, sizeof out->detail);
        strlcpy(out->project, shown->project, sizeof out->project);
        strlcpy(out->agent, shown->agent, sizeof out->agent);
        strlcpy(out->model, shown->model, sizeof out->model);
        strlcpy(out->title, shown->title, sizeof out->title);
        strlcpy(out->tool, shown->tool, sizeof out->tool);
        strlcpy(out->provider, shown->provider[0] ? shown->provider : shown->agent, sizeof out->provider);
        out->since = shown->since;
    }
    out->attention = attention;
    out->activity = activity;
    xSemaphoreGive(lock);
}

int agent_get_chats(agent_chat_t out[AGENT_MAX])
{
    if (!lock) return 0;
    int64_t now = esp_timer_get_time(), n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    refresh(now);                                     // (forgets stale ones)
    for (int i = 0; i < MAX_SESS; i++) {
        const sess_t *s = &sess[i];
        if (!s->id[0] || s->state == AG_IDLE) continue;
        agent_chat_t *c = &out[n++];
        c->state = s->state;
        c->slot = i;
        c->since = s->since;
        c->bubble = !(s->state == AG_DONE && now - s->since > DONE_SHOW_US);
        strlcpy(c->detail, s->detail, sizeof c->detail);
        strlcpy(c->title, s->title, sizeof c->title);
        strlcpy(c->project, s->project, sizeof c->project);
        strlcpy(c->agent, s->agent, sizeof c->agent);
        strlcpy(c->model, s->model, sizeof c->model);
        c->nopt = s->nopt;
        memcpy(c->opt, s->opt, sizeof c->opt);
        c->picked = s->picked;
    }
    xSemaphoreGive(lock);
    return (int)n;
}

void agent_select_next(void)
{
    if (!lock) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    const sess_t *shown = refresh(now);
    int at = shown ? (int)(shown - sess) : -1;
    for (int k = 1; k <= MAX_SESS; k++) {             // the next one along that has something to show
        const sess_t *s = &sess[(at + k + MAX_SESS) % MAX_SESS];
        if (!s->id[0] || s->state == AG_IDLE || s == shown) continue;
        strlcpy(pinned, s->id, sizeof pinned);
        pin_until = now + PIN_US;
        activity++;                                   // (counts as news: keeps the screen on)
        break;
    }
    xSemaphoreGive(lock);
}

void agent_select_slot(int slot)
{
    if (!lock || slot < 0 || slot >= MAX_SESS) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    refresh(now);
    if (sess[slot].id[0] && sess[slot].state != AG_IDLE) {
        strlcpy(pinned, sess[slot].id, sizeof pinned);
        pin_until = now + PIN_US;
        activity++;
    }
    xSemaphoreGive(lock);
}

void agent_answer(int slot, int choice)
{
    if (!lock || slot < 0 || slot >= MAX_SESS) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    sess_t *s = &sess[slot];
    if (s->id[0] && s->state == AG_WAITING && choice >= 0 && choice < s->nopt && s->picked < 0) {
        s->picked = choice;
        if (n_answers == ANSWERS_MAX) answers[0] = answers[--n_answers];   // (full: the oldest goes)
        answer_t *a = &answers[n_answers++];
        strlcpy(a->id, s->id, sizeof a->id);
        strlcpy(a->qid, s->qid, sizeof a->qid);
        a->choice = choice;
        activity++;
        ESP_LOGI(TAG, "%.8s: answered \"%s\"", s->id, s->opt[choice]);
    }
    xSemaphoreGive(lock);
}
