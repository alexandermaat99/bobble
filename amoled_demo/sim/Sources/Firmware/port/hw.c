// The board's hardware, played by the Mac: the panel draws into a frame buffer the app shows, the touch controller
// and motion sensor answer from the mouse and the tilt setting, BOOT is a key, the battery a setting. Deep sleep and
// restarts start the process over (as the board starts over), passing on why.
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>
#include "bobble_sim.h"
#include "gfx.h"
#include "sim_port.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_lcd_sh8601.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "nvs.h"

extern void app_main(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char **sim_argv;

// ---------------------------------------------------------------- where things live
const char *sim_support_dir(void)
{
    static char dir[1024];
    if (!dir[0]) {
#ifdef BOARD_SQUARE                                    // (Bobble Square: its own flash, so both can run at once)
        snprintf(dir, sizeof dir, "%s/Library/Application Support/BobbleSquare", getenv("HOME"));
#else
        snprintf(dir, sizeof dir, "%s/Library/Application Support/BobbleSim", getenv("HOME"));
#endif
        mkdir(dir, 0755);
    }
    return dir;
}

const char *sim_nvs_path(void)
{
    static char p[1100];
    if (!p[0]) snprintf(p, sizeof p, "%s/nvs.bin", sim_support_dir());
    return p;
}

// ---------------------------------------------------------------- the panel
#define GLASS W                     // (the firmware's own screen size: 466 round, 480 square)
static uint16_t glass[GLASS * GLASS];
static uint32_t glass_seq;
static int bright = 255;
static bool disp_on, chip_awake = true;

struct sim_lcd_io { esp_lcd_panel_io_color_trans_done_cb_t done; void *ctx; };
struct sim_lcd_panel { struct sim_lcd_io *io; };

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus, const esp_lcd_panel_io_spi_config_t *cfg, esp_lcd_panel_io_handle_t *out)
{
    struct sim_lcd_io *io = calloc(1, sizeof *io);
    io->done = cfg->on_color_trans_done;
    io->ctx = cfg->user_ctx;
    *out = io;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_sh8601(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *cfg, esp_lcd_panel_handle_t *out)
{
    struct sim_lcd_panel *p = calloc(1, sizeof *p);
    p->io = io;
    *out = p;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) { return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) { return ESP_OK; }
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y) { return ESP_OK; }   // (the glass is the window)

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on)
{
    pthread_mutex_lock(&lock);
    disp_on = on;
    glass_seq++;
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

// Commands the firmware sends the chip: 0x51 brightness, 0x10/0x11 sleep in/out
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t size)
{
    int c = (cmd >> 8) & 0xFF;
    pthread_mutex_lock(&lock);
    if (c == 0x51 && size >= 1) bright = *(const uint8_t *)param;
    if (c == 0x10) chip_awake = false;
    if (c == 0x11) chip_awake = true;
    glass_seq++;
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1, const void *data)
{
    const uint16_t *src = data;
    int w = x1 - x0;
    pthread_mutex_lock(&lock);
    for (int y = y0; y < y1 && y < GLASS; y++)
        if (y >= 0) memcpy(glass + y * GLASS + x0, src + (y - y0) * w, w * 2);
    glass_seq++;
    pthread_mutex_unlock(&lock);
    if (p->io->done) p->io->done(p->io, NULL, p->io->ctx);   // (sent: the buffer's free again)
    return ESP_OK;
}

int sim_glass_side(void) { return GLASS; }
bool sim_glass_round(void) { return SCREEN_ROUND; }

uint32_t sim_copy_glass(uint16_t *out, uint32_t seq)
{
    pthread_mutex_lock(&lock);
    uint32_t s = glass_seq;
    if (s != seq) memcpy(out, glass, sizeof glass);
    pthread_mutex_unlock(&lock);
    return s;
}

int sim_brightness(void) { return bright; }
bool sim_panel_on(void) { return disp_on && chip_awake; }

// ---------------------------------------------------------------- touch (FT3168 at 0x38) and motion (QMI8658 at 0x6B)
static volatile int touch_x, touch_y;
static volatile bool touch_down, boot_down, imu_on = true;
static float up[3] = {0, -1, 0};                      // standing on the desk, upright (screen axes: x right, y down)
static float shake;

void sim_touch(int x, int y, bool down) { touch_x = x; touch_y = y; touch_down = down; }
void sim_boot_button(bool down) { boot_down = down; }
void sim_shake(void) { shake = 0.4f; }

void sim_set_tilt(float deg, bool flat)
{
    float r = deg * (float)M_PI / 180;
    pthread_mutex_lock(&lock);
    if (flat) { up[0] = 0; up[1] = 0; up[2] = 1; }
    else { up[0] = sinf(r); up[1] = -cosf(r); up[2] = 0; }
    pthread_mutex_unlock(&lock);
}

struct sim_i2c_bus { int dummy; };
struct sim_i2c_dev { uint16_t addr; };

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *out)
{
    *out = calloc(1, sizeof **out);
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms)
{
    return addr == 0x38 || addr == 0x6B ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *c, i2c_master_dev_handle_t *out)
{
    struct sim_i2c_dev *d = calloc(1, sizeof *d);
    d->addr = c->device_address;
    *out = d;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t wl, int timeout_ms)
{
    if (d->addr == 0x6B && wl == 2 && w[0] == 0x08) imu_on = w[1] & 1;   // CTRL7: accelerometer on/off
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int timeout_ms)
{
    memset(r, 0, rl);
    if (d->addr == 0x38) {                            // touches: count at 0x02, then x/y (12 bits each) from 0x03
        if (w[0] == 0x02) r[0] = touch_down ? 1 : 0;
        if (w[0] == 0x03 && rl >= 4) {
            int x = touch_x < 0 ? 0 : touch_x, y = touch_y < 0 ? 0 : touch_y;
            r[0] = (x >> 8) & 0x0F; r[1] = x & 0xFF; r[2] = (y >> 8) & 0x0F; r[3] = y & 0xFF;
        }
        return ESP_OK;
    }
    if (d->addr == 0x6B && w[0] == 0x35 && rl >= 6) { // accelerometer x, y, z (16 bits, 1 g = 16384): the "up" vector
        float a[3], j = shake > 0 ? shake * (float)((arc4random() % 200) - 100) / 100 : 0;
        if (shake > 0) shake -= 0.02f;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < 3; i++) a[i] = imu_on ? up[i] + j * (i == 2 ? 1 : 0.5f) : 0;
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < 3; i++) {
            int v = (int)lroundf(a[i] * 16384 + (float)((int)(arc4random() % 41) - 20));   // (a little sensor noise)
            r[i * 2] = v & 0xFF;
            r[i * 2 + 1] = (v >> 8) & 0xFF;
        }
    }
    return ESP_OK;
}

int gpio_get_level(int pin) { return pin == 0 ? !boot_down : 1; }

// ---------------------------------------------------------------- battery (ADC through a divide-by-3) and USB
static volatile int batt_mv = 4050;
static volatile bool on_usb = true;

void sim_set_battery(int mv, bool usb) { batt_mv = mv; on_usb = usb; }
int sim_battery_mv(void) { return batt_mv; }
bool sim_usb(void) { return on_usb; }
bool usb_serial_jtag_is_connected(void) { return on_usb; }

struct sim_adc { int dummy; };
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *out) { *out = calloc(1, sizeof **out); return ESP_OK; }
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch, const adc_oneshot_chan_cfg_t *c) { return ESP_OK; }
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw)
{
    int cell = batt_mv + (on_usb ? 270 : 0);           // (on USB the charger lifts what the pin sees, as on the board)
    int mv = cell / 3 + (int)(arc4random() % 7) - 3;   // (at the pin, with a little noise)
    *raw = mv * 4095 / 3300;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *out) { *out = (adc_cali_handle_t)1; return ESP_OK; }
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) { *mv = raw * 3300 / 4095; return ESP_OK; }

// ---------------------------------------------------------------- restarts and deep sleep
static uint64_t wake_after_us;

// Start the process over, as the board starts over (the app keeps its window where it was)
static void start_over(const char *why)
{
    ESP_LOGW("sim", "restarting (%s)", why);
    char path[1024];
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) == 0) {
        setenv("BOBBLE_SIM_WAKE", why, 1);
        execv(path, sim_argv);
    }
    _exit(1);
}

void esp_restart(void) { start_over("restart"); }

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { wake_after_us = us; return ESP_OK; }

esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void)
{
    const char *w = getenv("BOBBLE_SIM_WAKE");
    return w && !strcmp(w, "timer") ? ESP_SLEEP_WAKEUP_TIMER : ESP_SLEEP_WAKEUP_UNDEFINED;
}

// Everything off until the timer (or BOOT, as the board's reset button would)
void esp_deep_sleep_start(void)
{
    ESP_LOGW("sim", "deep sleep%s", wake_after_us ? "" : " (until BOOT)");
    pthread_mutex_lock(&lock);
    disp_on = false;
    glass_seq++;
    pthread_mutex_unlock(&lock);
    int64_t until = wake_after_us ? esp_timer_get_time() + (int64_t)wake_after_us : INT64_MAX;
    while (esp_timer_get_time() < until && !boot_down) usleep(50000);
    start_over(boot_down ? "reset" : "timer");
}

// ---------------------------------------------------------------- odds and ends
const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ERROR";
    }
}

uint32_t esp_get_free_heap_size(void) { return 180 * 1024; }
esp_err_t esp_crt_bundle_attach(void *conf) { return ESP_OK; }

esp_err_t esp_wifi_restore(void)                       // (factory reset: the remembered networks go with NVS)
{
    return ESP_OK;
}

static esp_partition_t slot = {.size = 0x400000, .label = "ota_0"};
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *s) { return &slot; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &slot; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *st) { *st = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *out)
{
    ESP_LOGW("sim", "an update was sent: the emulator runs the firmware it was built with (rebuild it instead)");
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { return ESP_ERR_NOT_SUPPORTED; }

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d = {.version = "sim", .project_name = "amoled_demo", .time = __TIME__, .date = __DATE__};
    return &d;
}

// ---------------------------------------------------------------- the log: to stderr (Xcode-free: Console or the terminal)
static vprintf_like_t log_out = vprintf;
static struct { char tag[24]; esp_log_level_t level; } levels[16];
static int n_levels;

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    for (int i = 0; i < n_levels; i++) if (!strcmp(levels[i].tag, tag)) { levels[i].level = level; return; }
    if (n_levels < 16) { strlcpy(levels[n_levels].tag, tag, 24); levels[n_levels++].level = level; }
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t f)
{
    vprintf_like_t old = log_out;
    log_out = f;
    return old;
}

uint32_t esp_log_timestamp(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int log_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = log_out(fmt, ap);
    va_end(ap);
    return n;
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...)
{
    for (int i = 0; i < n_levels; i++) if (!strcmp(levels[i].tag, tag) && level > levels[i].level) return;
    if (level > ESP_LOG_INFO) return;
    static const char letter[] = " EWIDV";
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_print("%c (%u) %s: %s\n", letter[level], esp_log_timestamp(), tag, msg);
    fflush(stdout);
}

// ---------------------------------------------------------------- start-up
static void *run_app(void *arg)
{
    pthread_setname_np("main");
    app_main();
    return NULL;
}

// First launch (no "flash" yet): already on the pretend Wi-Fi, and paired with this Mac's Bobble app (its key), so it
// works straight away. A factory reset in Settings still gives the out-of-the-box setup.
static void first_run(void)
{
    if (sim_nvs_exists()) return;
    nvs_handle_t h;
    struct { char ssid[33], pass[65]; } net = {"Simulator Wi-Fi", ""};
    if (nvs_open("demo", NVS_READWRITE, &h) == ESP_OK) { nvs_set_blob(h, "wifinets", &net, sizeof net); nvs_close(h); }
    char path[1024], key[80] = "";
    snprintf(path, sizeof path, "%s/Library/Application Support/Bobble/display-key", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(key, sizeof key, f)) key[strcspn(key, "\r\n ")] = 0;
        fclose(f);
    }
    if (key[0] && nvs_open("agent", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "key", key); nvs_close(h); }
}

void sim_start(int argc, char **argv)
{
    sim_argv = argv;
    first_run();
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t th;
    pthread_create(&th, &a, run_app, NULL);
}
