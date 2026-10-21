// wifi_setup.c's job on the Mac, through the same calls: the Mac's own connection plays the Wi-Fi. Networks to pick from
// are pretend ones; joining any of them takes a moment, then the emulator is "online" (the network is really the Mac's).
// Remembered networks are kept in NVS as the board keeps them, so a factory reset brings back the setup screens. The web
// server is the board's, on the emulator's port, on this Mac only.
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include "wifi_setup.h"
#include "ota.h"
#include "agent.h"
#include "calendar.h"
#include "spotify.h"
#include "reminders.h"
#include "remote_log.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "sim_port.h"
#include "bobble_sim.h"
#include "gfx.h"

static const char *TAG = "wifi";

static volatile wifi_state_t state = WS_PORTAL;
static char sta_ssid[33], ap_ssid[33] = "Bobble-Setup-51M0", ap_pass[9];
static volatile bool scanning, last_failed, suspended;
static int64_t scan_done_at, join_done_at;
static httpd_handle_t httpd;

#define MAX_SAVED 5
typedef struct { char ssid[33], pass[65]; } saved_net_t;   // (the board's layout, in NVS "demo"/"wifinets")
static saved_net_t saved[MAX_SAVED];
static int n_saved;

static const wifi_net_t pretend[] = {
    {"Simulator Wi-Fi", -48, true}, {"Office", -61, true}, {"Coffee Shop Guest", -70, false}, {"Neighbours 5G", -79, true},
};

static void saved_store(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wifinets", saved, n_saved * sizeof saved[0]);
    nvs_commit(h);
    nvs_close(h);
}

static void saved_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof saved;
    if (nvs_open("demo", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "wifinets", saved, &len) == ESP_OK) n_saved = (int)(len / sizeof saved[0]);
        nvs_close(h);
    }
}

static void remember(const char *ssid, const char *pass)
{
    saved_net_t n = {0};
    strlcpy(n.ssid, ssid, sizeof n.ssid);
    strlcpy(n.pass, pass, sizeof n.pass);
    int k = 0;
    for (int i = 0; i < n_saved; i++) if (strcmp(saved[i].ssid, ssid)) saved[k++] = saved[i];
    n_saved = k < MAX_SAVED ? k : MAX_SAVED - 1;
    memmove(saved + 1, saved, n_saved * sizeof saved[0]);
    saved[0] = n;
    n_saved++;
    saved_store();
}

// ---- found by this Mac's Bobble app only: no Bonjour (other people's apps on the network took the simulator for their
// board). While online it writes its port to ~/Library/Application Support/BobbleSim/port, and the app looks on 127.0.0.1.
static void advertise(bool on)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/port", sim_support_dir());
    if (!on) { unlink(path); return; }
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", sim_http_port()); fclose(f); }
    ESP_LOGI(TAG, "online on 127.0.0.1:%d (for this Mac's Bobble app)", sim_http_port());
}

// Joins and scans finish a moment after they start (checked whenever the firmware asks how things stand)
static void tick(void)
{
    int64_t now = esp_timer_get_time();
    if (scanning && now >= scan_done_at) scanning = false;
    if (state == WS_CONNECTING && !suspended && join_done_at && now >= join_done_at) {
        join_done_at = 0;
        state = WS_CONNECTED;
        last_failed = false;
        ESP_LOGI(TAG, "joined %s", sta_ssid);
        advertise(true);
    }
}

wifi_state_t wifi_state(void) { tick(); return state; }
const char *wifi_ssid(void) { return sta_ssid; }
const char *wifi_ip(void) { tick(); return state == WS_CONNECTED ? "127.0.0.1" : ""; }
const char *wifi_ap_ssid(void) { return ap_ssid; }
const char *wifi_ap_pass(void) { return ap_pass; }
int wifi_ap_clients(void) { return 0; }
bool wifi_ap_active(void) { tick(); return state == WS_PORTAL && !suspended; }
bool wifi_time_synced(void) { return true; }          // (the Mac's clock)
bool wifi_last_failed(void) { return last_failed; }
void wifi_pause_retries(bool pause) {}
bool wifi_scanning(void) { tick(); return scanning; }

void wifi_scan(void)
{
    scanning = true;
    scan_done_at = esp_timer_get_time() + 1200000;
}

int wifi_networks(wifi_net_t *out, int max)
{
    tick();
    int n = 0;
    for (int i = 0; i < (int)(sizeof pretend / sizeof *pretend) && n < max; i++) out[n++] = pretend[i];
    return n;
}

static void connect_to(const char *ssid)
{
    strlcpy(sta_ssid, ssid, sizeof sta_ssid);
    state = WS_CONNECTING;
    join_done_at = esp_timer_get_time() + 1500000;
}

void wifi_join(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "joining %s", ssid);
    remember(ssid, pass);                            // (any password works here)
    connect_to(ssid);
}

void wifi_forget(void)
{
    ESP_LOGI(TAG, "forgetting %s", sta_ssid);
    int k = 0;
    for (int i = 0; i < n_saved; i++) if (strcmp(saved[i].ssid, sta_ssid)) saved[k++] = saved[i];
    n_saved = k;
    saved_store();
    sta_ssid[0] = 0;
    state = WS_PORTAL;
    advertise(false);
}

void wifi_suspend(bool on)
{
    if (on == suspended) return;
    suspended = on;
    ESP_LOGI(TAG, "radio %s", on ? "off" : "on");
    if (on) { state = WS_CONNECTING; advertise(false); return; }
    if (sta_ssid[0]) connect_to(sta_ssid);
    else state = WS_PORTAL;
}

void wifi_set_name(const char *name) {}             // (no network advert here: the app looks on 127.0.0.1)

// ---- start-up
static esp_err_t status_get(httpd_req_t *req)
{
    char out[160];
    snprintf(out, sizeof out, "{\"state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\"}", wifi_state(), sta_ssid, wifi_ip());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// (emulator only) GET /sim/glass: what's on the glass right now, W x H big-endian RGB565 (466 round, 480 square), for previews and tests
static esp_err_t glass_get(httpd_req_t *req)
{
    static uint16_t px[W * H];
    sim_copy_glass(px, 0xffffffff);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)px, sizeof px);
}

// (emulator only) POST /sim/input?touch=x,y | touch=up | boot=1|0 | tilt=degrees | flat=1: drive it from a script
static esp_err_t input_post(httpd_req_t *req)
{
    char q[96] = "", v[32];
    httpd_req_get_url_query_str(req, q, sizeof q);
    int x, y;
    if (httpd_query_key_value(q, "touch", v, sizeof v) == ESP_OK) {
        if (sscanf(v, "%d,%d", &x, &y) == 2) sim_touch(x, y, true);
        else sim_touch(0, 0, false);
    }
    if (httpd_query_key_value(q, "boot", v, sizeof v) == ESP_OK) sim_boot_button(atoi(v));
    if (httpd_query_key_value(q, "tilt", v, sizeof v) == ESP_OK) sim_set_tilt((float)atof(v), false);
    if (httpd_query_key_value(q, "flat", v, sizeof v) == ESP_OK) sim_set_tilt(0, true);
    return httpd_resp_sendstr(req, "ok\n");
}

static void start_web(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    if (httpd_start(&httpd, &cfg) != ESP_OK) return;
    const httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_get};
    httpd_register_uri_handler(httpd, &status);
    const httpd_uri_t glass = {.uri = "/sim/glass", .method = HTTP_GET, .handler = glass_get};
    httpd_register_uri_handler(httpd, &glass);
    const httpd_uri_t input = {.uri = "/sim/input", .method = HTTP_POST, .handler = input_post};
    httpd_register_uri_handler(httpd, &input);
    ota_register(httpd);
    spotify_register(httpd);
    reminders_register(httpd);
    remote_log_register(httpd);
    agent_register(httpd);
    calendar_register(httpd);
}

static void load_ap_password(void)
{
    nvs_handle_t h;
    size_t len = sizeof ap_pass;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, "ap_pass", ap_pass, &len) != ESP_OK || strlen(ap_pass) != 8) {
        for (int i = 0; i < 8; i++) ap_pass[i] = '0' + esp_random() % 10;
        ap_pass[8] = 0;
        nvs_set_str(h, "ap_pass", ap_pass);
        nvs_commit(h);
    }
    nvs_close(h);
}

void wifi_setup_start(void)
{
    load_ap_password();
    saved_load();
    setenv("TZ", "MST7MDT,M3.2.0,M11.1.0", 1);       // (the board's time zone, as wifi_setup.c sets it)
    tzset();
    start_web();
    if (n_saved) connect_to(saved[0].ssid);
    else ESP_LOGI(TAG, "no network remembered: setup");
}
