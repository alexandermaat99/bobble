// WiFi provisioning: a WPA2 setup hotspot with a captive-portal web page that lists nearby
// networks. Up to 5 networks that have worked are remembered (NVS); when the current one can't be
// found, a scan picks the strongest remembered one in range. Once online we sync the clock via SNTP.
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "wifi_setup.h"
#include "ota.h"
#include "agent.h"
#include "calendar.h"
#include "spotify.h"
#include "reminders.h"
#include "remote_log.h"
#include "mdns.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

static const char *TAG = "wifi";

#define MAX_RETRIES 4
#define MAX_APS 20

static volatile wifi_state_t state = WS_PORTAL;
static char sta_ssid[33], ip_str[16] = "", ap_ssid[33], ap_pass[9];
static volatile int ap_clients, retries;
static volatile bool ap_running, time_synced, last_auth_failed;
EXT_RAM_BSS_ATTR static wifi_ap_record_t aps[MAX_APS];
static uint16_t ap_count;
static volatile bool scanning, retries_paused, suspended;
static portMUX_TYPE aps_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t ap_off_timer, retry_timer, backoff_timer;
static bool ever_connected;           // this network worked since boot: a drop is the router, not the password
static int64_t lost_at;
static void go_portal(void);

// ---------------------------------------------------------------- remembered networks, most recent first
#define MAX_SAVED 5
typedef struct { char ssid[33], pass[65]; } saved_net_t;
EXT_RAM_BSS_ATTR static saved_net_t saved[MAX_SAVED];
static int n_saved;
static volatile bool picking;         // a scan is under way to choose a remembered network
static int64_t picked_at;

static void saved_store(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wifinets", saved, n_saved * sizeof saved[0]);
    nvs_commit(h);
    nvs_close(h);
}

static void saved_remove(const char *ssid)
{
    int k = 0;
    for (int i = 0; i < n_saved; i++)
        if (strcmp(saved[i].ssid, ssid)) saved[k++] = saved[i];
    n_saved = k;
}

// Joined successfully: put this network (and its password, from the driver) at the top of the list
static void remember_current(void)
{
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK || !c.sta.ssid[0]) return;
    saved_net_t n = {0};
    memcpy(n.ssid, c.sta.ssid, 32);
    memcpy(n.pass, c.sta.password, 64);
    if (n_saved && !memcmp(&saved[0], &n, sizeof n)) return;   // already first: no flash write
    saved_remove(n.ssid);
    if (n_saved == MAX_SAVED) n_saved--;               // drop the longest unused
    memmove(saved + 1, saved, n_saved * sizeof saved[0]);
    saved[0] = n;
    n_saved++;
    saved_store();
    ESP_LOGI(TAG, "remembered %s (%d networks saved)", n.ssid, n_saved);
}

static void saved_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof saved;
    if (nvs_open("demo", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "wifinets", saved, &len) == ESP_OK) n_saved = len / sizeof saved[0];
        nvs_close(h);
    }
    if (!n_saved) remember_current();                  // first run: adopt the network the driver already knows
}

static void join(const char *ssid, const char *pass, bool disconnect_first)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, pass, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    ESP_LOGI(TAG, "joining %s", ssid);
    esp_timer_stop(retry_timer);
    esp_timer_stop(backoff_timer);
    strlcpy(sta_ssid, ssid, sizeof sta_ssid);
    retries = 0;
    last_auth_failed = false;
    state = WS_CONNECTING;
    if (disconnect_first) esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);     // also persisted to NVS by the driver
    retries = 0;                                // the disconnect above may have bumped it
    esp_wifi_connect();
}

// Scan, then join the strongest remembered network in range (see scan_done). False if no scan started.
static bool pick_network(void)
{
    if (!n_saved || retries_paused || picking) return picking;
    if (ap_running && ap_clients > 0) return false;    // scanning hops channels: would drop a phone doing setup
    picked_at = esp_timer_get_time();
    picking = true;
    wifi_scan_config_t sc = {.show_hidden = false};
    scanning = true;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) { scanning = picking = false; return false; }
    return true;
}

static void pick_from_scan(void)
{
    for (int i = 0; i < ap_count; i++)                 // strongest first
        for (int j = 0; j < n_saved; j++)
            if (!strcmp((char *)aps[i].ssid, saved[j].ssid) && !(last_auth_failed && !strcmp(saved[j].ssid, sta_ssid))) {
                ESP_LOGI(TAG, "%s is in range (%d dBm)", saved[j].ssid, aps[i].rssi);
                join(saved[j].ssid, saved[j].pass, false);
                return;
            }
    ESP_LOGI(TAG, "no remembered network in range");
    if (ever_connected && !last_auth_failed && esp_timer_get_time() - lost_at < 5 * 60 * 1000000LL) {
        state = WS_CONNECTING;                         // keep trying the last one for now
        esp_timer_stop(backoff_timer);
        esp_timer_start_once(backoff_timer, 10 * 1000000);
    } else go_portal();
}
static httpd_handle_t httpd;

wifi_state_t wifi_state(void) { return state; }
const char *wifi_ssid(void) { return sta_ssid; }
const char *wifi_ip(void) { return ip_str; }
int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return state == WS_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
const char *wifi_ap_ssid(void) { return ap_ssid; }
const char *wifi_ap_pass(void) { return ap_pass; }
int wifi_ap_clients(void) { return ap_clients; }
bool wifi_time_synced(void) { return time_synced; }
bool wifi_ap_active(void) { return ap_running; }
bool wifi_scanning(void) { return scanning; }
bool wifi_last_failed(void) { return state == WS_PORTAL && retries > 0; }
void wifi_pause_retries(bool pause) { retries_paused = pause; }

// ---------------------------------------------------------------- hotspot on/off
static void start_ap(void)
{
    if (ap_running) return;
    ap_running = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG, "setup hotspot %s up", ap_ssid);
}

static void stop_ap(void *arg)
{
    if (!ap_running || state != WS_CONNECTED) return;
    ap_running = false;
    ap_clients = 0;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup hotspot down");
}

void wifi_scan(void)
{
    if (scanning) return;
    wifi_scan_config_t sc = {.show_hidden = false};
    scanning = true;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) scanning = false;   // e.g. busy connecting; try again later
}

static void scan_done(void)
{
    EXT_RAM_BSS_ATTR static wifi_ap_record_t tmp[MAX_APS];
    uint16_t n = MAX_APS;
    if (esp_wifi_scan_get_ap_records(&n, tmp) != ESP_OK) n = 0;
    // strongest first, one entry per network name
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && tmp[j].rssi > tmp[j - 1].rssi; j--) {
            wifi_ap_record_t t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t;
        }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!tmp[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = !strcmp((char *)tmp[j].ssid, (char *)tmp[i].ssid);
        if (!dup) tmp[k++] = tmp[i];
    }
    taskENTER_CRITICAL(&aps_lock);
    memcpy(aps, tmp, k * sizeof aps[0]);
    ap_count = k;
    taskEXIT_CRITICAL(&aps_lock);
    scanning = false;
    if (picking) { picking = false; pick_from_scan(); }
    ESP_LOGI(TAG, "scan: %d networks", k);
}

int wifi_networks(wifi_net_t *out, int max)
{
    taskENTER_CRITICAL(&aps_lock);
    int n = ap_count < max ? ap_count : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].ssid, aps[i].ssid, sizeof out[i].ssid);
        out[i].ssid[32] = 0;
        out[i].rssi = aps[i].rssi;
        out[i].secure = aps[i].authmode != WIFI_AUTH_OPEN;
    }
    taskEXIT_CRITICAL(&aps_lock);
    return n;
}

static void connect_sta(void *arg)
{
    if (state == WS_CONNECTED || !sta_ssid[0]) return;
    // reconnect attempts hop channels, which would knock a phone off the setup hotspot
    if (ap_running && ap_clients > 0 && arg) return;
    state = WS_CONNECTING;
    esp_wifi_connect();
}

// ---------------------------------------------------------------- events
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (suspended) return;                      // radio switched off on purpose (locked)
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ip_str[0] = 0;
        if (!sta_ssid[0]) {                     // forgotten: just sit in setup mode
            state = WS_PORTAL;
            start_ap();
            return;
        }
        last_auth_failed = d->reason == WIFI_REASON_AUTH_FAIL || d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                           d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        if (state == WS_CONNECTED) { retries = 0; lost_at = esp_timer_get_time(); }
        if (picking) return;                    // the scan in progress decides what to join
        // not there at all (e.g. back home): after two tries, look for another remembered network,
        // and while retrying a lost one, look around every 30 s
        bool gone = d->reason == WIFI_REASON_NO_AP_FOUND;
        bool look = n_saved > 1 && !retries_paused && esp_timer_get_time() - picked_at > 30 * 1000000LL &&
                    (retries >= MAX_RETRIES || (gone && retries >= 1));
        if (look && pick_network()) {
            ESP_LOGI(TAG, "%s not reachable (reason %d): looking for remembered networks", sta_ssid, d->reason);
            state = WS_CONNECTING;
            retries++;
        } else if (++retries <= MAX_RETRIES && !last_auth_failed) {
            state = WS_CONNECTING;
            esp_wifi_connect();
        } else if (ever_connected && !last_auth_failed && esp_timer_get_time() - lost_at < 5 * 60 * 1000000LL) {
            // a network that worked earlier (router rebooting, weak spell): keep trying it, 2 s doubling to 30 s,
            // for 5 minutes before falling back to the setup hotspot
            int n = retries - MAX_RETRIES, s = n < 4 ? 2 << n : 30;
            if (n == 1) ESP_LOGW(TAG, "lost %s (reason %d), retrying", sta_ssid, d->reason);
            state = WS_CONNECTING;
            esp_timer_stop(backoff_timer);
            esp_timer_start_once(backoff_timer, s * 1000000LL);
        } else {
            ESP_LOGW(TAG, "can't reach %s (reason %d), setup hotspot on", sta_ssid, d->reason);
            go_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&e->ip_info.ip));
        state = WS_CONNECTED;
        retries = 0;
        ever_connected = true;
        last_auth_failed = false;
        remember_current();
        ESP_LOGI(TAG, "connected to %s, ip %s", sta_ssid, ip_str);
        esp_netif_sntp_start();
        esp_timer_stop(ap_off_timer);
        esp_timer_start_once(ap_off_timer, 20 * 1000000);      // let the setup page show success first
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (ap_clients > 0) ap_clients--;
    }
}

static void go_portal(void)
{
    state = WS_PORTAL;
    start_ap();
    esp_timer_stop(retry_timer);
    esp_timer_start_once(retry_timer, 30 * 1000000);   // keep trying in the background
}

static void retry_cb(void *arg)
{
    if (state == WS_PORTAL && !retries_paused) {
        retries = MAX_RETRIES;   // one attempt per tick
        if (n_saved > 1) pick_network();               // any remembered network that's in range now
        else if (sta_ssid[0]) connect_sta((void *)1);
    }
    if (state != WS_CONNECTED) esp_timer_start_once(retry_timer, 30 * 1000000);
}

static void on_time_sync(struct timeval *tv) { time_synced = true; }

// ---------------------------------------------------------------- captive DNS: every name -> 192.168.4.1
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(sock, (struct sockaddr *)&addr, sizeof addr);
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !ap_running) continue;
        int q = 12;                               // skip the question name
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                   // null label + qtype + qclass
        if (q > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;             // response, recursion available
        buf[6] = 0; buf[7] = 1;                   // one answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + q, ans, sizeof ans);
        sendto(sock, buf, q + sizeof ans, 0, (struct sockaddr *)&from, fl);
    }
}

// ---------------------------------------------------------------- web page
static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Bobble Wi-Fi setup</title><style>"
    ":root{color-scheme:dark}body{font:16px system-ui,sans-serif;background:#0b0b12;color:#eee;margin:0;padding:20px;max-width:440px;margin:auto}"
    "h1{font-size:22px;margin:4px 0 16px}.net{display:flex;justify-content:space-between;padding:14px;margin:6px 0;background:#181826;"
    "border-radius:10px;cursor:pointer;border:2px solid transparent}.net.sel{border-color:#00e6c8}.net small{color:#889}"
    "input,button{width:100%;box-sizing:border-box;font-size:17px;padding:13px;border-radius:10px;border:1px solid #334;background:#12121c;color:#eee;margin:6px 0}"
    "button{background:#00e6c8;color:#002;border:0;font-weight:600}button.sec{background:#222234;color:#ccd}#st{margin-top:14px;min-height:24px}"
    ".ok{color:#3e9}.bad{color:#f66}</style></head><body><h1>Connect your Bobble to Wi-Fi</h1>"
    "<div id=list>Scanning&hellip;</div><button class=sec onclick='load(1)'>Rescan</button>"
    "<input id=ssid placeholder='Network name' autocapitalize=off><input id=pw type=password placeholder='Password (blank if open)'>"
    "<button onclick='go()'>Connect</button><div id=st></div><script>"
    "const $=i=>document.getElementById(i);"
    "function bars(r){return r>-55?'\\u2582\\u2584\\u2586\\u2588':r>-67?'\\u2582\\u2584\\u2586':r>-78?'\\u2582\\u2584':'\\u2582'}"
    "async function load(re){$('list').textContent='Scanning\\u2026';let a=await(await fetch('/scan'+(re?'?re=1':''))).json();"
    "$('list').innerHTML='';if(!a.length)$('list').textContent='No networks found';"
    "a.forEach(n=>{let d=document.createElement('div');d.className='net';d.innerHTML='<span></span><small></small>';"
    "d.firstChild.textContent=n.s+(n.l?' \\ud83d\\udd12':'');d.lastChild.textContent=bars(n.r);"
    "d.onclick=()=>{document.querySelectorAll('.net').forEach(e=>e.classList.remove('sel'));d.classList.add('sel');$('ssid').value=n.s;$('pw').focus()};"
    "$('list').appendChild(d)})}"
    "async function go(){let b=new URLSearchParams({s:$('ssid').value,p:$('pw').value});"
    "$('st').className='';$('st').textContent='Connecting\\u2026';try{await fetch('/connect',{method:'POST',body:b})}catch(e){}setTimeout(poll,1500)}"
    "async function poll(){let s;try{s=await(await fetch('/status')).json()}catch(e){return setTimeout(poll,1500)}"
    "if(s.state==2){$('st').className='ok';$('st').textContent='Connected to '+s.ssid+' \\u2014 IP '+s.ip+'. You can close this page.'}"
    "else if(s.state==0&&s.fail){$('st').className='bad';$('st').textContent='Could not join '+s.ssid+' \\u2014 check the password.'}"
    "else setTimeout(poll,1000)}load(0)</script></body></html>";

// The setup pages (and captive-portal redirects) only answer people joined to the setup hotspot;
// on the home network the board just offers the token-protected /ota endpoint.
static bool via_hotspot(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof local;
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0) return false;
    uint32_t ip = 0;
    if (local.ss_family == AF_INET) ip = ((struct sockaddr_in *)&local)->sin_addr.s_addr;
    else if (local.ss_family == AF_INET6) memcpy(&ip, ((uint8_t *)&((struct sockaddr_in6 *)&local)->sin6_addr) + 12, 4);   // v4-mapped
    return ip == ESP_IP4TOADDR(192, 168, 4, 1);
}

static esp_err_t not_here(httpd_req_t *req)
{
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "not found\n");
}

static esp_err_t page_get(httpd_req_t *req)
{
    if (!via_hotspot(req)) return not_here(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof PAGE - 1);
}

// Anything unknown (phone captive-portal probes) is redirected to the setup page
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
    if (!via_hotspot(req)) return not_here(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = *in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = 0;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!via_hotspot(req)) return not_here(req);
    char q[16];
    if (ap_count == 0 || (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "re=1"))) {
        wifi_scan();
        for (int i = 0; i < 60 && scanning; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (int i = 0; i < ap_count; i++) {
        char esc[80], item[128];
        json_escape(esc, sizeof esc, (char *)aps[i].ssid);
        snprintf(item, sizeof item, "%s{\"s\":\"%s\",\"r\":%d,\"l\":%d}", first ? "" : ",", esc, aps[i].rssi,
                 aps[i].authmode != WIFI_AUTH_OPEN);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

void wifi_join(const char *ssid, const char *pass)
{
    esp_wifi_scan_stop();
    picking = false;
    join(ssid, pass, true);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    if (!via_hotspot(req)) return not_here(req);
    char body[256] = {0}, ssid[100] = {0}, pass[200] = {0};
    int len = httpd_req_recv(req, body, sizeof body - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    httpd_query_key_value(body, "s", ssid, sizeof ssid);
    httpd_query_key_value(body, "p", pass, sizeof pass);
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 63) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid/password");

    wifi_join(ssid, pass);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!via_hotspot(req)) return not_here(req);
    char esc[80], out[200];
    json_escape(esc, sizeof esc, sta_ssid);
    snprintf(out, sizeof out, "{\"state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"fail\":%d}", state, esc, ip_str,
             state == WS_PORTAL && retries > 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static void start_web(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;   // (in internal RAM: /ota maps flash to check the image, which freezes the cache: a PSRAM stack asserts)
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 24;
    if (httpd_start(&httpd, &cfg) != ESP_OK) return;
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/connect", .method = HTTP_POST, .handler = connect_post},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(httpd, &uris[i]);
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, redirect);
    ota_register(httpd);
    spotify_register(httpd);
    reminders_register(httpd);
    remote_log_register(httpd);
    agent_register(httpd);
    calendar_register(httpd);
}

// ---------------------------------------------------------------- setup
static void load_ap_password(void)
{
    nvs_handle_t h;
    size_t len = sizeof ap_pass;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, "ap_pass", ap_pass, &len) != ESP_OK || strlen(ap_pass) != 8) {
        for (int i = 0; i < 8; i++) ap_pass[i] = '0' + esp_random() % 10;   // digits only: easy to read off the screen
        ap_pass[8] = 0;
        nvs_set_str(h, "ap_pass", ap_pass);
        nvs_commit(h);
    }
    nvs_close(h);
}

void wifi_setup_start(void)
{
    load_ap_password();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof ap_ssid, "Bobble-Setup-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_t *apn = esp_netif_create_default_wifi_ap();
    // hand out 192.168.4.1 as the DNS server so the captive portal DNS answers every lookup
    esp_netif_dns_info_t dns = {.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1), .ip.type = ESP_IPADDR_TYPE_V4};
    uint8_t offer_dns = 1;
    esp_netif_dhcps_stop(apn);
    esp_netif_set_dns_info(apn, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(apn, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns);
    esp_netif_dhcps_start(apn);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    esp_timer_create_args_t t1 = {.callback = stop_ap, .name = "ap_off"};
    esp_timer_create(&t1, &ap_off_timer);
    esp_timer_create_args_t t2 = {.callback = retry_cb, .name = "wifi_retry"};
    esp_timer_create(&t2, &retry_timer);
    esp_timer_create_args_t t3 = {.callback = connect_sta, .name = "wifi_backoff"};
    esp_timer_create(&t3, &backoff_timer);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap.ap.password, ap_pass, sizeof ap.ap.password);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 3;
    ap.ap.channel = 1;

    wifi_config_t sta = {0};
    esp_wifi_get_config(WIFI_IF_STA, &sta);     // the network last joined, if any
    saved_load();
    strlcpy(sta_ssid, (char *)sta.sta.ssid, sizeof sta_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (sta_ssid[0]) {
        esp_wifi_set_mode(WIFI_MODE_STA);       // known network: no hotspot unless joining fails
        ap_running = false;
    } else {
        ap_running = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (sta_ssid[0]) connect_sta(NULL);
    else esp_timer_start_once(retry_timer, 30 * 1000000);

    setenv("TZ", "MST7MDT,M3.2.0,M11.1.0", 1);  // America/Denver
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.sync_cb = on_time_sync;
    sntp.start = false;
    esp_netif_sntp_init(&sntp);

    start_web();
    if (mdns_init() == ESP_OK) {                // reachable as bobble.local (a second one on the network gets bobble-2.local)
        mdns_hostname_set("bobble");
        mdns_instance_name_set(agent_device_name());
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        // the Mac's Bobble app finds displays by this: POST /agent, protocol version 1, and which board it is (id), so
        // it keeps to the one it paired with when there are several about
        mdns_txt_item_t txt[] = {{"api", "1"}, {"path", "/agent"}, {"id", agent_device_id()}};
        mdns_service_add(agent_device_name(), "_bobble", "_tcp", 80, txt, 3);
    }
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    xTaskCreateWithCaps(dns_task, "dns", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
#else
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
#endif
}

// Renamed in the app: the adverts say the new name
void wifi_set_name(const char *name)
{
    mdns_instance_name_set(name);
    mdns_service_instance_name_set("_bobble", "_tcp", name);
}

// Locked: the radio goes off completely (no reconnecting, no hotspot); unlocking starts it again
void wifi_suspend(bool on)
{
    if (on == suspended) return;
    suspended = on;
    if (on) {
        esp_timer_stop(retry_timer);
        esp_timer_stop(backoff_timer);
        esp_timer_stop(ap_off_timer);
        esp_wifi_stop();
        state = WS_CONNECTING;
        ip_str[0] = 0;
        picking = scanning = false;
        ap_running = false;
        ESP_LOGI(TAG, "radio off");
        return;
    }
    retries = 0;
    ap_running = !sta_ssid[0];
    esp_wifi_set_mode(sta_ssid[0] ? WIFI_MODE_STA : WIFI_MODE_APSTA);
    esp_wifi_start();
    ESP_LOGI(TAG, "radio on");
    if (sta_ssid[0]) connect_sta(NULL);
    else go_portal();
}

void wifi_forget(void)
{
    ESP_LOGI(TAG, "forgetting %s", sta_ssid);
    saved_remove(sta_ssid);
    saved_store();
    esp_timer_stop(ap_off_timer);
    wifi_config_t empty = {0};
    sta_ssid[0] = 0;
    ip_str[0] = 0;
    state = WS_PORTAL;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    retries = 0;
    start_ap();
    pick_network();                             // another remembered network may be in range
}
