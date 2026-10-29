#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
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

#define MAX_FRAME (20 * 1024)                 // (the Mac sends speech in 16000-byte pieces)
#define MIC_RING (AUDIO_RATE)                 // a second of microphone waiting to go (more: the oldest goes)
#define IDLE_CLOSE_US (120 * 1000000LL)       // a chat nobody's said anything in for 2 minutes closes itself

// One task does all the talking on the network (chat_task): it listens, reads what the Mac sends, and sends what's
// queued for it. Its stack and buffers are in PSRAM: internal RAM is what Wi-Fi, TLS and the screen's DMA run on, and
// there's little of it to spare.
static SemaphoreHandle_t st_lock, mic_lock, wake;
static QueueHandle_t ctl_q;                   // messages for the Mac (from the page, the reader, the audio): sent by chat_task
EXT_RAM_BSS_ATTR static chat_status_t st;
// The Mac's connection (chat_task's own): its socket, whether it gave our key, which one it is (messages queued for
// an earlier connection aren't sent on a later one), and when it last said anything
static int conn = -1;
static volatile bool conn_authed;
static volatile uint32_t conn_gen;
static int64_t conn_heard;
typedef struct { char *s; uint32_t gen; } ctl_t;
EXT_RAM_BSS_ATTR static int16_t mic_ring[MIC_RING];
static int mic_r, mic_n;
static volatile bool awaiting_played;         // speech came in since the last "played"
static volatile int64_t last_activity;
static bool paused_music;                     // Spotify was playing when the chat started: we paused it

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

static void drop_conn(void);

// (chat_task only) One message to the Mac, if the connection is still the one it was meant for
static void send_frame(char type, const void *data, int n, uint32_t gen)
{
    if (conn < 0 || !conn_authed || gen != conn_gen) return;
    uint8_t h[4] = {(uint8_t)type, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    if (!send_all(conn, h, 4) || !send_all(conn, data, n)) drop_conn();
}

// Queued for chat_task: the page's buttons never wait on the network
static void send_json(cJSON *j)
{
    ctl_t c = {cJSON_PrintUnformatted(j), conn_gen};
    cJSON_Delete(j);
    if (!c.s) return;
    if (xQueueSend(ctl_q, &c, 0) != pdTRUE) cJSON_free(c.s);
    else xSemaphoreGive(wake);
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

// The microphones' frames (from the audio task): kept in a ring for chat_task, so a slow network never holds up the
// speaker
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
    xSemaphoreGive(wake);
}

// Everything waiting to go: queued messages, then the microphones
static void send_pending(void)
{
    EXT_RAM_BSS_ATTR static int16_t out[1024];
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
}

// The reply has all played (a moment's quiet after the last of it): the Mac goes back to listening. And a chat
// nobody's said anything in for a while closes (you're talking, or Claude is: still going).
static void check_quiet(int64_t now)
{
    static int64_t dry_since;
    if (awaiting_played && audio_queued_ms() == 0) {
        if (!dry_since) dry_since = now;
        else if (now - dry_since > 150000) { awaiting_played = false; dry_since = 0; send_msg("played"); }
    } else dry_since = 0;
    if (st.active && (audio_mic_level() > 0.45f || st.state == CS_THINKING || st.state == CS_SPEAKING || awaiting_played))
        last_activity = now;
    if (st.active && now - last_activity > IDLE_CLOSE_US) {
        ESP_LOGI(TAG, "quiet for 2 minutes: closing the chat");
        chat_close();
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
    EXT_RAM_BSS_ATTR static spotify_state_t sp;      // (chat_task only)
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
    if (strcmp(a, "status")) { paused_music = false; snprintf(s, 400, "Done (%s).", a); return s; }   // (yours now: left as it is after)
    if (!sp.active) snprintf(s, 400, "Nothing is playing on Spotify.");
    else snprintf(s, 400, "%s %s%s%s on %s.%s%s", sp.playing ? "Playing" : "Paused:", sp.title, sp.artist[0] ? " by " : "",
                  sp.artist, sp.device, sp.note[0] ? " Note: " : "", sp.note);
    return s;
}

// ---------------------------------------------------------------- receiving

static void handle_json(int fd, volatile bool *authed, const char *text)
{
    cJSON *j = cJSON_Parse(text);
    if (!j) return;
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(j, "s"));
    const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    t = t ? t : "";
    if (!*authed) {
        const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
        if (strcmp(t, "hello") || !key || !agent_key_ok(key)) {
            ESP_LOGW(TAG, "a connection without our key: hanging up");
            shutdown(fd, SHUT_RDWR);
        } else {
            *authed = conn_authed = true;
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

// The connection's gone (it closed, went quiet, failed a send, or a newer one replaced it)
static int rx_n, rx_len;                      // bytes of the frame coming in so far (its 4-byte header included), its length
static uint8_t *rx_body;

static void drop_conn(void)
{
    if (conn < 0) return;
    bool was = conn_authed;
    close(conn);
    conn = -1;
    conn_authed = false;
    rx_n = 0;
    if (was) {
        ESP_LOGI(TAG, "the Mac went away");
        chat_close();
        ST_EDIT(st.mac = false; st.state = CS_IDLE);
    }
}

// Whatever's arrived, as whole frames; false: the connection's finished
static bool read_some(int64_t now)
{
    static uint8_t hdr[4];
    for (;;) {
        int want = rx_n < 4 ? 4 - rx_n : 4 + rx_len - rx_n;
        uint8_t *dst = rx_n < 4 ? hdr + rx_n : rx_body + (rx_n - 4);
        int k = want ? recv(conn, dst, want, MSG_DONTWAIT) : 0;
        if (want && k == 0) return false;
        if (want && k < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        rx_n += k;
        conn_heard = now;
        if (rx_n == 4) {
            rx_len = hdr[1] << 16 | hdr[2] << 8 | hdr[3];
            if (rx_len > MAX_FRAME) return false;
        }
        if (rx_n < 4 || rx_n < 4 + rx_len) continue;
        rx_n = 0;
        if (hdr[0] == 'A' && conn_authed) {
            if (st.active) { audio_play((const int16_t *)rx_body, rx_len / 2); awaiting_played = true; }
        } else if (hdr[0] == 'J') {
            rx_body[rx_len] = 0;
            handle_json(conn, &conn_authed, (const char *)rx_body);
            if (conn < 0) return true;                 // (it hung up on a bad key)
        }
    }
}

static int listen_on(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (ls >= 0) setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CHAT_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) {
        ESP_LOGW(TAG, "can't listen on port %d", CHAT_PORT);
        if (ls >= 0) close(ls);
        return -1;
    }
    ESP_LOGI(TAG, "listening for the Mac on port %d", CHAT_PORT);
    return ls;
}

static void take_conn(int ls, int64_t now)
{
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) return;
    // A new connection replaces the old one, unless the old one is a Mac that's still here (it says something every
    // 10 s): then it's a second Mac, and the first keeps the board
    if (conn >= 0 && conn_authed && now - conn_heard < 15000000) {
        ESP_LOGI(TAG, "another Mac is already connected: turning this one away");
        close(fd);
        return;
    }
    drop_conn();
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval stv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
    conn = fd;
    conn_gen++;
    conn_heard = now;
}

static void chat_task(void *arg)
{
    rx_body = heap_caps_malloc(MAX_FRAME + 1, MALLOC_CAP_SPIRAM);
    int ls = -1;
    for (;;) {
#ifndef BOBBLE_SIM
        if (wifi_state() != WS_CONNECTED && conn < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
#endif
        if (ls < 0 && (ls = listen_on()) < 0) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        if (conn >= 0) FD_SET(conn, &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        int r = select((conn > ls ? conn : ls) + 1, &rd, NULL, NULL, &tv);
        int64_t now = esp_timer_get_time();
        if (r > 0 && conn >= 0 && FD_ISSET(conn, &rd) && !read_some(now)) drop_conn();
        if (r > 0 && FD_ISSET(ls, &rd)) take_conn(ls, now);
        if (conn >= 0 && now - conn_heard > 30000000) { ESP_LOGW(TAG, "the Mac's gone quiet"); drop_conn(); }
        send_pending();
        check_quiet(now);
        xSemaphoreTake(wake, 0);                       // (select did the waiting)
    }
}

// ---------------------------------------------------------------- the page's buttons

bool chat_available(void) { return audio_available(); }
int chat_port(void) { return audio_available() ? CHAT_PORT : 0; }

void chat_start(void)
{
    if (!audio_available() || st_lock) return;
    st_lock = xSemaphoreCreateMutex();
    mic_lock = xSemaphoreCreateMutex();
    wake = xSemaphoreCreateBinary();
    ctl_q = xQueueCreate(24, sizeof(ctl_t));
#ifdef BOBBLE_SIM
    xTaskCreate(chat_task, "chat", 6144, NULL, 5, NULL);
#else
    xTaskCreatePinnedToCoreWithCaps(chat_task, "chat", 6144, NULL, 5, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
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
    // music from Spotify would keep the speech recognition hearing words after you've stopped (so your turn never
    // ends), and talk over Claude: it's paused for the chat, and carries on after
    spotify_state_t sp;
    spotify_get(&sp);
    paused_music = sp.configured && sp.playing;
    if (paused_music) spotify_command(SP_PLAY_PAUSE);
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
    if (paused_music) { paused_music = false; spotify_command(SP_PLAY_PAUSE); }   // (the music we paused, back on)
    xSemaphoreTake(mic_lock, portMAX_DELAY);
    mic_n = 0;
    xSemaphoreGive(mic_lock);
    send_msg("stop");
}
