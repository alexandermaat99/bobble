#include "board_square.h"
#if SQUARE_HW
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"

static const char *TAG = "square";

#define PIN_SDA   47
#define PIN_SCL   48
#define PIN_BL    4                     // backlight PWM: high is dark
#define BL_CH     LEDC_CHANNEL_1

// TCA9554 expander lines
#define EX_CS     (1 << 0)              // panel's 3-wire SPI
#define EX_SDA    (1 << 1)
#define EX_SCL    (1 << 2)
#define EX_AMP    (1 << 3)              // speaker amplifier (on only while something's playing: audio.c)
#define EX_RST    (1 << 5)              // panel and touch reset
#define EX_TINT   (1 << 6)              // touch interrupt: held low through reset to pick the touch chip's address

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t ex_dev, gt_dev, pmu_dev;
static uint8_t ex_out;
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;
static async_memcpy_handle_t mcp;

i2c_master_bus_handle_t sq_i2c(void)
{
    if (bus) return bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    return bus;
}

static i2c_master_dev_handle_t add_dev(uint8_t addr)
{
    i2c_master_dev_handle_t d = NULL;
    i2c_device_config_t c = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    if (i2c_master_probe(sq_i2c(), addr, 50) != ESP_OK || i2c_master_bus_add_device(sq_i2c(), &c, &d) != ESP_OK) return NULL;
    return d;
}

static void ex_reg(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    i2c_master_transmit(ex_dev, b, 2, 50);
}
static void ex_set(uint8_t bits, bool on)
{
    ex_out = on ? ex_out | bits : ex_out & ~bits;
    ex_reg(0x01, ex_out);
}

// ---------------------------------------------------------------- panel setup (3-wire SPI through the expander)
// Each word is 9 bits: 0 for a command or 1 for a parameter, then the byte, MSB first, read on SCL's rising edge
static void spi9(bool param, uint8_t v)
{
    uint16_t w = (param ? 0x100 : 0) | v;
    for (int i = 8; i >= 0; i--) {
        ex_out = (w >> i) & 1 ? ex_out | EX_SDA : ex_out & ~EX_SDA;
        ex_set(EX_SCL, false);
        ex_set(EX_SCL, true);
    }
}
static void panel_cmd(uint8_t cmd, const uint8_t *data, int n)
{
    ex_set(EX_SCL, false);
    ex_set(EX_CS, false);
    spi9(false, cmd);
    for (int i = 0; i < n; i++) spi9(true, data[i]);
    ex_set(EX_SCL, false);
    ex_set(EX_CS, true);
}

typedef struct { uint8_t cmd; uint8_t data[16]; uint8_t n; uint8_t delay_ms; } init_cmd_t;
static const init_cmd_t st7701_init[] = {       // (Waveshare's sequence for this panel)
    {0x11, {0}, 0, 120},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, {0x3B, 0x00}, 2, 0},
    {0xC1, {0x0D, 0x02}, 2, 0},
    {0xC2, {0x21, 0x08}, 2, 0},
    {0xCD, {0x08}, 1, 0},
    {0xB0, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x60}, 1, 0},
    {0xB1, {0x30}, 1, 0},
    {0xB2, {0x87}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x49}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x21}, 1, 0},
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 20},
    {0xE0, {0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, {0x44, 0x44}, 2, 0},
    {0xE5, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, {0x44, 0x44}, 2, 0},
    {0xE8, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, {0x3C, 0x00}, 2, 0},
    {0xED, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x36, {0x00}, 1, 0},
    {0x3A, {0x66}, 1, 0},
    {0x21, {0}, 0, 120},
    {0x29, {0}, 0, 0},
};

static void backlight_init(void)
{
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                             .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {.gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_CH,
                               .timer_sel = LEDC_TIMER_1, .duty = 0};   // full on: the firmware starts at full brightness
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    gpio_sleep_sel_dis(PIN_BL);                         // keep driving it through light sleep (floating would light it)
}

void sq_backlight(int level)
{
    level = level < 0 ? 0 : (level > 255 ? 255 : level);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CH, 1023 - level * 1023 / 255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CH);
}

void sq_display_init(void)
{
    backlight_init();
    ex_dev = add_dev(0x20);
    if (!ex_dev) ESP_LOGE(TAG, "no I/O expander: the panel can't be set up");
    ex_out = EX_CS | EX_RST;
    ex_reg(0x01, ex_out);
    ex_reg(0x03, (uint8_t)~(EX_CS | EX_SDA | EX_SCL | EX_AMP | EX_RST | EX_TINT));   // those are outputs
    vTaskDelay(pdMS_TO_TICKS(20));
    ex_set(EX_RST, false);                              // reset the panel and touch (touch INT held low)
    vTaskDelay(pdMS_TO_TICKS(20));
    ex_set(EX_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    ex_reg(0x03, (uint8_t)~(EX_CS | EX_SDA | EX_SCL | EX_AMP | EX_RST));   // let go of touch INT
    panel_cmd(0x01, NULL, 0);                           // software reset
    vTaskDelay(pdMS_TO_TICKS(120));
    for (int i = 0; i < sizeof st7701_init / sizeof *st7701_init; i++) {
        panel_cmd(st7701_init[i].cmd, st7701_init[i].data, st7701_init[i].n);
        if (st7701_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(st7701_init[i].delay_ms));
    }

    esp_lcd_rgb_panel_config_t rc = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000, .h_res = 480, .v_res = 480,
            .hsync_pulse_width = 10, .hsync_back_porch = 10, .hsync_front_porch = 20,
            .vsync_pulse_width = 10, .vsync_back_porch = 10, .vsync_front_porch = 10,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 480 * 16,              // scanned out from internal RAM (straight from PSRAM it glitches;
                                                        // 32 rows each made the picture shake: each refill too long)
        .psram_trans_align = 64,
        .hsync_gpio_num = 46, .vsync_gpio_num = 3, .de_gpio_num = 17, .pclk_gpio_num = 9, .disp_gpio_num = -1,
        // The drawing code makes big-endian pixels (what the round board's SPI panel takes): naming the data lines
        // high byte first turns them round on the way out, for nothing (the panel's B0..B4 G0..G5 R0..R4 are
        // 40 41 42 2 1 / 21 8 18 45 38 39 / 10 11 12 13 14)
        .data_gpio_nums = {45, 38, 39, 10, 11, 12, 13, 14, 40, 41, 42, 2, 1, 21, 8, 18},
        .flags.fb_in_psram = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    void *f;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &f));
    fb = f;
    memset(fb, 0, 480 * 480 * 2);
    esp_cache_msync(fb, 480 * 480 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // (no dirty lines left to land on DMA's work)
    async_memcpy_config_t mc = {.backlog = 4, .dma_burst_size = 32};   // (rows are 960 bytes: whole bursts)
    if (esp_async_memcpy_install_gdma_ahb(&mc, &mcp) != ESP_OK) { mcp = NULL; ESP_LOGW(TAG, "no DMA copy: the CPU copies"); }
}

void sq_amp(bool on)
{
    if (ex_dev) ex_set(EX_AMP, on);
}

// DMA copies each strip into the frame in PSRAM while the next one is drawn (copying with the CPU stalled on every
// PSRAM write and took over half of each frame). The bounce buffers are refilled through the cache, so once a copy
// lands, whatever the cache still holds of those rows is dropped.
typedef struct { uint16_t *dst; int bytes; SemaphoreHandle_t done; } copy_t;
static copy_t copies[4];                                // (the DMA's backlog: one per strip in flight)
static int copy_next;

static bool copied(async_memcpy_handle_t m, async_memcpy_event_t *e, void *arg)
{
    copy_t *c = arg;
    esp_cache_msync(c->dst, c->bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(c->done, &hp);
    return hp == pdTRUE;
}

void sq_copy(uint16_t *dst, uint16_t *px, int bytes, SemaphoreHandle_t done)
{
    copy_t *c = &copies[copy_next];
    copy_next = (copy_next + 1) % 4;
    *c = (copy_t){dst, bytes, done};
    if (mcp && esp_async_memcpy(mcp, dst, px, bytes, copied, c) == ESP_OK) return;
    memcpy(dst, px, bytes);
    esp_cache_msync(dst, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    xSemaphoreGive(done);
}

void sq_draw(int x0, int y0, int x1, int y1, uint16_t *px, SemaphoreHandle_t done)
{
    int w = x1 - x0;
    if (mcp && w == 480) { sq_copy(fb + y0 * 480, px, w * (y1 - y0) * 2, done); return; }
    for (int y = y0; y < y1; y++, px += w) memcpy(fb + y * 480 + x0, px, w * 2);   // (part of a row: by hand)
    esp_cache_msync(fb + y0 * 480, (y1 - y0) * 480 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    xSemaphoreGive(done);
}

// ---------------------------------------------------------------- touch (GT911)
static bool gt_read(uint16_t reg, uint8_t *d, int n)
{
    uint8_t a[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(gt_dev, a, 2, d, n, 20) == ESP_OK;
}

// The chip flags each new report; between reports the finger is where it last was
static bool gt_poll(int *x, int *y)
{
    static bool down;
    static int lx, ly;
    uint8_t s;
    *x = lx; *y = ly;
    if (!gt_dev || !gt_read(0x814E, &s, 1)) return down;
    if (s & 0x80) {
        uint8_t p[4];
        down = (s & 0x0F) && gt_read(0x8150, p, 4);
        if (down) { lx = p[0] | (p[1] << 8); ly = p[2] | (p[3] << 8); }
        uint8_t clr[3] = {0x81, 0x4E, 0};
        i2c_master_transmit(gt_dev, clr, 3, 20);
    }
    *x = lx; *y = ly;
    return down;
}

// A task reads the chip every 10 ms (every 100 ms while the screen sleeps), so a quick tap that starts and ends
// between two frames isn't lost: the main loop read it once a frame, 30-50 ms apart with the settings open, and the
// chip keeps only its latest report. A press that came and went is handed over once, then the let-go.
static portMUX_TYPE t_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool t_down, t_pressed, t_fast = true;   // t_pressed: went down since the last sq_touch_read
static volatile bool t_paused;                          // not reading the chip (sq_touch_sleep)
static volatile int t_x, t_y, p_x, p_y;

static void touch_task(void *arg)
{
    int ups = 0;                                       // reports in a row with no finger
    for (;;) {
        if (t_paused) { t_down = t_pressed = false; vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int x, y;
        bool d = gt_poll(&x, &y);
        ups = d ? 0 : ups + 1;
        taskENTER_CRITICAL(&t_mux);
        if (d) {
            if (!t_down) { t_pressed = true; p_x = x; p_y = y; }
            t_x = x; t_y = y;
            t_down = true;
        } else if (ups >= 3) t_down = false;           // lifted only after 30 ms of none: the chip drops a finger for a
        taskEXIT_CRITICAL(&t_mux);                      // report now and then (near the edges most), which ended swipes
        vTaskDelay(pdMS_TO_TICKS(t_fast ? 10 : 100));
    }
}

void sq_touch_init(void)
{
    gt_dev = add_dev(0x5D);
    if (!gt_dev) gt_dev = add_dev(0x14);
    if (!gt_dev) { ESP_LOGE(TAG, "no touch chip found"); return; }
    xTaskCreatePinnedToCoreWithCaps(touch_task, "touch", 3072, NULL, 7, NULL, 0, MALLOC_CAP_SPIRAM);
}

void sq_touch_fast(bool on) { t_fast = on; }

// While the PWR button has it off, the touch chip isn't read, so nothing in a bag can wake the board. (Its own sleep
// mode was tried: woken by a pulse on its interrupt line it didn't answer again on this board, and the only other way
// back is the reset line it shares with the panel.)
void sq_touch_sleep(bool on)
{
    t_paused = on;
    ESP_LOGI(TAG, "touch %s", on ? "ignored" : "back");
}

bool sq_touch_read(int *x, int *y)
{
    taskENTER_CRITICAL(&t_mux);
    bool d = t_down || t_pressed;
    if (t_down) { *x = t_x; *y = t_y; }
    else if (t_pressed) { *x = p_x; *y = p_y; }       // (a tap that's already over: down here this once)
    t_pressed = false;
    taskEXIT_CRITICAL(&t_mux);
    return d;
}

// ---------------------------------------------------------------- battery (AXP2101)
// Charge current: 500 mA fills a 2000 mAh cell in ~4.5 h, and is gentler still on a bigger one. (The charger is linear:
// what the cell doesn't take is heat in the chip. 1000 mA took it to 61 C even near full, ~0.7 W; from empty it's twice
// that.) The chip's codes are 25 mA steps to 200 mA (8), then 100 mA steps to 1000 mA (16); its own default is 200 mA.
#define CHARGE_MA 500
#define CHARGE_CODE (CHARGE_MA <= 200 ? CHARGE_MA / 25 : CHARGE_MA / 100 + 6)

static bool pmu_reg(uint8_t reg, uint8_t *v)
{
    if (!pmu_dev) {
        static bool tried;
        if (tried) return false;
        tried = true;
        if (!(pmu_dev = add_dev(0x34))) { ESP_LOGE(TAG, "no power chip found"); return false; }
        uint8_t on[2] = {0x30, 0};                      // ADC channels: battery, USB and system voltage, chip temperature
        if (pmu_reg(0x30, &on[1])) { on[1] |= 0x1D; i2c_master_transmit(pmu_dev, on, 2, 50); }
        uint8_t icc[2] = {0x62, 0};                     // charge current (bits 4-0)
        if (pmu_reg(0x62, &icc[1])) { icc[1] = (icc[1] & 0xE0) | CHARGE_CODE; i2c_master_transmit(pmu_dev, icc, 2, 50); }
    }
    return i2c_master_transmit_receive(pmu_dev, &reg, 1, v, 1, 20) == ESP_OK;
}

float sq_battery_volts(void)
{
    uint8_t st, hi, lo;
    if (!pmu_reg(0x00, &st) || !(st & 0x08)) return 0;   // status 1, bit 3: a battery is connected
    if (!pmu_reg(0x34, &hi) || !pmu_reg(0x35, &lo)) return 0;
    return (((hi & 0x3F) << 8) | lo) / 1000.0f;
}

// The PWR button: the power chip notes each press (interrupt status 2: bit 3 short press, bit 2 long press, bits 1-0
// its edges); read and clear them. 1: a short press since last time, 2: a long one (held ~1.5 s; ~6 s cuts the power)
int sq_power_key(void)
{
    uint8_t st;
    if (!pmu_reg(0x49, &st) || !(st & 0x0F)) return 0;
    uint8_t clr[2] = {0x49, st & 0x0F};               // (write 1 to clear)
    i2c_master_transmit(pmu_dev, clr, 2, 20);
    return st & 0x08 ? 1 : st & 0x04 ? 2 : 0;
}

bool sq_plugged_in(void)
{
    uint8_t st;
    return pmu_reg(0x00, &st) && (st & 0x20);           // status 1, bit 5: USB power is good
}

static int pmu_adc(uint8_t reg)                         // a 14-bit reading in two registers (high 6 bits first)
{
    uint8_t hi, lo;
    return pmu_reg(reg, &hi) && pmu_reg(reg + 1, &lo) ? ((hi & 0x3F) << 8) | lo : -1;
}

void sq_power_read(battery_diag_t *d)
{
    memset(d, 0, sizeof *d);
    d->vbat_mv = d->vbus_mv = d->vsys_mv = d->gauge_pct = d->charge_ma = d->full_mv = d->in_limit_ma = -1;
    d->dir = d->stage = -1;
    uint8_t st1, st2, v;
    if (!pmu_reg(0x00, &st1) || !pmu_reg(0x01, &st2)) return;
    d->chip = true;
    d->st1 = st1;
    d->st2 = st2;
    d->usb = st1 & 0x20;                                // status 1: bit 5 USB power good, bit 3 battery connected
    d->battery = st1 & 0x08;
    d->dir = (st2 >> 5) & 3;                            // status 2: bits 6-5 current direction, 2-0 charge stage
    d->stage = st2 & 7;
    if (pmu_reg(0x18, &v)) d->charge_on = v & 0x02;     // charger / fuel gauge control: bit 1 charging enabled
    if (d->battery) d->vbat_mv = pmu_adc(0x34);
    if (d->usb) d->vbus_mv = pmu_adc(0x38);
    d->vsys_mv = pmu_adc(0x3A);
    int t = pmu_adc(0x3C);
    if (t >= 0) d->chip_c = 22 + (7274 - t) / 20.0f;
    if (d->battery && pmu_reg(0xA4, &v) && v <= 100) d->gauge_pct = v;
    if (pmu_reg(0x62, &v)) { v &= 0x1F; d->charge_ma = v <= 8 ? v * 25 : v <= 16 ? (v - 6) * 100 : -1; }
    static const int full[8] = {-1, 4000, 4100, 4200, 4350, 4400, -1, -1};
    static const int limit[8] = {100, 500, 900, 1000, 1500, 2000, -1, -1};
    if (pmu_reg(0x64, &v)) d->full_mv = full[v & 7];
    if (pmu_reg(0x16, &v)) d->in_limit_ma = limit[v & 7];
    pmu_reg(0x50, &d->ts);
    for (int i = 0; i < 3; i++) pmu_reg(0x48 + i, &d->irq[i]);
}
#endif
