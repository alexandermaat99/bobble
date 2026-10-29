#include <string.h>
#include <math.h>
#include "audio.h"
#include "board_square.h"

// Loudness of a stretch of samples as 0..1 (about -55 dBFS to -10 dBFS), eased so it rises quickly and falls slowly
static void level_update(volatile float *lvl, const int16_t *pcm, int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)pcm[i] * pcm[i];
    float rms = n ? sqrtf((float)(sum / n)) : 0;
    float v = rms > 1 ? (20 * log10f(rms / 32768) + 55) / 45 : 0;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    *lvl = v > *lvl ? *lvl * 0.4f + v * 0.6f : *lvl * 0.85f + v * 0.15f;
}

#if SQUARE_HW
// ---------------------------------------------------------------- the square board: ES8311 (speaker), ES7210 (mics)
// One I2S port, both ways: out in standard stereo to the ES8311, in as 4-slot TDM from the ES7210, whose slots come
// as MIC1, MIC3, MIC2, MIC4. MIC1 is a microphone; MIC3 is the ES8311's own output fed back (Waveshare's "AEC" network
// on the schematic), so the reference lines up with what the mic hears without any timing guesswork.
// (The setup follows xiaozhi-esp32's BoxAudioCodec, which runs on this board.)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_aec.h"

static const char *TAG = "audio";

#define PIN_MCLK 5
#define PIN_BCLK 16
#define PIN_WS   7
#define PIN_DOUT 6                      // to the ES8311
#define PIN_DIN  15                     // from the ES7210
#define ES8311_ADDR 0x30                // (8-bit forms of 0x18 and 0x40, as esp_codec_dev takes them)
#define ES7210_ADDR 0x80
#define MIC_GAIN_DB 30.0f
#define VOLUME 80                       // 0..100
#define RING_SAMPLES (AUDIO_RATE * 15)  // up to 15 s of reply waiting to be played

static esp_codec_dev_handle_t in_dev, out_dev;
static aec_handle_t *aec;
static int chunk;                        // samples per AEC frame
static int16_t *raw, *mic, *ref, *clean, *play;
static TaskHandle_t task;
static SemaphoreHandle_t wake, opened, ring_lock;
static volatile bool want, running, speaker_mute;
static volatile bool open_ok;
static audio_mic_cb mic_cb;
static volatile float mic_lvl, out_lvl;
static int16_t *ring;
static volatile int ring_r, ring_n;      // read position, how many queued

static bool hw_init(void)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = {.id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 240,
                            .auto_clear_after_cb = true};
    if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = AUDIO_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .ws_pol = false, .bit_shift = true, .left_align = true},
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
    };
    i2s_tdm_config_t tdm = {
        .clk_cfg = {.sample_rate_hz = AUDIO_RATE, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                    .bclk_div = 8},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                     .ws_width = I2S_TDM_AUTO_WS_WIDTH, .ws_pol = false, .bit_shift = true, .left_align = false,
                     .total_slot = I2S_TDM_AUTO_SLOT_NUM},
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_DIN},
    };
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_init_tdm_mode(rx, &tdm) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t ic = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c = {.port = I2C_NUM_0, .addr = ES8311_ADDR, .bus_handle = sq_i2c()};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    i2c.addr = ES7210_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    if (!data_if || !gpio_if || !out_ctrl || !in_ctrl) return false;

    es8311_codec_cfg_t oc = {.ctrl_if = out_ctrl, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                             .pa_pin = -1, .use_mclk = true,
                             .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t *out_codec = es8311_codec_new(&oc);
    es7210_codec_cfg_t ec = {.ctrl_if = in_ctrl,
                             .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4};
    const audio_codec_if_t *in_codec = es7210_codec_new(&ec);
    if (!out_codec || !in_codec) return false;
    esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    out_dev = esp_codec_dev_new(&dc);
    dc.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dc.codec_if = in_codec;
    in_dev = esp_codec_dev_new(&dc);
    if (!out_dev || !in_dev) return false;

    aec_config_t ac = {.mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = 4, .sample_rate = AUDIO_RATE,
                       .caps = MALLOC_CAP_SPIRAM, .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR};
    aec = aec_create_from_config(&ac);
    if (!aec) return false;
    chunk = aec_get_chunksize(aec);
    raw = heap_caps_aligned_alloc(16, chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    mic = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ref = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    clean = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    play = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!raw || !mic || !ref || !clean || !play || !ring) return false;
    ESP_LOGI(TAG, "ready: %s, %d-sample frames", aec_get_config_string(aec), chunk);
    return true;
}

static bool devs_open(void)
{
    esp_codec_dev_sample_info_t out = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
    esp_codec_dev_sample_info_t in = {.bits_per_sample = 16, .channel = 4, .sample_rate = AUDIO_RATE,
                                      .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)};
    if (esp_codec_dev_open(out_dev, &out) != ESP_CODEC_DEV_OK) return false;
    if (esp_codec_dev_open(in_dev, &in) != ESP_CODEC_DEV_OK) { esp_codec_dev_close(out_dev); return false; }
    esp_codec_dev_set_out_vol(out_dev, VOLUME);
    esp_codec_dev_set_in_channel_gain(in_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), MIC_GAIN_DB);
    return true;
}

// The next frame of reply for the speaker (silence when there's none, or the speaker's muted)
static void ring_take(int16_t *out, int n)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    int k = ring_n < n ? ring_n : n;
    for (int i = 0; i < k; i++) out[i] = ring[(ring_r + i) % RING_SAMPLES];
    ring_r = (ring_r + k) % RING_SAMPLES;
    ring_n -= k;
    xSemaphoreGive(ring_lock);
    if (k < n) memset(out + k, 0, (n - k) * sizeof(int16_t));
    if (speaker_mute) memset(out, 0, n * sizeof(int16_t));
}

static void audio_task(void *arg)
{
    for (;;) {
        if (want != running) {
            if (want) {
                open_ok = devs_open();
                running = open_ok;
                if (running) sq_amp(true);
                else want = false;
            } else {
                sq_amp(false);
                esp_codec_dev_close(in_dev);
                esp_codec_dev_close(out_dev);
                running = false;
                mic_lvl = out_lvl = 0;
            }
            xSemaphoreGive(opened);
        }
        if (!running) { xSemaphoreTake(wake, portMAX_DELAY); continue; }
        if (esp_codec_dev_read(in_dev, raw, chunk * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) { vTaskDelay(1); continue; }
        for (int i = 0; i < chunk; i++) { mic[i] = raw[2 * i]; ref[i] = raw[2 * i + 1]; }
        aec_process(aec, mic, ref, clean);
        level_update(&mic_lvl, clean, chunk);
        if (mic_cb) mic_cb(clean, chunk);
        ring_take(play, chunk);
        level_update(&out_lvl, play, chunk);
        esp_codec_dev_write(out_dev, play, chunk * sizeof(int16_t));
    }
}

bool audio_available(void) { return true; }

bool audio_start(audio_mic_cb cb)
{
    if (!task) {
        if (!hw_init()) { ESP_LOGE(TAG, "the speaker and microphones didn't come up"); return false; }
        wake = xSemaphoreCreateBinary();
        opened = xSemaphoreCreateBinary();
        ring_lock = xSemaphoreCreateMutex();
        // (above the drawing, which uses both cores in bursts: a late frame here is a click in the speaker)
        xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 9, &task, 1);
    }
    mic_cb = cb;
    if (running) return true;
    audio_flush();
    xSemaphoreTake(opened, 0);
    want = true;
    xSemaphoreGive(wake);
    xSemaphoreTake(opened, pdMS_TO_TICKS(1500));
    if (!open_ok) ESP_LOGE(TAG, "couldn't open the codecs");
    return open_ok;
}

void audio_stop(void)
{
    if (!task || !running) return;
    xSemaphoreTake(opened, 0);
    want = false;
    xSemaphoreTake(opened, pdMS_TO_TICKS(500));
    audio_flush();
}

void audio_play(const int16_t *pcm, int n)
{
    if (!ring) return;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    int room = RING_SAMPLES - ring_n;
    if (n > room) n = room;                           // (the Mac keeps well ahead of this: nothing to lose normally)
    int w = (ring_r + ring_n) % RING_SAMPLES;
    for (int i = 0; i < n; i++) ring[(w + i) % RING_SAMPLES] = pcm[i];
    ring_n += n;
    xSemaphoreGive(ring_lock);
}

void audio_flush(void)
{
    if (!ring_lock) return;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    ring_n = 0;
    xSemaphoreGive(ring_lock);
}

int audio_queued_ms(void) { return ring_n * 1000 / AUDIO_RATE; }
void audio_set_speaker_mute(bool m) { speaker_mute = m; }
float audio_mic_level(void) { return mic_lvl; }
float audio_out_level(void) { return out_lvl; }

#elif defined(BOBBLE_SIM) && defined(BOARD_SQUARE)
// ---------------------------------------------------------------- the Square simulator: the page works, silently
// (no sound of its own: the board's speaker and microphones are the only ones a chat uses, never the Mac's)
static int64_t queued_until;                     // when what it's been "sent" would have finished playing
#include "esp_timer.h"
bool audio_available(void) { return true; }
bool audio_start(audio_mic_cb cb) { (void)cb; return true; }
void audio_stop(void) { queued_until = 0; }
void audio_play(const int16_t *pcm, int n)
{
    (void)pcm;
    int64_t now = esp_timer_get_time();
    queued_until = (queued_until > now ? queued_until : now) + (int64_t)n * 1000000 / AUDIO_RATE;
}
void audio_flush(void) { queued_until = 0; }
int audio_queued_ms(void) { int64_t d = queued_until - esp_timer_get_time(); return d > 0 ? (int)(d / 1000) : 0; }
void audio_set_speaker_mute(bool m) { (void)m; }
float audio_mic_level(void) { return 0; }
float audio_out_level(void) { return audio_queued_ms() > 0 ? 0.5f : 0; }
static inline void level_unused(void) { (void)level_update; }

#else
// ---------------------------------------------------------------- the round board: no speaker or microphones
bool audio_available(void) { return false; }
bool audio_start(audio_mic_cb cb) { (void)cb; return false; }
void audio_stop(void) {}
void audio_play(const int16_t *pcm, int n) { (void)pcm; (void)n; }
void audio_flush(void) {}
int audio_queued_ms(void) { return 0; }
void audio_set_speaker_mute(bool m) { (void)m; }
float audio_mic_level(void) { return 0; }
float audio_out_level(void) { return 0; }
static inline void level_unused(void) { (void)level_update; }
#endif
