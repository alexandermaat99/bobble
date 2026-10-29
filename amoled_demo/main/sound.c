#include "sound.h"
#include "board_square.h"

#if SQUARE_HW
// ---------------------------------------------------------------- the square board: ES8311 codec, amplifier on EXIO3
// I2S out only, standard stereo to the ES8311 (Waveshare's pins: MCLK 5, BCLK 16, WS 7, data 6). The codec and the
// amplifier come on for a sound and go off again once it's been quiet for a few seconds (no hiss, no power wasted).
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "screen.h"

static const char *TAG = "sound";

#define PIN_MCLK 5
#define PIN_BCLK 16
#define PIN_WS   7
#define PIN_DOUT 6
#define RATE 16000
#define CLICK_MS 60
#define IDLE_MS 3000                    // quiet this long: codec and amplifier off

static esp_codec_dev_handle_t out_dev;
static TaskHandle_t task;
static int16_t click[RATE * CLICK_MS / 1000], quiet[RATE * 30 / 1000];

static bool hw_init(void)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear_after_cb = true;                    // (silence, not the last samples again, once a sound ends)
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t ic = {.port = I2S_NUM_0, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    audio_codec_i2c_cfg_t i2c = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = sq_i2c()};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl || !gpio_if) return false;
    es8311_codec_cfg_t oc = {.ctrl_if = ctrl, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                             .pa_pin = -1, .use_mclk = true,   // (the amplifier is on the expander: sq_amp)
                             .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t *codec = es8311_codec_new(&oc);
    if (!codec) return false;
    esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if};
    return (out_dev = esp_codec_dev_new(&dc)) != NULL;
}

// The tick: a soft "tock" that slides from 1.4 kHz down to 900 Hz and dies away in a few tens of ms
static void make_click(void)
{
    int n = sizeof click / sizeof click[0];
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / RATE, f = 900 + 500 * expf(-t / 0.012f);
        float env = (t < 0.002f ? t / 0.002f : 1) * expf(-t / 0.014f);
        ph += 2 * (float)M_PI * f / RATE;
        click[i] = (int16_t)(14000 * env * sinf(ph));
    }
}

static void sound_task(void *arg)
{
    bool open = false;
    for (;;) {
        if (!ulTaskNotifyTake(pdTRUE, open ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY)) {   // been quiet: all off
            sq_amp(false);
            esp_codec_dev_close(out_dev);
            open = false;
            continue;
        }
        int vol = device_volume_get();
        if (vol <= 0) continue;
        if (!open) {
            esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = RATE};
            if (esp_codec_dev_open(out_dev, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "couldn't open the codec"); continue; }
            open = true;
            sq_amp(true);
            esp_codec_dev_write(out_dev, quiet, sizeof quiet);   // (a moment for the amplifier to wake)
        }
        esp_codec_dev_set_out_vol(out_dev, vol);
        esp_codec_dev_write(out_dev, click, sizeof click);
        esp_codec_dev_write(out_dev, quiet, sizeof quiet);   // (so the tick's tail isn't cut off by the next close)
    }
}

void sound_click(void)
{
    if (!task) {
        static bool failed;
        if (failed) return;
        if (!hw_init()) { failed = true; ESP_LOGE(TAG, "the speaker didn't come up"); return; }
        make_click();
        xTaskCreatePinnedToCore(sound_task, "sound", 3072, NULL, 5, &task, 1);
    }
    xTaskNotifyGive(task);
}

#else
void sound_click(void) {}
#endif
