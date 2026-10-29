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
#include "screen.h"

static const char *TAG = "audio";

#define PIN_MCLK 5
#define PIN_BCLK 16
#define PIN_WS   7
#define PIN_DOUT 6                      // to the ES8311
#define PIN_DIN  15                     // from the ES7210
#define ES8311_ADDR 0x30                // (8-bit forms of 0x18 and 0x40, as esp_codec_dev takes them)
#define ES7210_ADDR 0x80
#define MIC_GAIN_DB 30.0f
#define IDLE_MS 3000                    // a tick with no chat open: the speaker goes off again after this long quiet
#define WAKE_MS 210                     // ...and on again: this long for the codec and amplifier to come up before it

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
static int ring_cap;                     // samples it holds: up to 15 s of reply waiting to be played (less if PSRAM is short)
static volatile int ring_r, ring_n;      // read position, how many queued
static int16_t click[AUDIO_RATE * 60 / 1000], quiet[AUDIO_RATE * 30 / 1000];
static volatile bool click_wanted;       // audio_click() asked for one...
static int click_at = -1;                // ...and how far into it a chat's frames have got (-1: not playing)
static bool out_only;                    // just the speaker is open, for ticks (no chat)
static int vol_set = -1;                 // the volume the codec was last given

// The tick: a soft "tock" that slides from 1.4 kHz down to 900 Hz and dies away in a few tens of ms
static void make_click(void)
{
    int n = sizeof click / sizeof click[0];
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / AUDIO_RATE, f = 900 + 500 * expf(-t / 0.012f);
        float env = (t < 0.002f ? t / 0.002f : 1) * expf(-t / 0.014f);
        ph += 2 * (float)M_PI * f / AUDIO_RATE;
        click[i] = (int16_t)(14000 * env * sinf(ph));
    }
}

// Give the codec the volume picked in Settings, if it's changed (0: silent)
static void follow_volume(void)
{
    int v = device_volume_get();
    if (v != vol_set) esp_codec_dev_set_out_vol(out_dev, v);
    vol_set = v;
}

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
    raw = heap_caps_aligned_alloc(16, chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    mic = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ref = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    clean = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    play = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    static const int secs[] = {15, 8, 4};          // (PSRAM is mostly frames: take what fits rather than no speech at all)
    for (int i = 0; i < 3 && !ring; i++) {
        ring_cap = AUDIO_RATE * secs[i];
        ring = heap_caps_malloc(ring_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (!ring) ring_cap = 0;
    else if (ring_cap < AUDIO_RATE * 15) ESP_LOGW(TAG, "room for %d s of reply only", ring_cap / AUDIO_RATE);
    if (!raw || !mic || !ref || !clean || !play) return false;
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
    vol_set = -1;
    follow_volume();
    esp_codec_dev_set_in_channel_gain(in_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), MIC_GAIN_DB);
    return true;
}

// The next frame of reply for the speaker (silence when there's none, or the speaker's muted)
static void ring_take(int16_t *out, int n)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    int k = ring_n < n ? ring_n : n;
    for (int i = 0; i < k; i++) out[i] = ring[(ring_r + i) % ring_cap];
    if (k) ring_r = (ring_r + k) % ring_cap;          // (no ring at all: ring_cap 0, nothing taken)
    ring_n -= k;
    xSemaphoreGive(ring_lock);
    if (k < n) memset(out + k, 0, (n - k) * sizeof(int16_t));
    if (speaker_mute) memset(out, 0, n * sizeof(int16_t));
}

// A tick while no chat is open: just the speaker, on for it and off again once it's been quiet a while
static void tick_alone(void)
{
    click_wanted = false;
    if (device_volume_get() <= 0) return;
    if (!out_only) {
        esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
        if (esp_codec_dev_open(out_dev, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "couldn't open the speaker"); return; }
        out_only = true;
        vol_set = -1;
        follow_volume();
        sq_amp(true);
        for (int i = 0; i < WAKE_MS / 30; i++) esp_codec_dev_write(out_dev, quiet, sizeof quiet);   // (30 ms each)
    }
    follow_volume();
    esp_codec_dev_write(out_dev, click, sizeof click);
    esp_codec_dev_write(out_dev, quiet, sizeof quiet);       // (so the tick's tail isn't cut off when it closes)
}

static void audio_task(void *arg)
{
    for (;;) {
        if (want != running) {
            if (want) {
                if (out_only) { esp_codec_dev_close(out_dev); out_only = false; }   // (a chat opens both)
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
        if (!running) {
            if (click_wanted) { tick_alone(); continue; }
            if (xSemaphoreTake(wake, out_only ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY) != pdTRUE && out_only && !click_wanted) {
                sq_amp(false);                        // been quiet: the speaker off again
                esp_codec_dev_close(out_dev);
                out_only = false;
            }
            continue;
        }
        follow_volume();
        if (esp_codec_dev_read(in_dev, raw, chunk * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) { vTaskDelay(1); continue; }
        for (int i = 0; i < chunk; i++) { mic[i] = raw[2 * i]; ref[i] = raw[2 * i + 1]; }
        aec_process(aec, mic, ref, clean);
        level_update(&mic_lvl, clean, chunk);
        if (mic_cb) mic_cb(clean, chunk);
        ring_take(play, chunk);
        level_update(&out_lvl, play, chunk);
        if (click_wanted) { click_wanted = false; click_at = device_volume_get() > 0 ? 0 : -1; }
        for (int i = 0, n = sizeof click / sizeof click[0]; click_at >= 0 && i < chunk; i++, click_at++) {   // a tick over it
            if (click_at >= n) { click_at = -1; break; }
            int v = play[i] + click[click_at];
            play[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        }
        esp_codec_dev_write(out_dev, play, chunk * sizeof(int16_t));
    }
}

bool audio_available(void) { return true; }

static bool task_up(void)
{
    static bool failed;
    if (task) return true;
    if (failed) return false;
    if (!hw_init()) { failed = true; ESP_LOGE(TAG, "the speaker and microphones didn't come up"); return false; }
    make_click();
    wake = xSemaphoreCreateBinary();
    opened = xSemaphoreCreateBinary();
    ring_lock = xSemaphoreCreateMutex();
    // (above the drawing, which uses both cores in bursts: a late frame here is a click in the speaker; its stack in
    // PSRAM, as the square runs its code from there anyway: internal RAM is short)
    xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", 6144, NULL, 9, &task, 1, MALLOC_CAP_SPIRAM);
    return true;
}

void audio_click(void)
{
    if (!task_up()) return;
    click_wanted = true;
    xSemaphoreGive(wake);
}

bool audio_start(audio_mic_cb cb)
{
    if (!task_up()) return false;
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
    int room = ring_cap - ring_n;
    if (n > room) n = room;                           // (the Mac keeps well ahead of this: nothing to lose normally)
    int w = (ring_r + ring_n) % ring_cap;
    for (int i = 0; i < n; i++) ring[(w + i) % ring_cap] = pcm[i];
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
void audio_click(void) {}
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
void audio_click(void) {}
float audio_mic_level(void) { return 0; }
float audio_out_level(void) { return 0; }
static inline void level_unused(void) { (void)level_update; }
#endif
