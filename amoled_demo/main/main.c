// Bobble: a desk display for your coding agents (and calendar, music), on a Waveshare ESP32-S3-Touch-AMOLED-1.43.
// The calendar is the home screen; Wi-Fi settings is a card you pull down from the top edge
// (or open with BOOT); it also holds touch and tilt calibration. Holding BOOT for 3 s recalibrates touch.
// The screen fades off after a minute without use and wakes on touch, BOOT, or picking the board up.
// Swipe left from the calendar for Spotify's Now Playing (it also comes up by itself when music starts),
// right for the pet: a pair of robot eyes.
// The motion sensor keeps everything upright at any angle, spinning smoothly as the board turns.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "gfx.h"
#include "wifi_setup.h"
#include "wifi_ui.h"
#include "calendar.h"
#include "calendar_ui.h"
#include "battery.h"
#include "ota.h"
#include "spotify.h"
#include "spotify_ui.h"
#include "library_ui.h"
#include "pet_ui.h"
#include "agent.h"
#include "screen.h"
#include "statusbar.h"
#include "remote_log.h"
#include "board_square.h"

static const char *TAG = "main";
static bool show_empty;              // battery ran out: showing "plug in" before shutting down

#define STRIP 26                     // lines per transfer (even: the panel wants even-aligned windows)
#define NBUF 2
#define FRAME_US 33000               // ~30 fps for ordinary updates...
#define FRAME_US_MOVING 4000         // ...and as fast as frames can be made while things move (smaller steps = less tearing)

#define PIN_CS   9
#define PIN_CLK  10
#define PIN_D0   11
#define PIN_D1   12
#define PIN_D2   13
#define PIN_D3   14
#define PIN_RST  21
#define PIN_SDA  47
#define PIN_SCL  48
#define PIN_BOOT 0

// ---------------------------------------------------------------- display
static const sh8601_lcd_init_cmd_t co5300_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 1},
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static SemaphoreHandle_t buf_free;
static uint16_t *bufs[NBUF];

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(buf_free, &hp);
    return hp == pdTRUE;
}

static void display_init(void)
{
#if SQUARE_HW
    sq_display_init();
    buf_free = xSemaphoreCreateCounting(NBUF, NBUF);
    for (int i = 0; i < NBUF; i++) {
        bufs[i] = heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(bufs[i]);
    }
    return;
#endif
    spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(PIN_CLK, PIN_D0, PIN_D1, PIN_D2, PIN_D3, W * STRIP * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(PIN_CS, on_trans_done, NULL);
    io_cfg.pclk_hz = 80 * 1000 * 1000;              // the S3 drives these pins fine at 80 MHz; was 40
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    sh8601_vendor_config_t vendor = {
        .init_cmds = co5300_init,
        .init_cmds_size = sizeof(co5300_init) / sizeof(co5300_init[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io, &pcfg, &panel));
    panel_io = io;
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 6, 0));   // CO5300 panel is offset by 6 columns
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    buf_free = xSemaphoreCreateCounting(NBUF, NBUF);
    for (int i = 0; i < NBUF; i++) {
        bufs[i] = heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(bufs[i]);
    }
}

// ---------------------------------------------------------------- orientation (QMI8658 accelerometer)
// The content is turned clockwise on the glass by `ang` degrees, easing towards `ang_target`.
static float ang = 0, ang_target = 0;
static i2c_master_dev_handle_t imu_dev;
static bool imu_ok;
static float acc_x, acc_y, acc_z;   // filtered, in g

// Sensor model from the 6-position calibration: corrected = (raw - bias) / scale per axis, then R turns
// it into screen axes (x right, y down, z out of the glass). At rest the sensor reads the "up" vector.
typedef struct { uint32_t magic; float bias[3], scale[3], R[9]; } imu_cal_t;
#define IMU_CAL_MAGIC 0x1C0CA106
static imu_cal_t imu_cal = {0, {0, 0, 0}, {1, 1, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};

static void imu_cal_load(void)
{
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return;
    imu_cal_t c;
    size_t len = sizeof c;
    bool ok = nvs_get_blob(h, "tilt3", &c, &len) == ESP_OK && len == sizeof c && c.magic == IMU_CAL_MAGIC;
    if (ok) imu_cal = c;
    else {
        // older 2-direction calibration (up/right in the sensor's x-y plane) -> equivalent rotation
        struct { uint32_t magic; float bx, by, ux, uy, rx, ry; } old;
        len = sizeof old;
        if (nvs_get_blob(h, "calib", &old, &len) == ESP_OK && len == sizeof old && old.magic == 0xCA11B002) {
            float out = old.rx * old.uy - old.ry * old.ux > 0 ? 1 : -1;
            float R[9] = {-old.rx, -old.ry, 0, old.ux, old.uy, 0, 0, 0, out};
            memcpy(imu_cal.R, R, sizeof R);
        }
    }
    nvs_close(h);
    ESP_LOGI("main", "tilt calibration: %s", ok ? "6-position" : "basic/guessed");
}

// Up vector in screen axes from the current (filtered) reading
static void imu_up(float up[3])
{
    float a[3] = {acc_x, acc_y, acc_z}, v[3];
    for (int i = 0; i < 3; i++) v[i] = (a[i] - imu_cal.bias[i]) / imu_cal.scale[i];
    for (int i = 0; i < 3; i++) up[i] = imu_cal.R[i * 3] * v[0] + imu_cal.R[i * 3 + 1] * v[1] + imu_cal.R[i * 3 + 2] * v[2];
}

static void imu_init(i2c_master_bus_handle_t bus)
{
    uint8_t addr = i2c_master_probe(bus, 0x6B, 50) == ESP_OK ? 0x6B : 0x6A;
    i2c_device_config_t ic = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &ic, &imu_dev) != ESP_OK) return;
    uint8_t rst[2] = {0x60, 0xB0};                    // soft reset
    i2c_master_transmit(imu_dev, rst, 2, 50);
    vTaskDelay(pdMS_TO_TICKS(20));
    const uint8_t cfg[][2] = {
        {0x02, 0x60},   // CTRL1: register auto-increment
        {0x03, 0x05},   // CTRL2: accel +-2g, ~235 Hz
        {0x08, 0x01},   // CTRL7: accel on
    };
    for (int i = 0; i < 3; i++) i2c_master_transmit(imu_dev, cfg[i], 2, 50);
    imu_ok = true;
    imu_cal_load();
}

static void imu_read(void)
{
    uint8_t reg = 0x35, a[6];
    if (!imu_ok || i2c_master_transmit_receive(imu_dev, &reg, 1, a, 6, 20) != ESP_OK) return;
    acc_x = acc_x * 0.8f + 0.2f * (int16_t)(a[0] | (a[1] << 8)) / 16384.0f;
    acc_y = acc_y * 0.8f + 0.2f * (int16_t)(a[2] | (a[3] << 8)) / 16384.0f;
    acc_z = acc_z * 0.8f + 0.2f * (int16_t)(a[4] | (a[5] << 8)) / 16384.0f;
}

static float wrap180(float a)
{
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

// Point the content's "down" along gravity. Lying flat keeps the current angle; wobbles under 3
// degrees are ignored, and anything within 5 degrees of straight snaps to it so text stays crisp.
// Point the content's "down" along gravity. Lying flat keeps the current angle; wobbles under 3
// degrees are ignored, and anything within 5 degrees of straight snaps to it so text stays crisp.
static void update_orientation(void)
{
#if SQUARE_HW
    return;                                           // (the 4" board stays upright: its sensor's mounting isn't mapped yet)
#endif
    float up[3];
    imu_up(up);
    float gx = -up[0], gy = -up[1];                   // gravity on the glass
    if (sqrtf(gx * gx + gy * gy) < 0.45f) return;     // too flat to tell
    float a = atan2f(-gx, gy) * 180 / (float)M_PI;    // content down = (-sin a, cos a)
    float nearest = roundf(a / 90) * 90;
    if (fabsf(a - nearest) < 5) a = nearest;
    if (fabsf(wrap180(a - ang_target)) > 3) ang_target = wrap180(a);
}

static void animate_rotation(void)
{
    float d = wrap180(ang_target - ang);
    if (fabsf(d) < 0.3f) ang = ang_target;
    else ang = wrap180(ang + d * 0.22f);
}

// ---------------------------------------------------------------- tilt calibration (6 positions)
// Hold the board still in each position for a second. From the six readings we get each axis's
// offset and sensitivity, and the sensor's exact 3D mounting relative to the screen.
#define TILT_POSES 6
static const float pose_up[TILT_POSES][3] = {   // what the up vector should be, in screen axes
    {0, -1, 0},    // upright, bottom edge down
    {-1, 0, 0},    // right edge down
    {0, 1, 0},     // upside down
    {1, 0, 0},     // left edge down
    {0, 0, 1},     // flat, screen up
    {0, 0, -1},    // flat, screen down
};
static int tilt_step;               // 0 = idle, 1..6 = position being captured, 7 = done, 8 = failed
static float tilt_progress, pose_raw[TILT_POSES][3], sum[3], last_a[3];
static int64_t tilt_since;
static int nsum;

static void tilt_cal_start(void)
{
    tilt_step = 1;
    tilt_since = 0;
    tilt_progress = 0;
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static bool inv3(const float *m, float *o)
{
    float d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabsf(d) < 1e-6f) return false;
    o[0] = (m[4] * m[8] - m[5] * m[7]) / d; o[1] = (m[2] * m[7] - m[1] * m[8]) / d; o[2] = (m[1] * m[5] - m[2] * m[4]) / d;
    o[3] = (m[5] * m[6] - m[3] * m[8]) / d; o[4] = (m[0] * m[8] - m[2] * m[6]) / d; o[5] = (m[2] * m[3] - m[0] * m[5]) / d;
    o[6] = (m[3] * m[7] - m[4] * m[6]) / d; o[7] = (m[1] * m[6] - m[0] * m[7]) / d; o[8] = (m[0] * m[4] - m[1] * m[3]) / d;
    return true;
}

// Fit bias/scale per axis and the closest pure rotation (or mirror) from the six poses.
// Returns the worst remaining error in degrees, or -1 if the poses were unusable.
static float tilt_cal_solve(imu_cal_t *c)
{
    for (int i = 0; i < 3; i++) {                      // each axis sees about +1 g in one pose and -1 g in another
        float lo = 9, hi = -9;
        for (int p = 0; p < TILT_POSES; p++) {
            if (pose_raw[p][i] < lo) lo = pose_raw[p][i];
            if (pose_raw[p][i] > hi) hi = pose_raw[p][i];
        }
        c->bias[i] = (hi + lo) / 2;
        c->scale[i] = (hi - lo) / 2;
        if (c->scale[i] < 0.8f || c->scale[i] > 1.2f) { c->bias[i] = 0; c->scale[i] = 1; }   // axis not lined up: skip
    }
    float v[TILT_POSES][3], M[9] = {0};
    for (int p = 0; p < TILT_POSES; p++) {
        for (int i = 0; i < 3; i++) v[p][i] = (pose_raw[p][i] - c->bias[i]) / c->scale[i];
        float n = sqrtf(dot3(v[p], v[p]));
        for (int i = 0; i < 3; i++) v[p][i] /= n;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) M[r * 3 + k] += pose_up[p][r] * v[p][k];
    }
    // polar decomposition by Newton iteration: R <- (R + R^-T) / 2 converges to the nearest orthogonal matrix
    float R[9], inv[9];
    memcpy(R, M, sizeof R);
    for (int it = 0; it < 20; it++) {
        if (!inv3(R, inv)) return -1;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) R[r * 3 + k] = 0.5f * (R[r * 3 + k] + inv[k * 3 + r]);
    }
    memcpy(c->R, R, sizeof R);
    float worst = 0;
    for (int p = 0; p < TILT_POSES; p++) {
        float u[3];
        for (int r = 0; r < 3; r++) u[r] = R[r * 3] * v[p][0] + R[r * 3 + 1] * v[p][1] + R[r * 3 + 2] * v[p][2];
        float e = acosf(fminf(1, dot3(u, pose_up[p]))) * 180 / (float)M_PI;
        if (e > worst) worst = e;
    }
    return worst;
}

static void tilt_cal_update(int64_t now)
{
    if (tilt_step >= 7) {                              // result message
        if (now - tilt_since > 2500000) tilt_step = tilt_step == 8 ? 1 : 0;   // failed: start over
        if (tilt_step == 1) tilt_since = 0;
        return;
    }
    float a[3] = {acc_x, acc_y, acc_z};
    float m = sqrtf(dot3(a, a));
    bool still = fabsf(a[0] - last_a[0]) + fabsf(a[1] - last_a[1]) + fabsf(a[2] - last_a[2]) < 0.02f;
    memcpy(last_a, a, sizeof a);
    bool ok = still && m > 0.8f && m < 1.2f;
    for (int p = 0; ok && p < tilt_step - 1; p++)      // must be a new position (> 60 degrees from the others)
        ok = dot3(a, pose_raw[p]) / (m * sqrtf(dot3(pose_raw[p], pose_raw[p]))) < 0.5f;
    if (!ok) { tilt_since = 0; tilt_progress = 0; return; }
    if (!tilt_since) { tilt_since = now; sum[0] = sum[1] = sum[2] = 0; nsum = 0; }
    for (int i = 0; i < 3; i++) sum[i] += a[i];
    nsum++;
    tilt_progress = (now - tilt_since) / 1000000.0f;
    if (tilt_progress < 1) return;

    for (int i = 0; i < 3; i++) pose_raw[tilt_step - 1][i] = sum[i] / nsum;   // average over the second
    tilt_since = now;
    tilt_progress = 0;
    if (tilt_step < TILT_POSES) { tilt_step++; tilt_since = 0; return; }

    imu_cal_t c = {IMU_CAL_MAGIC};
    float worst = tilt_cal_solve(&c);
    ESP_LOGI("main", "tilt cal: bias %.3f %.3f %.3f scale %.3f %.3f %.3f, worst error %.1f deg",
             c.bias[0], c.bias[1], c.bias[2], c.scale[0], c.scale[1], c.scale[2], worst);
    if (worst >= 0 && worst < 12) {
        imu_cal = c;
        nvs_handle_t h;
        if (nvs_open("demo", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "tilt3", &imu_cal, sizeof imu_cal);
            nvs_commit(h);
            nvs_close(h);
        }
        tilt_step = 7;
    } else {
        tilt_step = 8;                                 // positions didn't agree
    }
    tilt_since = now;
}

static void draw_arrow(uint16_t *buf, int y0, int h, int dir, uint16_t c)   // 0 down, 1 right, 2 up, 3 left
{
    for (int i = 0; i < 34; i++) {
        int w = 34 - i;                                // head narrows towards the tip
        switch (dir) {
        case 0: fill_rect(buf, y0, h, CX - w, 392 + i, CX + w, 393 + i, c); break;
        case 1: fill_rect(buf, y0, h, 392 + i, CY - w, 393 + i, CY + w, c); break;
        case 2: fill_rect(buf, y0, h, CX - w, 73 - i, CX + w, 74 - i, c); break;
        case 3: fill_rect(buf, y0, h, 73 - i, CY - w, 74 - i, CY + w, c); break;
        }
    }
    switch (dir) {                                     // shaft
    case 0: fill_rect(buf, y0, h, CX - 9, 352, CX + 9, 392, c); break;
    case 1: fill_rect(buf, y0, h, 352, CY - 9, 392, CY + 9, c); break;
    case 2: fill_rect(buf, y0, h, CX - 9, 74, CX + 9, 114, c); break;
    case 3: fill_rect(buf, y0, h, 74, CY - 9, 114, CY + 9, c); break;
    }
}

static void tilt_cal_draw(uint16_t *buf, int y0, int h)
{
    uint16_t white = rgb(255, 255, 255), accent = rgb(0, 230, 200), grey = rgb(70, 70, 90), dim = rgb(140, 140, 160);
    if (tilt_step == 7) { text_c(buf, y0, h, 210, 3, "Tilt calibrated", accent); return; }
    if (tilt_step == 8) {
        text_c(buf, y0, h, 190, 3, "Let's try again", rgb(255, 150, 60));
        text_c(buf, y0, h, 236, 2, "the positions didn't agree", dim);
        return;
    }
    static const char *title[] = {"Stand it upright", "Right edge down", "Upside down", "Left edge down",
                                  "Lay it flat", "Last: face down"};
    static const char *sub[] = {"arrow pointing at the table", "arrow pointing at the table", "arrow pointing at the table",
                                "arrow pointing at the table", "screen facing up", "screen on the table, then flip back"};
    int i = tilt_step - 1;
    text_c(buf, y0, h, 172, 3, title[i], white);
    text_c(buf, y0, h, 212, 2, sub[i], dim);
    if (i < 4) draw_arrow(buf, y0, h, i, accent);
    char step[48];
    snprintf(step, sizeof step, "Position %d of %d - hold still", tilt_step, TILT_POSES);
    text_c(buf, y0, h, 146, 1, step, dim);
    fill_rect(buf, y0, h, CX - 100, 262, CX + 100, 272, grey);
    fill_rect(buf, y0, h, CX - 100, 262, CX - 100 + (int)(200 * (tilt_progress > 1 ? 1 : tilt_progress)), 272, accent);
}

// ---------------------------------------------------------------- touch (FT3168)
static i2c_master_dev_handle_t touch_dev;

static void input_init(void)
{
    i2c_master_bus_handle_t bus;
#if SQUARE_HW
    bus = sq_i2c();
#else
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
#endif
    {                                                 // what's on the bus (in the log): touch, motion sensor, clock... any audio chips?
        char found[160] = "";
        for (int a = 0x08; a < 0x78; a++)
            if (i2c_master_probe(bus, a, 20) == ESP_OK) snprintf(found + strlen(found), sizeof found - strlen(found), " 0x%02x", a);
        ESP_LOGI(TAG, "i2c devices:%s", found);
    }
#if SQUARE_HW
    sq_touch_init();
#else
    i2c_device_config_t tc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x38, .scl_speed_hz = 400000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &tc, &touch_dev));
    uint8_t mode[2] = {0x00, 0x00};                   // normal operating mode
    i2c_master_transmit(touch_dev, mode, 2, 50);
#endif

    gpio_config_t g = {.pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&g);
    imu_init(bus);
}

// Touch calibration: screen = raw * a + b per axis, fitted from taps on known targets
typedef struct { uint32_t magic; float ax, bx, ay, by; } touch_cal_t;
#define TOUCH_CAL_MAGIC 0x70C4CA13   // bump to make everyone recalibrate
static touch_cal_t tcal = {0, 1, 0, 1, 0};

static bool touch_cal_load(void)
{
    nvs_handle_t h;
    touch_cal_t c;
    size_t len = sizeof c;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, "touchcal", &c, &len) == ESP_OK && len == sizeof c && c.magic == TOUCH_CAL_MAGIC;
    nvs_close(h);
    if (ok) tcal = c;
    return ok;
}

static void touch_cal_save(void)
{
    nvs_handle_t h;
    tcal.magic = TOUCH_CAL_MAGIC;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "touchcal", &tcal, sizeof tcal);
    nvs_commit(h);
    nvs_close(h);
}

static bool read_touch_raw(int *x, int *y)
{
#if SQUARE_HW
    return sq_touch_read(x, y);
#endif
    uint8_t reg = 0x02, n = 0, p[4];
    if (i2c_master_transmit_receive(touch_dev, &reg, 1, &n, 1, 20) != ESP_OK || !(n & 0x0F)) return false;
    reg = 0x03;
    if (i2c_master_transmit_receive(touch_dev, &reg, 1, p, 4, 20) != ESP_OK) return false;
    *x = ((p[0] & 0x0F) << 8) | p[1];
    *y = ((p[2] & 0x0F) << 8) | p[3];
    return true;
}

static bool read_touch(int *x, int *y)
{
    int rx, ry;
    if (!read_touch_raw(&rx, &ry)) return false;
    // glass position -> position in the turned content (same mapping the renderer uses)
    float c = (W - 1) / 2.0f, dx = rx * tcal.ax + tcal.bx - c, dy = ry * tcal.ay + tcal.by - c;
    float co = cosf(ang * (float)M_PI / 180), si = sinf(ang * (float)M_PI / 180);
    *x = (int)lroundf(c + co * dx + si * dy);
    *y = (int)lroundf(c - si * dx + co * dy);
    return true;
}

// ---------------------------------------------------------------- touch calibration screen
static const int cal_pts[][2] = {{CX, CY}, {CX, 70}, {CX, H - 70}, {70, CY}, {W - 70, CY}};
#define N_CAL_PTS 5
static bool calibrating, cal_retry;
static int cal_idx, cal_raw[N_CAL_PTS][2], cal_tries;
static char cal_note[48];                // shown after a run: why it's starting over, or why it gave up

static void touch_cal_start(void)
{
    calibrating = true;
    cal_retry = false;
    cal_idx = 0;
    cal_tries = 0;
    cal_note[0] = 0;
}

// least squares fit of target = a * raw + b over the points in `use`
static void fit_axis(int axis, const bool *use, float *a, float *b)
{
    float sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (int i = 0; i < N_CAL_PTS; i++) {
        if (!use[i]) continue;
        float r = cal_raw[i][axis], t = cal_pts[i][axis];
        sx += r; sy += t; sxx += r * r; sxy += r * t; n++;
    }
    float d = n * sxx - sx * sx;
    *a = fabsf(d) > 1e-3f ? (n * sxy - sx * sy) / d : 1;
    *b = (sy - *a * sx) / n;
}

// Fit both axes; returns the worst remaining miss (px) over the points used, and which point that was
static float fit_all(const bool *use, touch_cal_t *c, int *worst_i)
{
    fit_axis(0, use, &c->ax, &c->bx);
    fit_axis(1, use, &c->ay, &c->by);
    float worst = 0;
    for (int i = 0; i < N_CAL_PTS; i++) {
        float ex = cal_raw[i][0] * c->ax + c->bx - cal_pts[i][0], ey = cal_raw[i][1] * c->ay + c->by - cal_pts[i][1];
        float e = sqrtf(ex * ex + ey * ey);
        if (use[i] && e > worst) { worst = e; *worst_i = i; }
    }
    return worst;
}

static bool cal_ok(const touch_cal_t *c, float worst)
{
    return worst <= 22 && c->ax > 0.8f && c->ax < 1.25f && c->ay > 0.8f && c->ay < 1.25f;
}

// Feed raw touches. Each tap counts where the finger rested: the average of its samples, leaving out
// the first and last ~60 ms (landing and lifting smear the position).
static void touch_cal_update(bool down, int rx, int ry, int64_t now)
{
    static bool was;
    static int64_t t_down;
    static struct { int x, y; int64_t t; } smp[64];
    static int ns;
    if (down && !was) { t_down = now; ns = 0; }
    if (down && ns < 64) { smp[ns].x = rx; smp[ns].y = ry; smp[ns].t = now; ns++; }
    if (!down && was && ns > 0) {
        int64_t t_up = smp[ns - 1].t;
        long sx = 0, sy = 0, n = 0;
        for (int i = 0; i < ns; i++)
            if (smp[i].t >= t_down + 60000 && smp[i].t <= t_up - 60000) { sx += smp[i].x; sy += smp[i].y; n++; }
        if (!n) for (int i = 0; i < ns; i++) { sx += smp[i].x; sy += smp[i].y; n++; }   // very quick tap
        cal_raw[cal_idx][0] = (int)(sx / n);
        cal_raw[cal_idx][1] = (int)(sy / n);
        ESP_LOGI(TAG, "touch cal point %d: target %d,%d raw %d,%d (%ld samples)", cal_idx, cal_pts[cal_idx][0], cal_pts[cal_idx][1],
                 cal_raw[cal_idx][0], cal_raw[cal_idx][1], n);
        cal_note[0] = 0;
        if (++cal_idx == N_CAL_PTS) {
            bool use[N_CAL_PTS] = {true, true, true, true, true};
            touch_cal_t c = {0};
            int wi = 0;
            float worst = fit_all(use, &c, &wi);
            if (!cal_ok(&c, worst)) {                  // one stray tap? drop it and fit the other four
                use[wi] = false;
                worst = fit_all(use, &c, &wi);
            }
            ESP_LOGI(TAG, "touch cal: x = %.3f*raw%+.1f, y = %.3f*raw%+.1f, worst miss %.0f px", c.ax, c.bx, c.ay, c.by, worst);
            if (cal_ok(&c, worst)) {
                tcal = c;
                touch_cal_save();
                calibrating = cal_retry = false;
            } else if (++cal_tries >= 3) {             // don't trap anyone: keep what we had
                calibrating = cal_retry = false;
                ESP_LOGW(TAG, "touch cal: giving up, keeping the previous calibration");
            } else {
                cal_idx = 0;
                cal_retry = true;
            }
        }
    }
    was = down;
}

#if !SCREEN_ROUND
// The square, in the settings' style: a blue ring with a soft halo to tap, the words in the Inter fonts, and the steps as
// a row of dots (done white, this one blue)
static void ftext_mid(uint16_t *buf, int y0, int h, int cx, int y, const font_t *f, const char *t, uint16_t col)
{
    draw_ftext(buf, y0, h, cx - ftext_width(f, t) / 2, y, f, t, col);
}

static void touch_cal_draw(uint16_t *buf, int y0, int h)
{
    const uint16_t white = rgb(255, 255, 255), blue = rgb(10, 132, 255), grey = rgb(150, 150, 158);
    int tx = cal_pts[cal_idx][0], ty = cal_pts[cal_idx][1];
    bool centre = cal_idx == 0;
    int top = centre ? 92 : CY - 70;                  // (the words sit clear of whichever dot is up)
    ftext_mid(buf, y0, h, CX, top, &font_body, "Touch calibration", white);
    if (cal_retry && centre) ftext_mid(buf, y0, h, CX, top + 46, &font_label, "That didn't line up. Try again.", rgb(255, 159, 10));
    else ftext_mid(buf, y0, h, CX, top + 46, &font_label, "Tap the middle of the dot", grey);
    int dy = centre ? 318 : CY + 34;
    for (int i = 0; i < N_CAL_PTS; i++)
        aa_disc(buf, y0, h, CX + (i - (N_CAL_PTS - 1) / 2.0f) * 18, dy, i == cal_idx ? 4.5f : 3.5f,
                i == cal_idx ? blue : i < cal_idx ? white : rgb(70, 70, 76));
    ftext_mid(buf, y0, h, CX, dy + 22, &font_small, "Press BOOT to cancel", rgb(100, 100, 108));
    aa_disc(buf, y0, h, tx, ty, 34, rgb(8, 34, 64));    // halo
    aa_disc(buf, y0, h, tx, ty, 22, blue);              // ring
    aa_disc(buf, y0, h, tx, ty, 19, rgb(8, 34, 64));
    aa_disc(buf, y0, h, tx, ty, 5, white);              // the point to hit
}
#else
static void touch_cal_draw(uint16_t *buf, int y0, int h)
{
    uint16_t white = rgb(255, 255, 255), accent = rgb(0, 230, 200), grey = rgb(130, 130, 150), dim = rgb(80, 80, 98);
    int tx = cal_pts[cal_idx][0], ty = cal_pts[cal_idx][1];
    bool centre = cal_idx == 0;
    int top = centre ? 140 : CY - 40;
    if (cal_retry && centre) text_c(buf, y0, h, 112, 2, "Didn't line up - try again", rgb(255, 150, 60));
    text_c(buf, y0, h, top, 2, "Tap the centre of the dot", white);
    char n[32];
    snprintf(n, sizeof n, "%d of %d", cal_idx + 1, N_CAL_PTS);
    text_c(buf, y0, h, centre ? 300 : CY + 28, 2, n, grey);
    text_c(buf, y0, h, centre ? 330 : CY + 56, 2, "Press BOOT to cancel", dim);
    fill_circle(buf, y0, h, tx, ty, 18, accent);
    fill_circle(buf, y0, h, tx, ty, 12, 0);
    fill_circle(buf, y0, h, tx, ty, 4, white);
}
#endif

// ---------------------------------------------------------------- screens
// Wi-Fi settings is a card that slides down over the calendar. `sheet` is the card's bottom edge in
// screen rows: 0 = hidden (calendar has the touch), H = fully open (Wi-Fi has the touch).
#define PULL_ZONE 48        // a drag starting this close to the top edge pulls the card down
#define PUSH_ZONE 384       // with the card open, an upward drag starting below this row pushes it back up
#define FLICK 9.0f          // px per frame: a quick flick finishes the move even if it was short

static float sheet = 0, vel = 0;
static int target = -1;     // animation target (0 or H), -1 when idle
static bool card_drag, card_live;   // finger is moving the card / Wi-Fi UI has been entered
static int grab_y;
static float grab_sheet;
static int frame_sheet;
static bool card_settled;   // the card just finished opening or closing     // snapshot of the card position used while rendering a frame

static void card_open_ui(void)
{
    if (!card_live) { wifi_ui_enter(); card_live = true; }
}

static void card_closed(void)
{
    if (card_live) { wifi_ui_leave(); card_live = false; }
    cal_ui_enter();
}

static void card_animate(int to)
{
    target = to;
    if (to == H) card_open_ui();
}

static void card_step(void)
{
    if (card_drag || target < 0) return;
    sheet += (target - sheet) * 0.32f;                 // ease out
    if (fabsf(target - sheet) < 1.5f) {
        sheet = target;
        if (target == 0) card_closed();
        card_settled = true;                           // draw the final picture now, not at the next clock tick
        target = -1;
    }
}

// Darken rows to k/256 of their brightness
static void dim_rows(uint16_t *buf, int y0, int from, int to, int k)
{
    for (int y = from; y < to; y++) {
        uint16_t *o = buf + (y - y0) * W;
        for (int x = 0; x < W; x++) {
            uint32_t c = SWAP16(o[x]), sp = (c | (c << 16)) & 0x07E0F81F;   // channels spread apart
            sp = ((sp * (uint32_t)(k >> 3)) >> 5) & 0x07E0F81F;   // 0..32 steps: no room for more without red spilling into green
            o[x] = SWAP16((uint16_t)(sp | (sp >> 16)));
        }
    }
}

// Pages you swipe between under the card: the pet, calendar, Spotify's Now Playing, and your Spotify library
enum { PAGE_PET, PAGE_CAL, PAGE_MUSIC, PAGE_LIB };
#define N_PAGES 4
static int page = PAGE_PET;                // the eyes are home: where it starts and wakes
static bool page_auto;     // Now Playing was opened automatically (so it may close itself too)

// Sideways page swipes: the page follows the finger with its neighbour sliding in beside it.
// pg_off > 0 means we're heading to the next page (content moving left), < 0 to the previous one.
static int sw_x0, sw_y0;
static bool pg_drag;               // finger is moving the pages
static float pg_off, pg_vel, pg_tgt;   // offset px, velocity px/s, target px
static int pg_dir;                 // +1 next page, -1 previous, 0 none
// Mid-swipe the pages don't change, only their position: each is drawn once into a full-screen snapshot
// and frames just copy the two snapshots at the current offset
// The current page's snapshot is captured as it's drawn; the neighbours are pre-drawn while you're not
// touching anything, so a swipe starts with no setup at all.
static uint16_t *snap_buf[3];             // [0] current page, [1] page to the left, [2] page to the right
static int snap_page[3] = {-2, -2, -2};   // which page each holds (-1 = empty/black, -2 = nothing yet)
static int64_t snap_time[3];
static uint16_t *snap_a, *snap_b;          // the two being composited this frame

// Where the battery bar sits: at the top edge on most screens, but inside the meeting ring on the
// calendar face, gliding up to the edge as the card is pulled down
static int status_bar_top(int fs)
{
    if (page != PAGE_CAL || !cal_ui_is_face()) return 18;
    int k = fs > 60 ? 60 : (fs < 0 ? 0 : fs);
    return 58 - 40 * k / 60;
}
// One page's background / foreground, for drawing pages side by side
static void page_bg(int p, uint16_t *buf, int y0, int y1)
{
    if (p == PAGE_MUSIC) spotify_ui_render_bg(buf, y0, y1);
    else if (p == PAGE_CAL) cal_ui_render_bg(buf, y0, y1);
    else memset(buf, 0, (y1 - y0) * W * 2);
}

// the pet's face stays clear of the battery until it's getting low
// (the square's corner is clear of everything: always there)
static bool status_shown(int p) { return !SCREEN_ROUND || p != PAGE_PET || (battery_percent() <= 15 && !battery_charging()); }

static void page_fg(int p, uint16_t *buf, int y0, int h)
{
    if (p == PAGE_MUSIC) spotify_ui_draw(buf, y0, h);
    else if (p == PAGE_LIB) library_ui_draw(buf, y0, h);
    else if (p == PAGE_PET) pet_ui_draw(buf, y0, h);
    else cal_ui_draw(buf, y0, h);
    if (SCREEN_ROUND && status_shown(p)) draw_status_bar(buf, y0, h, p == PAGE_CAL && cal_ui_is_face() ? 58 : 18);   // slides with its page
    // (the square's stays put over the sliding pages: drawn by draw_overlay and draw_strip instead)
}

// Square: while the card moves, the page under it is still, so frames copy it from its snapshot instead of drawing
// it again (drawing both the page and the card made the pull-down choppy)
static bool card_snap;
// ...and the card itself is drawn once as it starts to move and copied into place after that
static uint16_t *card_img;
static bool card_cached;
static int pg_render;              // page being snapshotted (read by both render cores)
static void page_render_bg(uint16_t *buf, int y0, int y1) { page_bg(pg_render, buf, y0, y1); }

static void render_bg(uint16_t *buf, int y0, int y1)
{
    int fs = frame_sheet;
    if (calibrating || tilt_step || fs >= H || ota_progress() >= 0 || show_empty) { memset(buf, 0, (y1 - y0) * W * 2); return; }
    int a = y0 > fs ? y0 : (fs < y1 ? fs : y1);        // first calendar row below the card, within this strip
    if (a > y0) memset(buf, 0, (a - y0) * W * 2);
    if (a < y1 && card_snap) memcpy(buf + (a - y0) * W, snap_buf[0] + a * W, (y1 - a) * W * 2);
    else if (a < y1) {
        if (page == PAGE_MUSIC) spotify_ui_render_bg(buf + (a - y0) * W, a, y1);
        else if (page == PAGE_LIB || page == PAGE_PET) memset(buf + (a - y0) * W, 0, (y1 - a) * W * 2);
        else cal_ui_render_bg(buf + (a - y0) * W, a, y1);
    }
}

static void draw_overlay(uint16_t *buf, int y0, int h)
{
    int fs = frame_sheet;
    if (ota_progress() >= 0) {                        // firmware update in progress
        char p[24];
        snprintf(p, sizeof p, "%d%%", ota_progress());
        text_c(buf, y0, h, 180, 3, "Updating", rgb(255, 255, 255));
        text_c(buf, y0, h, 222, 3, p, rgb(0, 230, 200));
        fill_rect(buf, y0, h, CX - 120, 270, CX + 120, 280, rgb(70, 70, 90));
        fill_rect(buf, y0, h, CX - 120, 270, CX - 120 + 240 * ota_progress() / 100, 280, rgb(0, 230, 200));
        text_c(buf, y0, h, 300, 1, "don't unplug", rgb(140, 140, 160));
        return;
    }
    if (show_empty) {
        fill_rrect(buf, y0, h, CX - 60, 150, CX + 52, 210, 10, rgb(220, 60, 60));    // empty battery outline
        fill_rrect(buf, y0, h, CX - 54, 156, CX + 46, 204, 6, rgb(0, 0, 0));
        fill_rect(buf, y0, h, CX + 52, 168, CX + 62, 192, rgb(220, 60, 60));
        text_c(buf, y0, h, 250, 3, "Battery empty", rgb(255, 255, 255));
        text_c(buf, y0, h, 292, 2, "Plug in to charge", rgb(140, 140, 160));
        return;
    }
    if (calibrating) { touch_cal_draw(buf, y0, h); return; }
    if (tilt_step) { tilt_cal_draw(buf, y0, h); return; }
    if (fs < H) {
        if (card_snap) {}                              // (already in the copied rows)
        else if (page == PAGE_MUSIC) spotify_ui_draw(buf, y0, h);
        else if (page == PAGE_LIB) library_ui_draw(buf, y0, h);
        else if (page == PAGE_PET) pet_ui_draw(buf, y0, h);
        else cal_ui_draw(buf, y0, h);
        if (fs == 0) {
            if (SCREEN_ROUND && status_shown(page)) draw_status_bar(buf, y0, h, status_bar_top(0));
            if (snap_buf[0]) {                         // keep the current page's snapshot up to date (round-clipped)
                for (int r = 0; r < h; r++) {
                    int y = y0 + r;
                    float dy = y - CY + 0.5f;
                    int half = SCREEN_ROUND ? (int)sqrtf(fmaxf(0, (W / 2) * (W / 2) - dy * dy)) : W / 2, xa = CX - half, xb = CX + half;
                    uint16_t *d = snap_buf[0] + y * W;
                    memset(d, 0, xa * 2);
                    memcpy(d + xa, buf + r * W + xa, (xb - xa) * 2);
                    memset(d + xb, 0, (W - xb) * 2);
                }
                if (y0 + h >= H) { snap_page[0] = page; snap_time[0] = esp_timer_get_time(); }
            }
            if (!SCREEN_ROUND) draw_status_bar(buf, y0, h, 0);   // (after the snapshot, which leaves it out)
            // no network to join (or still none after 20 s): say where Wi-Fi is set up; on the eyes, under them
            if ((page != PAGE_PET || agent_known()) && !calendar_loaded() && (wifi_state() == WS_PORTAL || (wifi_state() != WS_CONNECTED && esp_timer_get_time() > 20000000)))
                text_c(buf, y0, h, page == PAGE_PET ? 380 : 262, 1, "Swipe down from the top for Wi-Fi", rgb(140, 140, 160));
        } else {
            int a = y0 > fs ? y0 : fs;
            if (a < y0 + h) dim_rows(buf, y0, a, y0 + h, 256 - 192 * fs / H);   // fades as the card comes down
        }
    }
    if (fs <= 0) return;                               // (status bar already drawn with the page)

    // the card: Wi-Fi UI drawn shifted up by (H - fs) and clipped to the card's rows
    int off = fs - H, card_end = fs < y0 + h ? fs : y0 + h;
    if (card_end > y0 && fs < H) memset(buf, 0, (card_end - y0) * W * 2);
    int vy0 = y0 - off;                                // the strip in the card's own coordinates
    gfx_base_clip(H - fs, H);
    if (card_cached)                                   // (card row = glass row - off)
        for (int y = y0 > 0 ? y0 : 0; y < card_end; y++) memcpy(buf + (y - y0) * W, card_img + (y - off) * W, W * 2);
    else wifi_ui_draw(buf, vy0, h);
    fill_rrect(buf, vy0, h, CX - 22, H - 22, CX + 22, H - 17, 2, rgb(90, 90, 110));   // push-up handle
    if (fs < H) fill_rect(buf, vy0, h, CX - 120, H - 2, CX + 120, H, rgb(60, 60, 80)); // card edge
    gfx_base_clip(-100000, 100000);
    if (fs < H || wifi_ui_wants_status_bar()) draw_status_bar(buf, y0, h, status_bar_top(fs));   // stays put while the card slides
}

// ---------------------------------------------------------------- screen sleep
#ifdef BOBBLE_SIM
#define SLEEP_AFTER_US (365 * 24 * 3600 * 1000000LL)   // the simulator: never dozes off on its own (Lock still works)
#else
#define SLEEP_AFTER_US (60 * 1000000LL)
#endif
#define AUTO_LOCK_US (2 * 3600 * 1000000LL)   // asleep this long on battery: lock (radio + motion off, 3 s touch wakes)
enum { PW_AWAKE, PW_FADING_OUT, PW_ASLEEP, PW_FADING_IN };
static int power = PW_AWAKE, brightness = 255;
static int bright_pref = 255;         // chosen in Settings; saved in NVS
static float rest[3];                 // slowly-following accelerometer reading: where the board has been sitting

static void set_brightness(int b)
{
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    if (b == brightness) return;
    brightness = b;
#if SQUARE_HW
    sq_backlight(b);
    return;
#endif
    uint8_t v = b;
    esp_lcd_panel_io_tx_param(panel_io, (0x02 << 24) | (0x51 << 8), &v, 1);   // write display brightness (QSPI)
}

int screen_brightness_get(void) { return bright_pref; }

void screen_brightness_set(int level, bool save)
{
    bright_pref = level < 10 ? 10 : (level > 255 ? 255 : level);
    if (power == PW_AWAKE) set_brightness(bright_pref);
    if (!save) return;
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bright", (uint8_t)bright_pref);
    nvs_commit(h);
    nvs_close(h);
}

// Locked (Settings > Lock, held 3 s): the screen goes to sleep and only a finger held on it for 3 s wakes it,
// so bumps in a bag and the button do nothing. Kept in NVS so a restart stays dark too.
static uint32_t agent_seen;        // agent messages already acted on (woke the screen / showed the eyes)
// A coding agent sent something new since the last look: working on the next step, waiting for you, finished
// (not while locked: the radio's off then). Going idle doesn't count: nothing to show.
static bool agent_calls(void)
{
    agent_status_t ag;
    agent_get(&ag);
    bool call = ag.activity != agent_seen && ag.state != AG_IDLE;
    agent_seen = ag.activity;
    return call;
}
// On USB the radio stays fully awake so agent updates land at once (modem sleep adds up to ~1 s per packet and
// drops some); on battery it naps between beacons as usual. Re-applied every few seconds (the radio can restart).
static void wifi_power_update(int64_t now)
{
    static int64_t last;
    if (now - last < 5000000) return;
    last = now;
    esp_wifi_set_ps(battery_charging() ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}
static bool locked, auto_locked;   // auto_locked: by the 2-hour timer (a meeting may still wake it), not by hand

// Settings > Reset (held 3 s): wipe what a new owner shouldn't inherit (Wi-Fi, the Mac pairing key,
// calendars, Spotify, reminders, preferences) and restart into first-run setup. Touch and tilt
// calibration and the learned battery curve belong to this board, so they stay (and so does PHY cal).
static void factory_reset(void)
{
    static const char *keep[] = {"touchcal", "tilt3", "bsag", "bcurve", "blog"};
    ESP_LOGW(TAG, "factory reset");
    esp_wifi_restore();
    enum { MAX_ENTRIES = 256 };
    nvs_entry_info_t *list = malloc(MAX_ENTRIES * sizeof *list);   // (collect first: don't erase under the iterator)
    int n = 0;
    nvs_iterator_t it = NULL;
    for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it); e == ESP_OK && list && n < MAX_ENTRIES;
         e = nvs_entry_next(&it))
        nvs_entry_info(it, &list[n++]);
    nvs_release_iterator(it);
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].namespace_name, "phy")) continue;
        bool kept = false;
        for (int k = 0; k < sizeof keep / sizeof *keep; k++)
            if (!strcmp(list[i].namespace_name, "demo") && !strcmp(list[i].key, keep[k])) kept = true;
        nvs_handle_t h;
        if (kept || nvs_open(list[i].namespace_name, NVS_READWRITE, &h) != ESP_OK) continue;
        nvs_erase_key(h, list[i].key);
        nvs_commit(h);
        nvs_close(h);
    }
    free(list);
    esp_restart();
}

static void set_locked(bool on)
{
    locked = on;
    auto_locked = false;
    nvs_handle_t h;
    if (nvs_open("demo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "locked", on);
    nvs_commit(h);
    nvs_close(h);
}

static void brightness_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("demo", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK) bright_pref = v < 10 ? 10 : v;
    if (nvs_get_u8(h, "locked", &v) == ESP_OK) locked = v;
    nvs_close(h);
    set_brightness(bright_pref);
}

// Picked up / moved: the reading drifts away from where it has been resting
static bool board_moved(void)
{
    float d = fabsf(acc_x - rest[0]) + fabsf(acc_y - rest[1]) + fabsf(acc_z - rest[2]);
    rest[0] += (acc_x - rest[0]) * 0.03f;
    rest[1] += (acc_y - rest[1]) * 0.03f;
    rest[2] += (acc_z - rest[2]) * 0.03f;
    return d > 0.06f;
}

// ---------------------------------------------------------------- power while the screen is off
// Awake: flat out at 240 MHz for smooth animation. Screen off: the processor naps between checks (Wi-Fi stays
// joined in its low-power mode) and the display chip sleeps. Locked: the radio and motion sensor go off too.
static bool hw_locked;

static void panel_cmd(uint8_t cmd)
{
#if SQUARE_HW
    return;                                            // (the 4" panel's commands go through the slow expander: the light does it)
#endif
    esp_lcd_panel_io_tx_param(panel_io, (0x02 << 24) | (cmd << 8), NULL, 0);
}

static void imu_power(bool on)
{
    uint8_t c[2] = {0x08, on ? 0x01 : 0x00};          // CTRL7: accelerometer on/off
    if (imu_ok) i2c_master_transmit(imu_dev, c, 2, 50);
}

static void power_screen_off(void)
{
    panel_cmd(0x10);                                   // display chip: sleep in
    esp_pm_config_t pm = {.max_freq_mhz = 80, .min_freq_mhz = 40, .light_sleep_enable = true};
    esp_err_t e = esp_pm_configure(&pm);
    if (e != ESP_OK) ESP_LOGW(TAG, "power saving: %s", esp_err_to_name(e));
    if (locked && !hw_locked) {
        hw_locked = true;
        wifi_suspend(true);
        imu_power(false);
    }
}

static void power_screen_on(void)
{
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 240, .light_sleep_enable = false};
    esp_pm_configure(&pm);
    if (hw_locked) {
        hw_locked = false;
        imu_power(true);
        wifi_suspend(false);
    }
    panel_cmd(0x11);                                   // sleep out: the chip wants a moment before showing anything
    vTaskDelay(pdMS_TO_TICKS(80));
}

// Battery empty: everything off (deep sleep), looking every 5 minutes for the charger. Charging lifts the
// reading well above the empty point, and then the board starts up as usual.
#define EMPTY_CHECK_US (5 * 60 * 1000000LL)

// The 4" panel has no on/off line: there the backlight is the switch
static void panel_power(bool on)
{
#if SQUARE_HW
    if (!on) sq_backlight(0);
#else
    esp_lcd_panel_disp_on_off(panel, on);
#endif
}

static void sleep_until_charged(void)
{
    ESP_LOGW(TAG, "battery empty (%.2f V): sleeping until it's charged", battery_volts());
    panel_power(false);
    panel_cmd(0x10);
    imu_power(false);
    wifi_suspend(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_sleep_enable_timer_wakeup(EMPTY_CHECK_US);
    esp_deep_sleep_start();
}

// ---------------------------------------------------------------- frame pipeline (two cores)
typedef void (*rows_fn)(uint16_t *buf, int y0, int y1);   // fills rows y0..y1, buf points at row y0
static SemaphoreHandle_t job_go, job_done;
static struct { rows_fn fn; uint16_t *buf; int y0, y1, split; } job;

static void worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(job_go, portMAX_DELAY);
        job.fn(job.buf + (job.split - job.y0) * W, job.split, job.y1);
        xSemaphoreGive(job_done);
    }
}

// Run fn over rows y0..y0+h of buf, bottom half on the other core
static void split_rows(rows_fn fn, uint16_t *buf, int y0, int h)
{
    job.fn = fn; job.buf = buf; job.y0 = y0; job.y1 = y0 + h; job.split = y0 + h / 2;
    xSemaphoreGive(job_go);
    fn(buf, y0, job.split);
    xSemaphoreTake(job_done, portMAX_DELAY);
}

static uint16_t *frame;   // whole upright frame in PSRAM, only used when the content is rotated

// Per-frame rotation snapshot for the sampler
static int32_t rs_cos, rs_sin;   // 16.16 fixed point
static bool rs_smooth;
static int cmp_off;       // mid-swipe: how far the current page has moved
static bool rs_slide;     // mid-swipe at an angle: read the two page snapshots directly (no assembled frame)

static inline uint32_t spread565(uint16_t be) { uint32_t c = SWAP16(be); return (c | (c << 16)) & 0x07E0F81F; }
static inline uint16_t pack565(uint32_t x) { x &= 0x07E0F81F; return SWAP16((uint16_t)(x | (x >> 16))); }

// Only part of the glass is being redrawn (a circle of this radius around the centre; 0 = everything)
static int part_r;
// Columns of a strip to send: the round glass's widest row in the strip, narrowed to part_r's circle
static void strip_window(int y0, int h, int *x0, int *x1)
{
    if (!SCREEN_ROUND && !part_r) { *x0 = 0; *x1 = W; return; }   // (the square glass: every row is all of it)
    int dmin = H;                                       // the strip's row closest to the centre is its widest
    for (int y = y0; y < y0 + h; y++) {
        int d = abs(2 * y + 1 - H) / 2;
        if (d < dmin) dmin = d;
    }
    int R = part_r ? part_r : W / 2;
    int half = dmin >= R ? 0 : (int)sqrtf((float)(R * R - dmin * dmin)) + 2;
    int a = (CX - half) & ~1, b = (CX + half + 2) & ~1;   // the panel wants even window edges
    *x0 = a < 0 ? 0 : a;
    *x1 = b > W ? W : b;
}
static int win_x0, win_x1 = W;   // the window of the strip being sampled

// Glass rows y0..y1: each pixel looks up the upright frame at the turned position
// (content = centre + [cos sin; -sin cos] * (glass - centre)). Bilinear when settled, nearest while spinning.
// Works in 32-pixel-wide tiles so the frame data each tile needs stays in cache.
#define TILE 32
static void sample_rows(uint16_t *dst, int y0, int y1)
{
    const int32_t c = (W - 1) * 32768;                 // centre, 16.16
    const int R = W / 2;
    int xa_row[STRIP], xb_row[STRIP];
    for (int py = y0; py < y1; py++) {                 // visible span of each glass row (it's round)
        int dyi = 2 * py - (W - 1);
        int half = (int)sqrtf((float)(R * R) - dyi * dyi / 4.0f);
        int xa = W / 2 - half, xb = W / 2 + half;
        xa_row[py - y0] = xa < win_x0 ? win_x0 : xa;
        xb_row[py - y0] = xb > win_x1 ? win_x1 : xb;
        if (xb_row[py - y0] < xa_row[py - y0]) xb_row[py - y0] = xa_row[py - y0];
        uint16_t *d = dst + (py - y0) * W;
        memset(d, 0, xa_row[py - y0] * 2);
        memset(d + xb_row[py - y0], 0, (W - xb_row[py - y0]) * 2);
    }
    for (int tx = 0; tx < W; tx += TILE) {
        for (int py = y0; py < y1; py++) {
            int xa = xa_row[py - y0] > tx ? xa_row[py - y0] : tx;
            int xb = xb_row[py - y0] < tx + TILE ? xb_row[py - y0] : tx + TILE;
            if (xa >= xb) continue;
            uint16_t *d = dst + (py - y0) * W;
            int32_t dx0 = (2 * xa - (W - 1)) * 32768, dy = (2 * py - (W - 1)) * 32768;
            int32_t qx = c + (int32_t)(((int64_t)rs_cos * dx0 + (int64_t)rs_sin * dy) >> 16);
            int32_t qy = c + (int32_t)(((int64_t)-rs_sin * dx0 + (int64_t)rs_cos * dy) >> 16);
            if (rs_smooth) {
                for (int px = xa; px < xb; px++, qx += rs_cos, qy -= rs_sin) {
                    int ix = qx >> 16, iy = qy >> 16;
                    if ((unsigned)ix >= W - 1 || (unsigned)iy >= H - 1) { d[px] = 0; continue; }
                    uint32_t fx = (qx >> 11) & 31, fy = (qy >> 11) & 31;
                    const uint16_t *s0 = frame + iy * W + ix;
                    uint32_t a = spread565(s0[0]), b = spread565(s0[1]), e = spread565(s0[W]), f = spread565(s0[W + 1]);
                    uint32_t top = ((a * (32 - fx) + b * fx) >> 5) & 0x07E0F81F;
                    uint32_t bot = ((e * (32 - fx) + f * fx) >> 5) & 0x07E0F81F;
                    d[px] = pack565((top * (32 - fy) + bot * fy) >> 5);
                }
            } else if (rs_slide) {
                const int off = cmp_off;
                for (int px = xa; px < xb; px++, qx += rs_cos, qy -= rs_sin) {
                    int ix = (qx + 32768) >> 16, iy = (qy + 32768) >> 16;
                    if ((unsigned)ix >= W || (unsigned)iy >= H) { d[px] = 0; continue; }
                    int sx = ix + off;                 // current page shifted, the incoming one beside it
                    d[px] = sx >= W ? snap_b[iy * W + sx - W] : (sx < 0 ? snap_b[iy * W + sx + W] : snap_a[iy * W + sx]);
                }
            } else {
                for (int px = xa; px < xb; px++, qx += rs_cos, qy -= rs_sin) {
                    int ix = (qx + 32768) >> 16, iy = (qy + 32768) >> 16;
                    d[px] = ((unsigned)ix < W && (unsigned)iy < H) ? frame[iy * W + ix] : 0;
                }
            }
        }
    }
}

static bool pages_sliding(void)
{
    return (pg_drag || pg_off != 0) && frame_sheet <= 0 && !calibrating && !tilt_step && ota_progress() < 0;
}

static void snapshot(int p, uint16_t *dst)
{
    if (p < 0) { memset(dst, 0, W * H * 2); return; }   // nothing that way: rubber band shows black
    pg_render = p;
    for (int y0 = 0; y0 < H; y0 += STRIP) {
        int h = (y0 + STRIP > H) ? H - y0 : STRIP;
        split_rows(page_render_bg, dst + y0 * W, y0, h);
        page_fg(p, dst + y0 * W, y0, h);
    }
    for (int y = 0; y < H && SCREEN_ROUND; y++) {    // clip to the round glass: pages slide past as discs (square: whole)
        float dy = y - CY + 0.5f;
        int half = (int)sqrtf(fmaxf(0, (W / 2) * (W / 2) - dy * dy));
        int xa = CX - half, xb = CX + half;
        if (xa > 0) memset(dst + y * W, 0, xa * 2);
        if (xb < W) memset(dst + y * W + xb, 0, (W - xb) * 2);
    }
}

// Before drawing a mid-swipe frame: pick the snapshots (normally already prepared; drawn now only if not)
static void prepare_snapshots(void)
{
    int b = page + pg_dir;
    if (b < 0 || b >= N_PAGES || !pg_dir) b = -1;
    int slot = pg_dir < 0 ? 1 : 2;
    if (snap_page[0] != page) { snapshot(page, snap_buf[0]); snap_page[0] = page; snap_time[0] = esp_timer_get_time(); }
    if (snap_page[slot] != b) { snapshot(b, snap_buf[slot]); snap_page[slot] = b; snap_time[slot] = esp_timer_get_time(); }
    snap_a = snap_buf[0];
    snap_b = snap_buf[slot];
}

// While nothing is moving: keep the neighbouring pages' snapshots fresh (one per call, every ~10 s)
static void refresh_neighbour_snapshots(int64_t now)
{
    for (int slot = 1; slot <= 2; slot++) {
        int p = page + (slot == 1 ? -1 : 1);
        if (p < 0 || p >= N_PAGES) p = -1;
        if (snap_page[slot] != p || now - snap_time[slot] > 10000000) {
            snapshot(p, snap_buf[slot]);
            snap_page[slot] = p;
            snap_time[slot] = now;
            return;                                    // one at a time keeps each idle frame short
        }
    }
}

// Mid-swipe rows: the glass shows the current page shifted by cmp_off, the incoming page beside it
static void compose_rows(uint16_t *buf, int y0, int y1)
{
    int off = cmp_off;
    for (int y = y0; y < y1; y++) {
        uint16_t *o = buf + (y - y0) * W;
        const uint16_t *a = snap_a + y * W, *bb = snap_b + y * W;
        if (off >= 0) {
            memcpy(o, a + off, (W - off) * 2);
            memcpy(o + W - off, bb, off * 2);
        } else {
            memcpy(o, bb + W + off, -off * 2);
            memcpy(o - off, a, (W + off) * 2);
        }
    }
}

// Draw one strip of the screen: normally the current page (+ card), or two pages side by side mid-swipe
static void draw_strip(uint16_t *buf, int y0, int h)
{
    if (!pages_sliding() || !snap_a || !snap_b) {
        split_rows(render_bg, buf, y0, h);           // other core renders the bottom half of the strip
        draw_overlay(buf, y0, h);
        return;
    }
    int off = (int)lroundf(pg_off);
    cmp_off = off > W ? W : (off < -W ? -W : off);
    split_rows(compose_rows, buf, y0, h);            // both cores copy half the strip each
    if (!SCREEN_ROUND) draw_status_bar(buf, y0, h, 0);   // the square's status bar doesn't slide
}

// Send a finished strip to the glass, but only the part inside the round display: the square's
// corners are never visible, and skipping them saves about a fifth of every frame's transfer.
static void send_strip(uint16_t *buf, int y0, int h)
{
    int x0, x1;
    strip_window(y0, h, &x0, &x1);
    if (x0 < win_x0) x0 = win_x0;                      // (a partial turned frame: only the columns that were sampled)
    if (x1 > win_x1) x1 = win_x1;
    if (x1 <= x0) return;
    int w = x1 - x0;
    if (w < W)
        for (int r = 0; r < h; r++) memmove(buf + r * w, buf + r * W + x0, w * 2);   // pack the rows
#if SQUARE_HW
    sq_draw(x0, y0, x1, y0 + h, buf, buf_free);       // (frees the strip when its copy is done, as the round panel's DMA does)
#else
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y0 + h, buf);
#endif
}

static int why_lib, why_music, why_slide, why_rot;   // redraw reasons, for the status log
static bool quick_frame;   // an animation is playing: turn the picture with the quick lookup (the next still frame smooths it)
static bool frame_valid;   // `frame` holds the current content (false after drawing straight to the glass)
// Only this part of the content changed (the pet's eyes): redraw just the strips it touches. Off = whole screen.
static bool part_on;
static int part_rect[2][4], part_cnt;   // the changed areas of the page ({x0, y0, x1, y1}, upright), when part_on

// A strip of rows y0..y0+h overlaps one of the changed areas
static bool strip_in_rects(int y0, int h)
{
    for (int i = 0; i < part_cnt; i++) if (y0 + h > part_rect[i][1] && y0 < part_rect[i][3]) return true;
    return false;
}
static bool strip_skipped(int y0, int h, int a, int b) { return y0 + h <= a || y0 >= b; }

// content: the picture changed and must be redrawn (otherwise only the angle did)
static int64_t prof_draw_us, prof_turn_us;   // (profiling) drawing upright vs turning onto the glass and sending, rotated frames
static void render_frame(bool content)
{
    static int prev_sheet;
    frame_sheet = (int)(sheet + 0.5f);
    if (pages_sliding()) prepare_snapshots();
#if !SCREEN_ROUND
    card_snap = frame_sheet > 0 && frame_sheet < H && snap_buf[0] && !calibrating && !tilt_step && ota_progress() < 0 && !show_empty;
    // (the snapshot is kept up to date while the page shows; coming back from the open card it's from before, so
    // it's drawn afresh once)
    if (card_snap && (snap_page[0] != page || prev_sheet >= H)) {
        snapshot(page, snap_buf[0]);
        snap_page[0] = page;
        snap_time[0] = esp_timer_get_time();
    }
    bool card_moving = card_snap && card_img;
    if (card_moving && !card_cached) {
        for (int y0 = 0; y0 < H; y0 += STRIP) {
            int h = (y0 + STRIP > H) ? H - y0 : STRIP;
            memset(card_img + y0 * W, 0, h * W * 2);
            wifi_ui_draw(card_img + y0 * W, y0, h);
        }
    }
    card_cached = card_moving;
#endif
    prev_sheet = frame_sheet;
    if (ang != 0 && frame) {
        rs_slide = pages_sliding() && snap_a && snap_b;
        if (rs_slide) {                                // sample straight from the snapshots
            int off = (int)lroundf(pg_off);
            cmp_off = off > W ? W : (off < -W ? -W : off);
            frame_valid = false;
        }
#if !SCREEN_ROUND
        if (rs_slide) { rs_slide = false; content = true; }   // (compose upright, with the status bar on top, then turn)
#endif
        bool part = part_on && frame_valid && !rs_slide;
        int64_t p0 = esp_timer_get_time();
        for (int y0 = 0; y0 < H && !rs_slide && (content || !frame_valid); y0 += STRIP) {   // draw upright into the frame...
            int h = (y0 + STRIP > H) ? H - y0 : STRIP;
            if (part && !strip_in_rects(y0, h)) continue;
            uint16_t *buf = frame + y0 * W;
            draw_strip(buf, y0, h);
        }
        if (!rs_slide) frame_valid = true;
        int64_t p1 = esp_timer_get_time();
        prof_draw_us += p1 - p0;
        float r = ang * (float)M_PI / 180;
        rs_cos = (int32_t)lroundf(cosf(r) * 65536);
        rs_sin = (int32_t)lroundf(sinf(r) * 65536);
        // smooth (bilinear) only when everything is still; in motion the quick lookup looks the same
        rs_smooth = ang == ang_target && fmodf(fabsf(ang), 90) != 0 && !pages_sliding() && !card_drag && target < 0 && !quick_frame;
        // ...then turn it onto the glass: everything, or just where each changed area lands (its corners turned the
        // other way give the box on the glass, rows and columns)
        int bi = 0;
        for (int k = 0; k < (part ? part_cnt : 1); k++) {
            int ga = 0, gb = H, gx0 = 0, gx1 = W;
            if (part) {
                float co = cosf(r), si = sinf(r), mnx = W, mxx = 0, mny = H, mxy = 0;
                for (int q = 0; q < 4; q++) {
                    float px = part_rect[k][q & 1 ? 2 : 0] - (W - 1) / 2.0f, py = part_rect[k][q & 2 ? 3 : 1] - (W - 1) / 2.0f;
                    float gx = (W - 1) / 2.0f + co * px - si * py, gy = (W - 1) / 2.0f + si * px + co * py;
                    mnx = fminf(mnx, gx); mxx = fmaxf(mxx, gx); mny = fminf(mny, gy); mxy = fmaxf(mxy, gy);
                }
                ga = (int)mny - 3; gb = (int)mxy + 4; gx0 = (int)mnx - 3; gx1 = (int)mxx + 4;
            }
            for (int y0 = 0; y0 < H; y0 += STRIP) {
                int h = (y0 + STRIP > H) ? H - y0 : STRIP;
                if (part && strip_skipped(y0, h, ga, gb)) continue;
                strip_window(y0, h, &win_x0, &win_x1);
                if (part) {                            // (only the columns the area covers; even edges for the panel)
                    int a = gx0 & ~1, b = (gx1 + 1) & ~1;
                    if (a > win_x0) win_x0 = a;
                    if (b < win_x1) win_x1 = b;
                }
                if (win_x1 <= win_x0) continue;
                xSemaphoreTake(buf_free, portMAX_DELAY);
                split_rows(sample_rows, bufs[bi], y0, h);
                send_strip(bufs[bi], y0, h);
                bi = (bi + 1) % NBUF;
            }
        }
        part_r = 0;
        win_x0 = 0;
        win_x1 = W;
        prof_turn_us += esp_timer_get_time() - p1;
        return;
    }
    frame_valid = false;
    int bi = 0;
    for (int y0 = 0; y0 < H; y0 += STRIP) {
        int h = (y0 + STRIP > H) ? H - y0 : STRIP;
        if (part_on && !pages_sliding() && !strip_in_rects(y0, h)) continue;
        xSemaphoreTake(buf_free, portMAX_DELAY);
        uint16_t *buf = bufs[bi];
        draw_strip(buf, y0, h);
        send_strip(buf, y0, h);
        bi = (bi + 1) % NBUF;
    }
}

void app_main(void)
{
    // Drawing outranks the network tasks (Spotify, calendar: priority 3), so a TLS handshake waits for the
    // gaps between frames instead of freezing an animation for ~0.4 s
    vTaskPrioritySet(NULL, 6);
    remote_log_start();
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    // woken from the empty-battery sleep: carry on only if it's being charged
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        battery_init();
        float v = battery_read_now();
        if (v > 2.5f && v < battery_empty_volts() + 0.15f) {
            esp_sleep_enable_timer_wakeup(EMPTY_CHECK_US);
            esp_deep_sleep_start();
        }
        ESP_LOGI(TAG, "charging again (%.2f V): starting up", v);
    }
    display_init();
    cal_ui_init();
    wifi_setup_start();
    calendar_start();
    spotify_start();
    brightness_load();
    input_init();
    battery_init();
    cal_ui_enter();
    touch_cal_load();                                  // none saved: the panel's own mapping, fine out of the box
                                                       // (recalibrate from Settings, or hold BOOT for 3 s)
    if (locked) power = PW_FADING_OUT;                 // restarted while locked: straight back to sleep
    frame = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 3; i++) snap_buf[i] = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
#if !SCREEN_ROUND
    card_img = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
#endif
    for (int i = 0; i < 20; i++) { imu_read(); vTaskDelay(pdMS_TO_TICKS(5)); }
    update_orientation();
    ang = ang_target;                                 // start in the right orientation, no spin

    job_go = xSemaphoreCreateBinary();
    job_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(worker, "worker", 4096, NULL, 5, NULL, 1);

    int64_t log_t = esp_timer_get_time(), boot_down = 0, last_use = log_t;
    int frames = 0, boot_prev = 1, start_y = 0;
    int64_t render_us = 0, part_us = 0;
    int part_n = 0;
    bool was_down = false, boot_long = false, swallow_touch = false, force_redraw = false;
    rest[0] = acc_x; rest[1] = acc_y; rest[2] = acc_z;

    for (;;) {
        int64_t now = esp_timer_get_time();

        // BOOT: short press opens/closes Wi-Fi, hold 3 s to recalibrate the touch screen
        int boot = gpio_get_level(PIN_BOOT);
        bool boot_press = !boot && boot_prev;

        // asleep: screen off, just watch for a touch, BOOT, or the board being picked up
        if (power == PW_ASLEEP) {
            imu_read();
            battery_update();
            wifi_power_update(now);
            ota_confirm_if_healthy();                  // (a locked board can sleep straight through its first minute)
            if (battery_empty() && ota_progress() < 0) sleep_until_charged();
            static int64_t asleep_from;
            if (!asleep_from) asleep_from = now;
            if (!locked && !battery_charging() && ota_progress() < 0 && now - asleep_from > AUTO_LOCK_US) {
                ESP_LOGI(TAG, "untouched for 2 h: locking");
                set_locked(true);
                auto_locked = true;
                power_screen_off();                    // (turns the radio and motion sensor off now it's locked)
            }
            static int64_t asleep_log;
            if (now - asleep_log > 300000000) {        // every 5 minutes: how the battery is doing with the screen off
                asleep_log = now;
                ESP_LOGI(TAG, "asleep%s: battery %.2f V (%d%%%s)", locked ? " (locked)" : "", battery_volts(), battery_percent(),
                         battery_charging() ? ", charging" : "");
            }
            int rx, ry;
            bool touch = read_touch_raw(&rx, &ry), moved = board_moved();
            boot_prev = boot;
            bool updating = ota_progress() >= 0;
            // a meeting about to start: the screen comes on by itself (once per meeting), unless locked by hand
            static int64_t mt_checked;
            static time_t mt_woken;
            bool meeting = false;
            if (now - mt_checked > 5000000) {
                mt_checked = now;
                time_t m = pet_ui_meeting_window();
                meeting = m && m != mt_woken && (!locked || auto_locked);
                if (meeting) mt_woken = m;
            }
            if (meeting && locked) set_locked(false);
            bool agent = !locked && (agent_calls() || agent_pairing(NULL));   // a message from Claude, or a pairing code: show it
            meeting = meeting || agent;
            if (locked && !updating) {                 // locked: only a finger held on the glass for 3 s
                static int64_t hold_from, seen_at;
                if (touch) {
                    if (!hold_from || now - seen_at > 300000) hold_from = now;   // (short dropouts don't restart it)
                    seen_at = now;
                }
                if (!touch || now - hold_from < 3000000) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
                hold_from = 0;
                moved = false;
                set_locked(false);
            } else if (!touch && !moved && !boot_press && !updating && !meeting) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            asleep_from = 0;
            ESP_LOGI(TAG, "wake (%s)", agent ? "agent" : meeting ? "meeting" : touch ? (hw_locked ? "unlocked" : "touch") : moved ? "picked up" : updating ? "update" : "button");
            if (page != PAGE_PET && SCREEN_ROUND) { page = PAGE_PET; page_auto = false; cal_ui_enter(); }   // wakes to the eyes (home)
            // (the square wakes where it was left: it shouldn't pull you off a page you chose)
            power_screen_on();
            update_orientation();                      // face the right way straight away, no spin
            ang = ang_target;
            render_frame(true);                        // fresh picture before the light comes on
            panel_power(true);
            battery_set_screen(true);
            calendar_set_awake(true);
            spotify_set_awake(true);
            pet_ui_wake();
            power = PW_FADING_IN;
            last_use = now;
            swallow_touch = touch;                     // the waking tap doesn't press anything
            was_down = touch;
            continue;
        }

        if (!boot && boot_prev) { boot_down = now; boot_long = false; }
        if (!boot && !boot_long && now - boot_down > 3000000) { boot_long = true; touch_cal_start(); }
        if (boot && !boot_prev && !boot_long && calibrating) calibrating = false;   // BOOT cancels touch calibration
        else if (boot && !boot_prev && !boot_long && tilt_step) tilt_step = 0;       // ...and tilt calibration
        else if (boot && !boot_prev && !boot_long && power == PW_AWAKE) card_animate(frame_sheet >= H / 2 ? 0 : H);
        boot_prev = boot;

        if (calibrating || tilt_step) ang = ang_target = 0;   // calibrate on an unrotated screen
        if (tilt_step || calibrating) last_use = now;   // calibrating counts as use
        if (tilt_step) {
            imu_read();
            tilt_cal_update(now);
            render_frame(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (calibrating) {
            int rx = -1, ry = -1;
            bool d = read_touch_raw(&rx, &ry);
            touch_cal_update(d, rx, ry, now);
            was_down = d;
            render_frame(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        imu_read();
        battery_update();
        wifi_power_update(now);
        ota_confirm_if_healthy();
        if (ota_progress() >= 0) last_use = now;      // keep the screen on during an update
        static int64_t empty_at;
        if (battery_empty() && ota_progress() < 0) {  // say so for a few seconds, then everything off
            if (!empty_at) { empty_at = now; show_empty = true; force_redraw = true; }
            else if (now - empty_at > 4000000) sleep_until_charged();
        }
        if (!card_drag && !was_down && !pg_drag && pg_off == 0) update_orientation();   // never turn under a finger or mid-swipe
        animate_rotation();

        int tx = -1, ty = -1;
        bool down = read_touch(&tx, &ty);
        if (swallow_touch) {                           // finger that woke the screen: ignore until lifted
            if (down) { down = false; tx = ty = -1; }
            else swallow_touch = false;
        }
        if (down && !was_down) ESP_LOGI(TAG, "touch down %d,%d (card %d)", tx, ty, (int)sheet);

        // fade off after a minute without touches, button presses, or the board moving
        bool moved = board_moved();
        if (!locked && (down || boot_press || moved || swallow_touch)) {   // (locking: nothing stops the fade)
            last_use = now;
            if (power == PW_FADING_OUT) power = PW_FADING_IN;
        }
        // from 3 minutes before a meeting to 2 minutes in: stay on, and show the eyes if the board isn't in use
        static time_t mt_shown;
        time_t mt = locked ? 0 : pet_ui_meeting_window();
        if (mt) {
            if (mt != mt_shown && page != PAGE_PET && now - last_use > 10000000 && sheet <= 0 && !pg_drag && pg_off == 0 && !card_drag) {
                page = PAGE_PET; page_auto = false; force_redraw = true;
            }
            mt_shown = mt;
            last_use = now;
            if (power == PW_FADING_OUT) power = PW_FADING_IN;
        }
        // a message from the agent: the screen stays on for it (a minute from the last one, like a touch), and the
        // eyes come up if the board isn't in use; while it waits for you, stay on (up to 10 min)
        if (!locked && (agent_pairing(NULL) || agent_just_paired())) {   // pairing: the code's on the eyes page; stay on
            if (page != PAGE_PET && sheet <= 0 && !pg_drag && pg_off == 0 && !card_drag) { page = PAGE_PET; page_auto = false; force_redraw = true; }
            last_use = now;
            if (power == PW_FADING_OUT) power = PW_FADING_IN;
        }
        if (!locked && page == PAGE_PET && pet_ui_bubble_open()) {   // a message is up on the eyes: don't sleep under it
            last_use = now;
            if (power == PW_FADING_OUT) power = PW_FADING_IN;
        }
        // first run: no Wi-Fi yet (nothing saved, or none after 20 s), then no Mac yet: the eyes say what's next
        bool offline = wifi_state() == WS_PORTAL || (wifi_state() != WS_CONNECTED && now > 20000000);
        pet_ui_set_welcome(agent_known() ? 0 : offline ? 1 : 2);   // (set up already: a dropped Wi-Fi just gets the hint)
        if (pet_ui_wants_wifi() && sheet <= 0) { card_animate(H); wifi_ui_show_phone(); }
        if (!locked && agent_calls()) {               // (the square stays on whatever page you're on)
            if (SCREEN_ROUND && page != PAGE_PET && now - last_use > 10000000 && sheet <= 0 && !pg_drag && pg_off == 0 && !card_drag) {
                page = PAGE_PET; page_auto = false; force_redraw = true;
            }
            last_use = now;
            if (power == PW_FADING_OUT) power = PW_FADING_IN;
        }
        {
            agent_status_t ag;
            agent_get(&ag);
            if (!locked && ag.state == AG_WAITING && now - ag.since < 600000000) {
                last_use = now;
                if (power == PW_FADING_OUT) power = PW_FADING_IN;
            }
        }
        if (power == PW_AWAKE && now - last_use > SLEEP_AFTER_US && !card_drag) power = PW_FADING_OUT;
        if (power == PW_FADING_OUT) {
            set_brightness(brightness - 20);
            if (brightness == 0) {
                panel_power(false);
                battery_set_screen(false);
                calendar_set_awake(false);
                spotify_set_awake(false);
                power = PW_ASLEEP;
                ESP_LOGI(TAG, "screen off%s", locked ? " (locked)" : "");
                power_screen_off();
                continue;
            }
        } else if (power == PW_FADING_IN) {
            set_brightness(brightness + 40 < bright_pref ? brightness + 40 : bright_pref);
            if (brightness == bright_pref) { power = PW_AWAKE; force_redraw = true; }
        }

        bool page_has_touch = sheet <= 0 && target < 0 && !card_drag && !pg_drag && pg_off == 0;
        bool cal_has_touch = page_has_touch && page == PAGE_CAL, music_has_touch = page_has_touch && page == PAGE_MUSIC;
        bool lib_has_touch = page_has_touch && page == PAGE_LIB, pet_has_touch = page_has_touch && page == PAGE_PET;
        bool wifi_has_touch = sheet >= H && target < 0 && !card_drag;

        // start dragging the card. Down: only from the very top edge, so scrolling never opens it. Up: from the
        // bottom edge, or anywhere on card screens that don't scroll or type (once the drag is clearly vertical).
        static int start_x;
        if (down && !was_down) { start_x = tx; start_y = ty; }
        if (down && !card_drag && !pg_drag) {
            int ddx = abs(tx - start_x), ddy = ty - start_y;
            bool vertical_up = -ddy > 24 && -ddy > 3 * ddx / 2;
            if (page_has_touch && !was_down && ty < PULL_ZONE) {
                card_drag = true;
                card_open_ui();
            } else if (wifi_has_touch && ((start_y > PUSH_ZONE && -ddy > 30) || (vertical_up && wifi_ui_swipe_ok()))) {
                card_drag = true;
                wifi_ui_cancel_touch();
            }
            if (card_drag) { grab_y = start_y; grab_sheet = sheet; vel = 0; target = -1; }
        }
        if (card_drag) {
            if (down) {
                float ns = grab_sheet + (ty - grab_y);
                ns = ns < 0 ? 0 : (ns > H ? H : ns);
                vel = vel * 0.5f + (ns - sheet) * 0.5f;
                sheet = ns;
            } else {                                   // let go: finish or spring back
                card_drag = false;
                bool open = vel > FLICK ? true : vel < -FLICK ? false : sheet > H / 2;
                card_animate(open ? H : 0);
            }
            tx = ty = -1;
        }
        // swipe left/right between pages: once a drag is clearly sideways, the page lets go of the finger
        // (so no button underneath gets pressed) and follows it, with the neighbour sliding in
        if (down && !was_down) { sw_x0 = tx; sw_y0 = ty; }
        if (page_has_touch && down && tx >= 0 && !pg_drag) {
            int dx = tx - sw_x0, dy = ty - sw_y0;
            if (abs(dx) > 16 && abs(dx) * 2 > abs(dy) * 3) {
                pg_drag = true;
                pg_vel = 0;
                cal_ui_cancel_touch();
                spotify_ui_cancel_touch();
                library_ui_cancel_touch();
                pet_ui_cancel_touch();
            }
        }
        if (pg_drag) {
            static int64_t drag_t;
            if (down) {
                float off = (float)(sw_x0 - tx);
                int dir = off > 0 ? 1 : (off < 0 ? -1 : 0);
                if (page + dir < 0 || page + dir >= N_PAGES) off *= 0.3f;   // nothing that way: rubber band
                if (dir > 0 && page + 1 == PAGE_LIB && pg_dir != 1) library_ui_enter();   // start loading early
                float dts = drag_t ? (now - drag_t) / 1e6f : 0.02f;
                if (dts > 0.001f) pg_vel = pg_vel * 0.5f + 0.5f * (off - pg_off) / dts;   // smoothed finger speed
                drag_t = now;
                pg_off = off;
                pg_dir = dir;
            } else {                                   // let go: finish the move or spring back
                pg_drag = false;
                bool can = pg_dir && page + pg_dir >= 0 && page + pg_dir < N_PAGES;
                bool go = can && (fabsf(pg_off) > W * 0.3f || pg_vel * pg_dir > 500);   // far enough, or a flick
                pg_tgt = go ? pg_dir * W : 0;
                drag_t = 0;
            }
            tx = ty = -1;
        } else if (pg_off != 0) {                      // spring to the target, starting at the finger's speed
            static int64_t spring_t;
            float dts = spring_t && now - spring_t < 60000 ? (now - spring_t) / 1e6f : 0.016f;
            spring_t = now;
            // exact solution of a critically damped spring (no overshoot, ~0.25 s): unlike stepping the
            // physics, this can't start oscillating when frames arrive far apart
            const float w0 = 22;
            float x = pg_off - pg_tgt, e = expf(-w0 * dts), k = pg_vel + w0 * x;
            pg_off = pg_tgt + (x + k * dts) * e;
            pg_vel = (pg_vel - k * w0 * dts) * e;
            if (fabsf(pg_tgt - pg_off) < 1.5f && fabsf(pg_vel) < 60) {
                spring_t = 0;
                pg_vel = 0;
                if (pg_tgt != 0) {
                    if (page == PAGE_CAL) cal_ui_enter();   // leaving: come back to the clock face, not the agenda
                    int slot = pg_dir < 0 ? 1 : 2, other = 3 - slot;
                    uint16_t *t = snap_buf[other];         // old current page becomes the neighbour behind us
                    snap_buf[other] = snap_buf[0]; snap_page[other] = snap_page[0]; snap_time[other] = snap_time[0];
                    snap_buf[0] = snap_buf[slot]; snap_page[0] = snap_page[slot]; snap_time[0] = snap_time[slot];
                    snap_buf[slot] = t; snap_page[slot] = -2;
                    page += pg_dir;
                    page_auto = false;                 // you chose this page: don't flip it back by itself
                }
                pg_off = 0;
                pg_dir = 0;
                force_redraw = true;
            }
        }
        was_down = down;
        card_step();

        // Now Playing comes up by itself when music starts (if you're not using the board), and the
        // calendar comes back a minute after it stops
        static bool was_playing;
        static int64_t stopped_since;
        spotify_state_t sp;
        spotify_get(&sp);
        if (sp.playing && !was_playing && page == PAGE_PET && sheet <= 0 && now - last_use > 10000000) {
            page = PAGE_MUSIC; page_auto = true; force_redraw = true;
        }
        if (page == PAGE_MUSIC && page_auto) {
            if (sp.playing) stopped_since = 0;
            else if (!stopped_since) stopped_since = now;
            else if (now - stopped_since > 60000000) { page = PAGE_PET; page_auto = false; force_redraw = true; }   // back home
        }
        was_playing = sp.playing;
        spotify_set_poll_fast(page == PAGE_MUSIC && power == PW_AWAKE);
        spotify_set_library_visible(page == PAGE_LIB && power == PW_AWAKE);

        cal_ui_update(cal_has_touch && !card_drag ? tx : -1, cal_has_touch && !card_drag ? ty : -1, now);
        spotify_ui_update(music_has_touch && !card_drag ? tx : -1, music_has_touch && !card_drag ? ty : -1, now);
        library_ui_update(lib_has_touch && !card_drag ? tx : -1, lib_has_touch && !card_drag ? ty : -1, now);
        if (page == PAGE_PET) {
            float up[3];
            imu_up(up);
            pet_ui_update(pet_has_touch && !card_drag ? tx : -1, pet_has_touch && !card_drag ? ty : -1, now, up, ang,
                          power == PW_FADING_OUT ? 0 : SLEEP_AFTER_US - (now - last_use));
        }
        if (card_live) {
            int action = wifi_ui_update(wifi_has_touch && !card_drag && down ? tx : -1,
                                        wifi_has_touch && !card_drag && down ? ty : -1, now);
            if (action == WIFI_UI_CLOSE) card_animate(0);
            else if (action == WIFI_UI_CAL_TOUCH) touch_cal_start();
            else if (action == WIFI_UI_CAL_TILT) tilt_cal_start();
            else if (action == WIFI_UI_LOCK) {
                ESP_LOGI(TAG, "locked");
                set_locked(true);
                card_animate(0);
                power = PW_FADING_OUT;
            }
            else if (action == WIFI_UI_RESET) factory_reset();
        }

        // Redraw only when something can have changed: touches, the card, Wi-Fi screen animations, the
        // spin, or once a second for the clock. A still calendar costs almost nothing.
        static int64_t last_full;
        static float last_ang = 1000;
        static bool prev_down;
        if (card_settled) { force_redraw = true; card_settled = false; }
        bool lib_dirty = page == PAGE_LIB && library_ui_dirty(), music_dirty = page == PAGE_MUSIC && spotify_ui_dirty();
        bool pet_dirty = page == PAGE_PET && pet_ui_dirty();
        // the pet page has no clock: a full frame every 10 s is plenty, plus one (smoothly turned) once the eyes settle
        static int64_t pet_moved;
        if (pet_dirty) pet_moved = now;
        else if (pet_moved && now - pet_moved > 250000) { pet_moved = 0; force_redraw = true; }
        int64_t full_every = page == PAGE_PET && sheet <= 0 ? 10000000 : 1000000;
        bool others = force_redraw || pg_drag || pg_off != 0 || ota_progress() >= 0 || cal_ui_animating() || music_dirty || lib_dirty ||
                      ((down || prev_down) && page != PAGE_PET) || card_drag || target >= 0 || sheet > 0 || now - last_full >= full_every;
        bool content = others || pet_dirty;
        // just the eyes moved: redraw only around them (a full frame still comes every second for the clock and status)
        part_cnt = pet_dirty && !others && ang == last_ang ? pet_ui_take_rects(part_rect) : 0;
        part_on = part_cnt > 0;
        if (pet_dirty && !part_on) pet_ui_take_rect(NULL, NULL, NULL, NULL);
        // dragging the brightness slider on the open card: just its rows (the whole card made the slider lag)
        if (!part_on && sheet >= H && target < 0 && !card_drag && !force_redraw && ang == last_ang && ota_progress() < 0 &&
            wifi_ui_slider_rect(part_rect[0])) {
            part_cnt = 1;
            part_on = true;
        }
        {   // diagnostics: why frames are being redrawn (counts per 5 s, in the status line)
            if (lib_dirty) why_lib++;
            if (music_dirty) why_music++;
            if (pg_off != 0 || pg_drag) why_slide++;
            if (ang != last_ang) why_rot++;
        }
        prev_down = down;
        force_redraw = false;
        quick_frame = pet_dirty;
        if (content || ang != last_ang) {
            int64_t r0 = esp_timer_get_time();
            render_frame(content);
            if (part_on) { part_us += esp_timer_get_time() - r0; part_n++; }
            else render_us += esp_timer_get_time() - r0;
            if (others) last_full = now;
            last_ang = ang;
            frames++;
        }

        if (now - log_t > 5000000) {
            ESP_LOGI(TAG, "page %d: redraws lib %d music %d slide %d rot %d, pg_off %d", page, why_lib, why_music, why_slide, why_rot, (int)pg_off);
            why_lib = why_music = why_slide = why_rot = 0;
            ESP_LOGI(TAG, "%s %d frames/s, angle %d, battery %.2f V (%d%%%s, curve from %d runs%s), free internal %u, calendar: %s", sheet >= H ? "wifi" : "calendar",
                     frames / 5, (int)ang, battery_volts(), battery_percent(), battery_charging() ? ", charging" : "", battery_learned_runs(), battery_logging() ? ", logging" : "", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     calendar_loaded() ? "loaded" : calendar_error());
            if (frames > part_n) ESP_LOGI(TAG, "render %d ms/full frame, %d eyes-only frames at %d ms (all frames: draw %d ms, turn+send %d ms)",
                                          (int)(render_us / (frames - part_n) / 1000), part_n, part_n ? (int)(part_us / part_n / 1000) : 0,
                                          (int)(prof_draw_us / frames / 1000), (int)(prof_turn_us / frames / 1000));
            prof_draw_us = prof_turn_us = 0;
            render_us = part_us = 0;
            part_n = 0;
            frames = 0;
            log_t = now;
        }
        if (!down && !pg_drag && pg_off == 0 && sheet <= 0 && target < 0 && power == PW_AWAKE && !calibrating && !tilt_step &&
            now - last_use > 400000)
            refresh_neighbour_snapshots(now);          // pre-draw the pages either side for instant swipes
        bool moving = pg_drag || pg_off != 0 || card_drag || target >= 0 || cal_ui_animating() || ang != ang_target ||
                      (page == PAGE_LIB && library_ui_animating());
        int64_t budget = moving ? FRAME_US_MOVING : FRAME_US, spent = esp_timer_get_time() - now;
        vTaskDelay(spent < budget ? pdMS_TO_TICKS((budget - spent) / 1000 + 1) : 1);   // always let others run a little
    }
}
