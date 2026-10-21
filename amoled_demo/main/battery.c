#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "battery.h"

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static bool ok, have_cali;
static float volts;                   // smoothed, corrected to what the cell reads with the screen off
static int64_t last;
// Charging: the charger chip's status only drives the board's red LED, so the one thing we can sense is
// a computer talking to us over USB. (Guessing from voltage jumps proved unreliable: plugging in only
// moves the reading ~0.1 V, about the same as the screen or Wi-Fi switching on and off.)

// ---------------------------------------------------------------- learning
// 1. Screen sag: each time the screen turns on or off, compare the readings either side and keep a running
//    average of the drop, so readings taken with the screen on can be corrected to screen-off values.
// 2. Discharge curve: from the moment a full battery starts draining, log the corrected voltage every few
//    minutes (saved in NVS, survives reboots and OTA). When the run reaches empty - or the board dies and is
//    later switched on again on a charged battery - the log becomes a voltage -> % table, where % is the share
//    of this battery's real runtime left. Runs cut short by charging are thrown away.
// 3. On battery the shown % only goes down; it can only go up over USB, or after a sustained rise (wall charger).

#define SAG_MAGIC   0x5A600001
#define OFF_MAGIC   0x0FF5E701
#define CURVE_MAGIC 0xC0B70001
#define LOG_MAGIC   0x1060B001
#define LOG_MAX     240
#define FULL_MV     4150              // a run starts when a charged battery drops below this
#define EMPTY_MV    3400              // ...and is complete at this
#define NEAR_EMPTY_MV 3750            // below this, save every log entry (the board may die any minute);
                                      // this board browns out around 3.66 V, so a run ending under this counts as run flat
#define SHUTDOWN_MV 3700              // deep sleep here, just above the brown-out, so the board stops cleanly

static const uint8_t points[12] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0};
// until a run is learned: a generic LiPo curve squeezed to end at this board's shutdown point
static uint16_t curve[12] = {4200, 4110, 4040, 3980, 3930, 3890, 3855, 3825, 3795, 3760, 3730, 3700};
static uint8_t curve_runs;            // completed runs averaged into curve[] (0: generic LiPo table)

static bool screen_on = true;
static float sag;                     // volts the screen pulls the reading down by
static bool have_sag;
static float ring[8];                 // last raw readings, for measuring the sag
static int ring_n;
static float sag_pre;                 // reading just before the last screen change; 0: not measuring
static int sag_wait;                  // samples since that change
static bool sag_to_on;
static float sag_saved = -1;
static int64_t sag_saved_at;

static struct {
    uint32_t magic;
    uint16_t n, step_min;             // n entries, one every step_min minutes from the start of the run
    uint16_t mv[LOG_MAX];
} lg;
static enum { RUN_IDLE, RUN_ARMED, RUN_LOGGING } run;   // ARMED: battery seen full, waiting for it to drain
static int64_t run_t0;                // esp_timer time of entry 0
static uint16_t run_min_mv;
static int64_t rise_since;
static int unsaved;

static int shown = -1;                // % on screen
// On USB the board reads well above the cell's own voltage (charger): learned from the step at each plug / unplug
static float usb_offset = 0.27f;
static bool usb_prev, have_usb_prev;
static int64_t usb_edge_at, unplugged_at;
static float usb_edge_v;
static int64_t below_since, above_since;
static int below_max;                 // highest reading while below the shown %

// Flash writes pause both cores' access to PSRAM for a moment, which would show as a hitch if done here on the
// render loop: keep a copy and let a small background task write it
static struct { const char *key; uint8_t data[sizeof lg]; size_t len; bool pending, erase; } slots[3];
static SemaphoreHandle_t slot_lock;
static TaskHandle_t writer;

static void writer_task(void *arg)
{
    static uint8_t copy[sizeof lg];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int i = 0; i < 3; i++) {
            xSemaphoreTake(slot_lock, portMAX_DELAY);
            bool go = slots[i].pending, erase = slots[i].erase;
            size_t len = slots[i].len;
            const char *key = slots[i].key;
            memcpy(copy, slots[i].data, len);
            slots[i].pending = false;
            xSemaphoreGive(slot_lock);
            if (!go) continue;
            nvs_handle_t h;
            if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) continue;
            if (erase) nvs_erase_key(h, key);
            else nvs_set_blob(h, key, copy, len);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static void nvs_put(const char *key, const void *p, size_t len)
{
    if (!writer || len > sizeof slots[0].data) return;
    int i = 0;
    while (i < 2 && slots[i].key && strcmp(slots[i].key, key)) i++;
    xSemaphoreTake(slot_lock, portMAX_DELAY);
    slots[i].key = key;
    slots[i].erase = !p;
    slots[i].len = p ? len : 0;
    if (p) memcpy(slots[i].data, p, len);
    slots[i].pending = true;
    xSemaphoreGive(slot_lock);
    xTaskNotifyGive(writer);
}

static bool nvs_get(const char *key, void *p, size_t len)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = len;
    bool r = nvs_get_blob(h, key, p, &l) == ESP_OK && l == len;
    nvs_close(h);
    return r;
}

static void save_curve(void)
{
    struct { uint32_t magic; uint16_t mv[12]; uint8_t runs; } c = {CURVE_MAGIC, {0}, curve_runs};
    memcpy(c.mv, curve, sizeof curve);
    nvs_put("bcurve", &c, sizeof c);
}

static void save_log(void) { nvs_put("blog", &lg, sizeof lg); unsaved = 0; }

static void drop_log(const char *why)
{
    if (run == RUN_LOGGING) ESP_LOGI(TAG, "discharge log dropped after %d min: %s", lg.n * lg.step_min, why);
    run = RUN_IDLE;
    lg.n = 0;
    nvs_put("blog", NULL, 0);
}

// Turn a finished run into a voltage -> % table, averaged with earlier runs
static void learn_from_log(void)
{
    int span = (lg.n - 1) * lg.step_min;
    if (lg.n < 2 || span < 60 || lg.mv[lg.n - 1] > NEAR_EMPTY_MV) { drop_log("too short to learn from"); return; }
    uint16_t fresh[12];
    for (int i = 0; i < 12; i++) {
        float t = (100 - points[i]) / 100.0f * (lg.n - 1);   // in log entries
        int k = (int)t;
        float f = t - k;
        float v = k + 1 < lg.n ? lg.mv[k] * (1 - f) + lg.mv[k + 1] * f : lg.mv[lg.n - 1];
        fresh[i] = (uint16_t)v;
        if (i && fresh[i] > fresh[i - 1]) fresh[i] = fresh[i - 1];   // keep it falling
    }
    int w = curve_runs > 3 ? 3 : curve_runs;          // newer runs count more, so the table follows the cell as it ages
    for (int i = 0; i < 12; i++) curve[i] = (uint16_t)((curve[i] * w + fresh[i]) / (w + 1));
    if (curve_runs < 255) curve_runs++;
    save_curve();
    ESP_LOGI(TAG, "learned discharge curve from a %d min run (%d runs): 100%%=%d 50%%=%d 10%%=%d 0%%=%d mV",
             span, curve_runs, curve[0], curve[5], curve[9], curve[11]);
    run = RUN_IDLE;
    lg.n = 0;
    nvs_put("blog", NULL, 0);
}

static void load_learning(float v_now)
{
    struct { uint32_t magic; float sag; } s;
    if (nvs_get("bsag", &s, sizeof s) && s.magic == SAG_MAGIC) { sag = sag_saved = s.sag; have_sag = true; }
    struct { uint32_t magic; uint16_t mv[12]; uint8_t runs; } c;
    if (nvs_get("bcurve", &c, sizeof c) && c.magic == CURVE_MAGIC) {
        if (c.mv[11] < SHUTDOWN_MV - 100) {           // ends far below where this board shuts off: a bad run, forget it
            ESP_LOGW(TAG, "ignoring learned curve (100%%=%d, 0%%=%d mV): it runs below the board's shutdown", c.mv[0], c.mv[11]);
            nvs_put("bcurve", NULL, 0);
        } else { memcpy(curve, c.mv, sizeof curve); curve_runs = c.runs; }
    }
    // a run that sampled its first point while the voltage was still falling off the charger has a 100% point far
    // above the rest: put it one step above 90%
    int g1 = curve[0] - curve[1], g2 = curve[1] - curve[2];
    if (curve_runs && g1 > 3 * (g2 > 20 ? g2 : 20)) {
        ESP_LOGI(TAG, "curve: 100%% point %d mV was taken off the charger, using %d", curve[0], curve[1] + g2);
        curve[0] = curve[1] + g2;
        save_curve();
    }
    struct { uint32_t magic; float off; } o;
    if (nvs_get("boff", &o, sizeof o) && o.magic == OFF_MAGIC) usb_offset = o.off;
    ESP_LOGI(TAG, "curve (%d runs), mV at 100/90/80/70/60/50/40/30/20/10/5/0%%: %d %d %d %d %d %d %d %d %d %d %d %d", curve_runs,
             curve[0], curve[1], curve[2], curve[3], curve[4], curve[5], curve[6], curve[7], curve[8], curve[9], curve[10], curve[11]);
    if (!nvs_get("blog", &lg, sizeof lg) || lg.magic != LOG_MAGIC || lg.n == 0) { lg.n = 0; return; }

    // a run was in progress when we last stopped: did the battery die, get charged, or did we just reboot?
    int mv = (int)(v_now * 1000), last_mv = lg.mv[lg.n - 1];
    ESP_LOGI(TAG, "found discharge log: %d min, last %d mV, now %d mV", (lg.n - 1) * lg.step_min, last_mv, mv);
    if (last_mv <= NEAR_EMPTY_MV && mv > last_mv + 150) learn_from_log();   // ran flat, since recharged
    else if (abs(mv - last_mv) < 60 && !usb_serial_jtag_is_connected()) {  // a reboot or OTA mid-run: carry on
        run = RUN_LOGGING;
        run_t0 = esp_timer_get_time() - (int64_t)lg.n * lg.step_min * 60000000;
        run_min_mv = last_mv;
    } else drop_log("battery was charged before it ran out");
}

static void log_tick(int64_t now)
{
    int mv = (int)(volts * 1000);
    bool usb = usb_serial_jtag_is_connected();

    if (run == RUN_IDLE && mv >= FULL_MV + 20) run = RUN_ARMED;
    if (run == RUN_ARMED && !usb && mv < FULL_MV && unplugged_at && now - unplugged_at > 120000000) {   // settled
        lg = (typeof(lg)){.magic = LOG_MAGIC, .step_min = 2};
        lg.mv[lg.n++] = mv;
        run = RUN_LOGGING;
        run_t0 = now;
        run_min_mv = mv;
        rise_since = 0;
        save_log();
        ESP_LOGI(TAG, "battery full and draining: logging the discharge");
    }
    if (run != RUN_LOGGING) return;

    // charging mid-run spoils the log
    if (usb) { drop_log("plugged into USB"); return; }
    if (mv < run_min_mv) run_min_mv = mv;
    if (mv > run_min_mv + 80) {
        if (!rise_since) rise_since = now;
        if (now - rise_since > 180000000) { drop_log("voltage rose (wall charger?)"); return; }
    } else rise_since = 0;

    if (now - run_t0 < (int64_t)lg.n * lg.step_min * 60000000) return;
    if (lg.n == LOG_MAX) {                            // full: halve the resolution
        for (int i = 0; i < LOG_MAX / 2; i++) lg.mv[i] = lg.mv[2 * i];
        lg.n = LOG_MAX / 2;
        lg.step_min *= 2;
    }
    lg.mv[lg.n++] = mv;
    if (mv <= EMPTY_MV) { learn_from_log(); return; }
    if (++unsaved >= 5 || mv <= NEAR_EMPTY_MV) save_log();
}

// ---------------------------------------------------------------- reading

void battery_init(void)
{
    if (ok) return;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t ch = {.bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12};
    if (adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &ch) != ESP_OK) return;
    adc_cali_curve_fitting_config_t cc = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    have_cali = adc_cali_create_scheme_curve_fitting(&cc, &cali) == ESP_OK;
    slot_lock = xSemaphoreCreateMutex();
    xTaskCreate(writer_task, "batt_nvs", 4096, NULL, 1, &writer);
    ok = true;
}

static float read_once(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw;
        if (adc_oneshot_read(adc, ADC_CHANNEL_3, &raw) != ESP_OK) continue;
        int mv = raw * 3300 / 4095;
        if (have_cali) adc_cali_raw_to_voltage(cali, raw, &mv);
        sum += mv;
        n++;
    }
    return n ? sum / (float)n * 3 / 1000 : 0;          // board divides the battery by 3
}

static float ring_avg(int from)
{
    float s = 0;
    for (int i = from; i < ring_n; i++) s += ring[i];
    return ring_n > from ? s / (ring_n - from) : 0;
}

void battery_set_screen(bool on)
{
    if (on == screen_on) return;
    screen_on = on;
    sag_pre = ring_n >= 4 ? ring_avg(0) : 0;          // measure the step once things settle
    sag_to_on = on;
    sag_wait = 0;
    ring_n = 0;
}

static void sag_sample(float raw, int64_t now)
{
    if (ring_n == 8) memmove(ring, ring + 1, sizeof ring - sizeof ring[0]), ring_n--;
    ring[ring_n++] = raw;
    if (sag_pre == 0 || ++sag_wait < 8) return;
    float post = ring_avg(2);                          // skip the first second after the change
    float d = sag_to_on ? sag_pre - post : post - sag_pre;
    sag_pre = 0;
    if (d < -0.05f || d > 0.4f || usb_serial_jtag_is_connected()) return;   // something else moved it
    if (d < 0) d = 0;
    sag = have_sag ? sag * 0.8f + d * 0.2f : d;
    have_sag = true;
    if (fabsf(sag - sag_saved) > 0.005f && (sag_saved < 0 || now - sag_saved_at > 600000000)) {
        struct { uint32_t magic; float sag; } s = {SAG_MAGIC, sag};
        nvs_put("bsag", &s, sizeof s);
        sag_saved = sag;
        sag_saved_at = now;
        ESP_LOGI(TAG, "screen sag %.0f mV", sag * 1000);
    }
}

void battery_update(void)
{
    int64_t now = esp_timer_get_time();
    if (!ok || now - last < 500000) return;
    bool first = last == 0;
    last = now;
    float raw = read_once();
    if (raw < 2.5f) { volts = raw; return; }            // no battery
    sag_sample(raw, now);
    float v = raw + (screen_on ? sag : 0), before = volts;
    volts = volts < 2.5f ? v : volts * 0.9f + v * 0.1f;  // smooth out ADC noise
    bool usb = usb_serial_jtag_is_connected();
    if (have_usb_prev && usb != usb_prev) { usb_edge_at = now; usb_edge_v = before; }   // plugged in / unplugged
    if (!usb && (usb_prev || !have_usb_prev)) unplugged_at = now;
    usb_prev = usb;
    have_usb_prev = true;
    if (usb_edge_at && now - usb_edge_at > 30000000) {   // settled: the step is the charger's offset
        float d = usb ? volts - usb_edge_v : usb_edge_v - volts;
        usb_edge_at = 0;
        if (d > 0.05f && d < 0.6f) {
            usb_offset = usb_offset * 0.6f + d * 0.4f;
            struct { uint32_t magic; float off; } o = {OFF_MAGIC, usb_offset};
            nvs_put("boff", &o, sizeof o);
            ESP_LOGI(TAG, "%s: %.0f mV step, charger offset now %.0f mV", usb ? "plugged in" : "unplugged", d * 1000, usb_offset * 1000);
        }
    }
    if (first) load_learning(v);
    log_tick(now);
}

bool battery_charging(void) { return usb_serial_jtag_is_connected(); }

float battery_read_now(void) { return ok ? read_once() : 0; }

// Down to the shutdown point on battery for a minute: time to stop before the cell is harmed
bool battery_empty(void)
{
    static int64_t since;
    int64_t now = esp_timer_get_time();
    if (volts < 2.5f || battery_charging() || volts * 1000 >= SHUTDOWN_MV) { since = 0; return false; }
    if (!since) since = now;
    return now - since > 60000000;
}

float battery_empty_volts(void) { return SHUTDOWN_MV / 1000.0f; }

float battery_volts(void) { return volts; }

static int percent_of(float v)
{
    int mv = (int)(v * 1000);
    if (mv >= curve[0]) return 100;
    for (int i = 1; i < 12; i++)
        if (mv >= curve[i]) {
            float t = curve[i - 1] > curve[i] ? (float)(mv - curve[i]) / (curve[i - 1] - curve[i]) : 1;
            return (int)(points[i] + t * (points[i - 1] - points[i]) + 0.5f);
        }
    return 0;
}

int battery_percent(void)
{
    if (volts < 2.5f) return -1;                        // nothing plugged in
    int p = percent_of(battery_charging() ? volts - usb_offset : volts);   // on USB: the cell's own voltage
    int64_t now = esp_timer_get_time();
    if (shown < 0 || battery_charging()) { shown = p; below_since = above_since = 0; return shown; }
    // on battery: drop once the reading has stayed lower for 20 s (to the best of it, so Wi-Fi blips don't count);
    // go up only after a big rise that lasts two minutes (a wall charger we can't otherwise see)
    if (p < shown) {
        if (!below_since || p > below_max) below_max = p;
        if (!below_since) below_since = now;
        if (now - below_since > 20000000) { shown = below_max; below_since = 0; }
    } else below_since = 0;
    if (p >= shown + 5) { if (!above_since) above_since = now; if (now - above_since > 120000000) shown = p; }
    else above_since = 0;
    return shown;
}

int battery_learned_runs(void) { return curve_runs; }
bool battery_logging(void) { return run == RUN_LOGGING; }
