#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "audio.h"
#include "agent.h"
#include "calendar.h"
#include "spotify.h"
#include "wifi_setup.h"
#ifdef BOBBLE_SIM
#include "bobble_sim.h"
#endif
#include "chat.h"

static const char *TAG = "chat";

#define MAX_FRAME (64 * 1024)
#define MIC_RING (AUDIO_RATE)                 // a second of microphone waiting to go (more: the oldest goes)
#define IDLE_CLOSE_US (120 * 1000000LL)       // a chat nobody's said anything in for 2 minutes closes itself

static SemaphoreHandle_t st_lock, send_lock, mic_lock, mic_ready;
static QueueHandle_t ctl_q;                   // messages for the Mac, sent by the sender task (never the page's loop)
static chat_status_t st;
// The Mac's connection (changed only under send_lock): its socket, whether it gave our key, which one it is (messages
// queued for an earlier connection aren't sent on a later one), and when it last said anything
static int conn = -1;
static bool conn_authed;
static uint32_t conn_gen;
static volatile int64_t conn_heard;
typedef struct { char *s; uint32_t gen; } ctl_t;
EXT_RAM_BSS_ATTR static int16_t mic_ring[MIC_RING];   // (in PSRAM: internal RAM is what Wi-Fi needs)
static int mic_r, mic_n;
static volatile bool awaiting_played;         // speech came in since the last "played"
static volatile int64_t last_activity;

// ---------------------------------------------------------------- sending

static bool send_all(int fd, const void *p, int n)
{
    const char *c = p;
    while (n > 0) {
        int k = send(fd, c, n, 0);
        if (k <= 0) return false;
        c += k;
        n -= k;
    }
    return true;
}

// (the sender task only) One message to the Mac, on the connection that's current (gen: only if it's still that one)
static void send_frame(char type, const void *data, int n, uint32_t gen)
{
    uint8_t h[4] = {(uint8_t)type, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    xSemaphoreTake(send_lock, portMAX_DELAY);
    int fd = conn;
    if (fd >= 0 && conn_authed && gen == conn_gen && !(send_all(fd, h, 4) && send_all(fd, data, n)))
        shutdown(fd, SHUT_RDWR);                      // (the reader notices, and tidies up)
    xSemaphoreGive(send_lock);
}

// Queued for the sender task: the page's buttons and the reader never wait on the network
static void send_json(cJSON *j)
{
    ctl_t c = {cJSON_PrintUnformatted(j), conn_gen};
    cJSON_Delete(j);
    if (!c.s) return;
    if (xQueueSend(ctl_q, &c, 0) != pdTRUE) cJSON_free(c.s);
    else xSemaphoreGive(mic_ready);
}

static void send_msg(const char *t)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", t);
    send_json(j);
}

static void send_flag(const char *t, bool on)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", t);
    cJSON_AddBoolToObject(j, "on", on);
    send_json(j);
}

// The microphones' frames (from the audio task): kept in a ring and sent from our own task, so a slow network never
// holds up the speaker
static void mic_frames(const int16_t *pcm, int n)
{
    if (!st.active || st.muted || !conn_authed) return;
    xSemaphoreTake(mic_lock, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        if (mic_n == MIC_RING) { mic_r = (mic_r + 1) % MIC_RING; mic_n--; }
        mic_ring[(mic_r + mic_n) % MIC_RING] = pcm[i];
        mic_n++;
    }
    xSemaphoreGive(mic_lock);
    xSemaphoreGive(mic_ready);
}

static void sender_task(void *arg)
{
    EXT_RAM_BSS_ATTR static int16_t out[1024];
    int64_t dry_since = 0;
    for (;;) {
        xSemaphoreTake(mic_ready, pdMS_TO_TICKS(50));
        ctl_t c;
        while (xQueueReceive(ctl_q, &c, 0) == pdTRUE) { send_frame('J', c.s, strlen(c.s), c.gen); cJSON_free(c.s); }
        for (;;) {
            xSemaphoreTake(mic_lock, portMAX_DELAY);
            int k = mic_n < 1024 ? mic_n : 1024;
            for (int i = 0; i < k; i++) out[i] = mic_ring[(mic_r + i) % MIC_RING];
            mic_r = (mic_r + k) % MIC_RING;
            mic_n -= k;
            xSemaphoreGive(mic_lock);
            if (!k) break;
            send_frame('A', out, k * 2, conn_gen);
        }
        // the reply has all played (a moment's quiet after the last of it): the Mac goes back to listening
        int64_t now = esp_timer_get_time();
        if (awaiting_played && audio_queued_ms() == 0) {
            if (!dry_since) dry_since = now;
            else if (now - dry_since > 150000) { awaiting_played = false; dry_since = 0; send_msg("played"); }
        } else dry_since = 0;
        // nobody's said anything for a while: close the chat (you're talking, or Claude is: still going)
        if (st.active && (audio_mic_level() > 0.45f || st.state == CS_THINKING || st.state == CS_SPEAKING || awaiting_played))
            last_activity = now;
        if (st.active && now - last_activity > IDLE_CLOSE_US) {
            ESP_LOGI(TAG, "quiet for 2 minutes: closing the chat");
            chat_close();
        }
    }
}

// ---------------------------------------------------------------- the status the page shows

#define ST_EDIT(...) do { xSemaphoreTake(st_lock, portMAX_DELAY); __VA_ARGS__; st.version++; xSemaphoreGive(st_lock); } while (0)

void chat_get(chat_status_t *out)
{
    if (!st_lock) { memset(out, 0, sizeof *out); return; }   // (a board that can't talk)
    xSemaphoreTake(st_lock, portMAX_DELAY);
    *out = st;
    xSemaphoreGive(st_lock);
}

float chat_level(void)
{
    if (!st.active) return 0;
    return st.state == CS_SPEAKING ? audio_out_level() : st.muted ? 0 : audio_mic_level();
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

// ---------------------------------------------------------------- what the Mac asks of the board

static void fmt_time(char *out, size_t cap, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    if (tm.tm_min) snprintf(out, cap, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    else snprintf(out, cap, "%d %s", h, tm.tm_hour < 12 ? "AM" : "PM");
}

// The calendar, as the board has it: today's and tomorrow's events, one a line
static char *agenda_text(void)
{
    EXT_RAM_BSS_ATTR static ical_item_t items[40];
    int n = calendar_items(items, 40);
    size_t cap = 4096, len = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = 0;
    if (!calendar_count()) { snprintf(s, cap, "No calendars are set up (they're added in the Bobble app on the Mac)."); return s; }
    if (!calendar_loaded()) { snprintf(s, cap, "The calendar hasn't loaded yet%s%s.", calendar_error()[0] ? ": " : "", calendar_error()); return s; }
    time_t day0 = calendar_window_start(), now = time(NULL);
    for (int i = 0; i < n && len < cap - 200; i++) {
        if (items[i].end < now - 3600) continue;      // (long over)
        char a[16], b[16];
        fmt_time(a, sizeof a, items[i].start);
        fmt_time(b, sizeof b, items[i].end);
        const char *day = items[i].start >= day0 + 86400 ? "Tomorrow" : "Today";
        if (items[i].start >= day0 + 2 * 86400) continue;
        if (items[i].all_day) len += snprintf(s + len, cap - len, "%s, all day: %s\n", day, items[i].title);
        else len += snprintf(s + len, cap - len, "%s %s-%s: %s%s\n", day, a, b, items[i].title, items[i].free ? " (free)" : "");
    }
    if (!len) snprintf(s, cap, "Nothing on the calendar for the rest of today or tomorrow.");
    return s;
}

static char *music_text(const char *a)
{
    spotify_state_t sp;
    spotify_get(&sp);
    char *s = malloc(400);
    if (!s) return NULL;
    if (!sp.configured) { snprintf(s, 400, "Spotify isn't connected (that's done from the Bobble app on the Mac)."); return s; }
    a = a ? a : "status";
    if (!strcmp(a, "play") && !sp.playing) spotify_command(SP_PLAY_PAUSE);
    else if (!strcmp(a, "pause") && sp.playing) spotify_command(SP_PLAY_PAUSE);
    else if (!strcmp(a, "next")) spotify_command(SP_NEXT);
    else if (!strcmp(a, "previous")) spotify_command(SP_PREV);
    else if (!strcmp(a, "louder")) spotify_command(SP_VOL_UP);
    else if (!strcmp(a, "quieter")) spotify_command(SP_VOL_DOWN);
    if (strcmp(a, "status")) { snprintf(s, 400, "Done (%s).", a); return s; }
    if (!sp.active) snprintf(s, 400, "Nothing is playing on Spotify.");
    else snprintf(s, 400, "%s %s%s%s on %s.%s%s", sp.playing ? "Playing" : "Paused:", sp.title, sp.artist[0] ? " by " : "",
                  sp.artist, sp.device, sp.note[0] ? " Note: " : "", sp.note);
    return s;
}

// ---------------------------------------------------------------- receiving

static void handle_json(int fd, bool *authed, const char *text)
{
    cJSON *j = cJSON_Parse(text);
    if (!j) return;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(j, "s"));
    const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    t = t ? t : "";
    if (*authed && fd != conn) { cJSON_Delete(j); return; }   // (a connection that's been replaced: not listened to)
    if (!*authed) {
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
        if (strcmp(t, "hello") || !key || !agent_key_ok(key)) {
            ESP_LOGW(TAG, "a connection without our key: hanging up");
            shutdown(fd, SHUT_RDWR);
        } else {
            xSemaphoreTake(send_lock, portMAX_DELAY);
            *authed = fd == conn;
            if (*authed) conn_authed = true;
            xSemaphoreGive(send_lock);
            if (!*authed) { cJSON_Delete(j); return; }   // (already replaced by a newer one)
            ST_EDIT(st.mac = true; st.status[0] = 0);
            ESP_LOGI(TAG, "the Mac is here");
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "t", "ready");
            cJSON_AddBoolToObject(r, "active", st.active);
            cJSON_AddBoolToObject(r, "muted", st.muted);
            cJSON_AddBoolToObject(r, "speaker", st.speaker_muted);
            send_json(r);
        }
    } else if (!strcmp(t, "state") && s) {
        int v = !strcmp(s, "listening") ? CS_LISTENING : !strcmp(s, "thinking") ? CS_THINKING : !strcmp(s, "speaking") ? CS_SPEAKING : CS_IDLE;
        ST_EDIT(st.state = v);
    } else if (!strcmp(t, "heard")) {
        ST_EDIT(copy_str(st.heard, sizeof st.heard, txt));
    } else if (!strcmp(t, "say")) {
        ST_EDIT(copy_str(st.said, sizeof st.said, txt));
    } else if (!strcmp(t, "status")) {
        ST_EDIT(copy_str(st.status, sizeof st.status, txt));
    } else if (!strcmp(t, "models")) {
        cJSON *names = cJSON_GetObjectItem(j, "names");
        int cur = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "current"));
        ST_EDIT(
            st.n_models = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, names) {
                if (st.n_models < CHAT_MODELS && cJSON_IsString(it)) copy_str(st.models[st.n_models++], sizeof st.models[0], it->valuestring);
            }
            st.model = cur >= 0 && cur < st.n_models ? cur : 0);
    } else if (!strcmp(t, "flush")) {
        audio_flush();
    } else if (!strcmp(t, "end")) {
        chat_close();
    } else if (!strcmp(t, "ask")) {
        const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(j, "q"));
        char *answer = q && !strcmp(q, "agenda") ? agenda_text()
                     : q && !strcmp(q, "music") ? music_text(cJSON_GetStringValue(cJSON_GetObjectItem(j, "a"))) : NULL;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "t", "reply");
        cJSON_AddItemToObject(r, "id", cJSON_Duplicate(cJSON_GetObjectItem(j, "id"), true));
        cJSON_AddStringToObject(r, "text", answer ? answer : "The board doesn't know how to do that.");
        free(answer);
        send_json(r);
    }
    cJSON_Delete(j);
}

static bool recv_all(int fd, void *p, int n)
{
    char *c = p;
    while (n > 0) {
        int k = recv(fd, c, n, 0);
        if (k <= 0) return false;
        c += k;
        n -= k;
    }
    return true;
}

static void reader_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    bool authed = false;                              // (this connection gave our key)
    uint8_t *buf = malloc(MAX_FRAME + 1);
    uint8_t h[4];
    while (buf && recv_all(fd, h, 4)) {
        int n = h[1] << 16 | h[2] << 8 | h[3];
        if (n > MAX_FRAME || !recv_all(fd, buf, n)) break;
        if (fd == conn) conn_heard = esp_timer_get_time();
        if (h[0] == 'A' && authed && fd == conn) {
            if (st.active) { audio_play((const int16_t *)buf, n / 2); awaiting_played = true; }
        } else if (h[0] == 'J') {
            buf[n] = 0;
            handle_json(fd, &authed, (const char *)buf);
        }
    }
    free(buf);
    xSemaphoreTake(send_lock, portMAX_DELAY);         // (no send halfway through on this socket, and its number
    bool current = conn == fd;                        //  isn't reused while one could be)
    if (current) { conn = -1; conn_authed = false; }
    close(fd);
    xSemaphoreGive(send_lock);
    if (current) {
        ESP_LOGI(TAG, "the Mac went away");
        if (st.active) chat_close();
        ST_EDIT(st.mac = false; st.state = CS_IDLE);
    }
    vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
    int ls = -1;
    for (;;) {
#ifndef BOBBLE_SIM
        if (wifi_state() != WS_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
#endif
        if (ls < 0) {
            ls = socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CHAT_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
            if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) {
                ESP_LOGW(TAG, "can't listen on port %d", CHAT_PORT);
                if (ls >= 0) close(ls);
                ls = -1;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            ESP_LOGI(TAG, "listening for the Mac on port %d", CHAT_PORT);
        }
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        struct timeval tv = {.tv_sec = 30};           // (the Mac says something every 10 s at least: silence is a dead link)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct timeval stv = {.tv_sec = 5};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
        // A new connection replaces the old one, unless the old one is a Mac that's still here (it says something
        // every 10 s): then it's a second Mac, and the first keeps the board
        xSemaphoreTake(send_lock, portMAX_DELAY);
        bool busy = conn >= 0 && conn_authed && esp_timer_get_time() - conn_heard < 15000000;
        int old = busy ? -1 : conn;
        if (!busy) { conn = fd; conn_authed = false; conn_gen++; conn_heard = esp_timer_get_time(); }
        xSemaphoreGive(send_lock);
        if (busy) { ESP_LOGI(TAG, "another Mac is already connected: turning this one away"); close(fd); continue; }
        if (old >= 0) shutdown(old, SHUT_RDWR);
        xTaskCreate(reader_task, "chat_rx", 6144, (void *)(intptr_t)fd, 5, NULL);
    }
}

// ---------------------------------------------------------------- the page's buttons

bool chat_available(void) { return audio_available(); }
int chat_port(void) { return audio_available() ? CHAT_PORT : 0; }

void chat_start(void)
{
    if (!audio_available() || st_lock) return;
    st_lock = xSemaphoreCreateMutex();
    send_lock = xSemaphoreCreateMutex();
    mic_lock = xSemaphoreCreateMutex();
    mic_ready = xSemaphoreCreateBinary();
    ctl_q = xQueueCreate(24, sizeof(ctl_t));
#if CONFIG_SPIRAM_XIP_FROM_PSRAM   // (stack in PSRAM where code runs from there, as on the square: flash writes don't pause it, and internal RAM is short)
    xTaskCreateWithCaps(listen_task, "chat", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(sender_task, "chat_tx", 4096, NULL, 6, NULL, MALLOC_CAP_SPIRAM);
#else
    xTaskCreate(listen_task, "chat", 4096, NULL, 4, NULL);
    xTaskCreate(sender_task, "chat_tx", 4096, NULL, 6, NULL);
#endif
}

void chat_tap_mic(void)
{
    if (!st_lock) return;
    if (!st.mac) { ST_EDIT(copy_str(st.status, sizeof st.status, "Open Bobble on your Mac to talk")); return; }
    if (st.active) {
        bool m = !st.muted;
        ST_EDIT(st.muted = m);
        send_flag("mute", m);
        return;
    }
    if (!audio_start(mic_frames)) { ST_EDIT(copy_str(st.status, sizeof st.status, "The microphone didn't start")); return; }
    audio_set_speaker_mute(st.speaker_muted);
    last_activity = esp_timer_get_time();
    ST_EDIT(st.active = true; st.muted = false; st.state = CS_LISTENING; st.status[0] = 0);
    send_msg("start");
}

void chat_toggle_speaker(void)
{
    if (!st_lock) return;
    bool m = !st.speaker_muted;
    audio_set_speaker_mute(m);
    ST_EDIT(st.speaker_muted = m);
    send_flag("speaker", m);
}

void chat_pick_model(int i)
{
    if (!st_lock || i < 0 || i >= st.n_models) return;
    ST_EDIT(st.model = i);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "t", "model");
    cJSON_AddNumberToObject(j, "i", i);
    send_json(j);
}

void chat_new(void)
{
    if (!st_lock) return;
    audio_flush();
    ST_EDIT(st.heard[0] = st.said[0] = st.status[0] = 0);
    send_msg("new");
}

void chat_close(void)
{
    if (!st_lock || !st.active) return;
    ST_EDIT(st.active = false; st.muted = false; st.state = CS_IDLE);
    awaiting_played = false;
    audio_stop();
    xSemaphoreTake(mic_lock, portMAX_DELAY);
    mic_n = 0;
    xSemaphoreGive(mic_lock);
    send_msg("stop");
}
