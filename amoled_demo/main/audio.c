#include <string.h>
#include <math.h>
#include "audio.h"
#include "board_square.h"
#include "esp_attr.h"

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
static int16_t *raw, *mic, *ref, *clean;
static TaskHandle_t task;
static SemaphoreHandle_t wake, opened, ring_lock;
static volatile bool want, running, speaker_mute;
static volatile bool open_ok;
static audio_mic_cb mic_cb;
static volatile float mic_lvl, out_lvl;
static int16_t *ring;
static int ring_cap;                     // samples it holds: a few seconds of reply waiting to be played (the Mac keeps 2 s ahead)
static volatile int ring_r, ring_n;      // read position, how many queued
EXT_RAM_BSS_ATTR static int16_t click[AUDIO_RATE * 60 / 1000], quiet[AUDIO_RATE * 30 / 1000];
EXT_RAM_BSS_ATTR static int16_t chime[AUDIO_RATE * 700 / 1000];
static int chime_n;                      // samples in chime[]
enum { SND_NONE, SND_CLICK, SND_CHIME };
static volatile int snd_wanted;          // audio_click() / audio_chime() asked for one...
static const int16_t *snd;               // ...what a chat's frames are mixing in,
static int snd_n, snd_at = -1;           // how long it is, and how far they've got (-1: not playing)
static const int16_t *snd_pcm(int k, int *n)
{
    if (k == SND_CHIME) { *n = chime_n; return chime; }
    *n = sizeof click / sizeof click[0];
    return click;
}
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

// ---- the plugged-in sound, "landing": built like the Mac's own (measured from connect_power.aif): pure tones, no noise
// or clashing overtones (a small speaker makes those harsh), a soft swell rather than a strike, falling an octave so it
// settles. An octave over the Mac's: this speaker gives next to nothing under ~800 Hz (sounds down there played in full
// and weren't heard). C7 swells in and hands over to C6, which pulses twice and rings out.
#define CHIME_S 0.70f

static float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

static void make_chime(void)
{
    int n = (int)(CHIME_S * AUDIO_RATE);
    float ph[2] = {0}, peak = 0;
    for (int pass = 0; pass < 2; pass++) {            // (the first finds the peak, the second scales to it)
        ph[0] = ph[1] = 0;
        for (int i = 0; i < n; i++) {
            float t = (float)i / AUDIO_RATE;
            ph[0] += 2 * (float)M_PI * 2093 / AUDIO_RATE;
            ph[1] += 2 * (float)M_PI * 1047 / AUDIO_RATE;
            float hi = t < 0.07f ? powf(t / 0.07f, 1.5f) : expf(-(t - 0.07f) / 0.07f);
            float u = t - 0.065f, lo = 0;
            if (u > 0) {
                float second = u < 0.135f ? smooth01((u - 0.05f) / 0.085f) : expf(-(u - 0.135f) / 0.1f);
                lo = (u < 0.01f ? u / 0.01f : 1) * (0.9f * expf(-u / 0.045f) + second);
            }
            float x = 0.8f * hi * sinf(ph[0]) + lo * sinf(ph[1]);
            x *= t > CHIME_S - 0.03f ? (CHIME_S - t) / 0.03f : 1;   // (to silence by the end)
            if (!pass) { if (fabsf(x) > peak) peak = fabsf(x); }
            else chime[i] = (int16_t)(x * 22000 / peak);
        }
    }
    chime_n = n;
}

// Give the codec the volume picked in Settings, if it's changed (0: silent)
static void follow_volume(void)
{
    int v = device_volume_get();
    if (v != vol_set) esp_codec_dev_set_out_vol(out_dev, v);
    vol_set = v;
}

static i2s_chan_handle_t tx, rx;

// Gives back the I2S channels (and their DMA buffers) after a setup that didn't work, so the next try can start clean
static void hw_undo(void)
{
    if (tx) { i2s_channel_disable(tx); i2s_del_channel(tx); tx = NULL; }
    if (rx) { i2s_channel_disable(rx); i2s_del_channel(rx); rx = NULL; }
}

static bool hw_init(void)
{
    // DMA buffers in internal RAM (all I2S can use), which is short here: 6 x 8 ms each way (the microphones' side has
    // to hold a 32 ms frame while the last one's echo-cancelled), about 9 KB
    i2s_chan_config_t cc = {.id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 128,
                            .auto_clear_after_cb = true};
    ESP_LOGI(TAG, "starting: %u bytes of internal DMA memory free", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
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
    return out_dev && in_dev;
}

// The echo canceller and the reply's buffer: made when a chat starts and freed when it ends. PSRAM is mostly the
// screen's frames, and the calendar needs a lot of what's left while it reads a big calendar (holding these for good
// left it with no room: every event was dropped).
static bool session_alloc(void)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    aec_config_t ac = {.mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = 4, .sample_rate = AUDIO_RATE,
                       .caps = MALLOC_CAP_SPIRAM, .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR};
    if (!aec) aec = aec_create_from_config(&ac);
    if (!aec) return false;
    chunk = aec_get_chunksize(aec);
    if (!raw) {                                       // (small, and the same every time: kept)
        raw = heap_caps_aligned_alloc(16, chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        mic = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        ref = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        clean = heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    int16_t *r = NULL;
    int cap = 0;
    static const int secs[] = {5, 3};               // (the Mac sends 2 s ahead: 5 s is plenty, 3 s will do)
    for (int i = 0; i < 2 && !r; i++) r = heap_caps_malloc((cap = AUDIO_RATE * secs[i]) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    ring = r;
    ring_cap = r ? cap : 0;
    ring_r = ring_n = 0;
    xSemaphoreGive(ring_lock);
    if (!raw || !mic || !ref || !clean || !ring) return false;
    ESP_LOGI(TAG, "on: %d-sample frames, %u KB of PSRAM for the echo canceller and the reply (%u KB left)", chunk,
             (unsigned)((before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

static void session_free(void)
{
    if (aec) { aec_destroy(aec); aec = NULL; }
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    int16_t *r = ring;
    ring = NULL;
    ring_cap = ring_n = 0;
    xSemaphoreGive(ring_lock);
    heap_caps_free(r);
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
    int k = !ring ? 0 : ring_n < n ? ring_n : n;
    for (int i = 0; i < k; i++) out[i] = ring[(ring_r + i) % ring_cap];
    if (k) ring_r = (ring_r + k) % ring_cap;          // (no ring at all: ring_cap 0, nothing taken)
    ring_n -= k;
    xSemaphoreGive(ring_lock);
    if (k < n) memset(out + k, 0, (n - k) * sizeof(int16_t));
    if (speaker_mute) memset(out, 0, n * sizeof(int16_t));
}

// A tick (or the plug-in sound) while no chat is open: just the speaker, on for it and off again once it's been quiet a while
static void tick_alone(void)
{
    int k = snd_wanted, n;
    snd_wanted = SND_NONE;
    if (device_volume_get() <= 0) return;
    if (!out_only) {
        esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE};
        if (esp_codec_dev_open(out_dev, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "couldn't open the speaker"); return; }
        out_only = true;
        vol_set = -1;
        follow_volume();
        if (!sq_amp(true)) ESP_LOGE(TAG, "the amplifier didn't come on");
        for (int i = 0; i < WAKE_MS / 30; i++) esp_codec_dev_write(out_dev, quiet, sizeof quiet);   // (30 ms each)
    }
    follow_volume();
    const int16_t *pcm = snd_pcm(k, &n);
    if (esp_codec_dev_write(out_dev, (void *)pcm, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) ESP_LOGE(TAG, "couldn't play a sound");
    esp_codec_dev_write(out_dev, quiet, sizeof quiet);       // (so the tick's tail isn't cut off when it closes)
}

// During a chat, the speaker has a task of its own: small pieces, one after another, so its buffer never runs dry
// while the echo canceller works through a 32 ms frame (doing both in one loop left a gap every frame: a crackle all
// through the voice). A tick from the volume slider (or the plug-in sound) is mixed in.
#define PLAY_N 128                               // 8 ms a piece
static SemaphoreHandle_t play_wake;
static volatile bool play_parked = true;        // the speaker task is waiting (not writing): safe to close the codec

static void play_task(void *arg)
{
    static int16_t piece[PLAY_N];
    for (;;) {
        if (!running) {
            play_parked = true;
            xSemaphoreTake(play_wake, portMAX_DELAY);
            continue;
        }
        play_parked = false;
        follow_volume();
        ring_take(piece, PLAY_N);
        level_update(&out_lvl, piece, PLAY_N);
        if (snd_wanted) {
            snd = snd_pcm(snd_wanted, &snd_n);
            snd_wanted = SND_NONE;
            snd_at = device_volume_get() > 0 ? 0 : -1;
        }
        for (int i = 0; snd_at >= 0 && i < PLAY_N; i++, snd_at++) {   // a tick over it
            if (snd_at >= snd_n) { snd_at = -1; break; }
            int v = piece[i] + snd[snd_at];
            piece[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        }
        esp_codec_dev_write(out_dev, piece, sizeof piece);   // (waits for room: that's what keeps it in time)
    }
}

static void audio_task(void *arg)
{
    make_chime();                                     // (here, not on the caller's task: it takes a moment)
    for (;;) {
        if (want != running) {
            if (want) {
                if (out_only) { esp_codec_dev_close(out_dev); out_only = false; }   // (a chat opens both)
                open_ok = devs_open();
                running = open_ok;
                if (running) { sq_amp(true); xSemaphoreGive(play_wake); }
                else want = false;
            } else {
                running = false;
                for (int i = 0; i < 50 && !play_parked; i++) vTaskDelay(pdMS_TO_TICKS(2));   // (its last piece)
                sq_amp(false);
                esp_codec_dev_close(in_dev);
                esp_codec_dev_close(out_dev);
                mic_lvl = out_lvl = 0;
            }
            xSemaphoreGive(opened);
        }
        if (!running) {
            if (snd_wanted) { tick_alone(); continue; }
            if (xSemaphoreTake(wake, out_only ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY) != pdTRUE && out_only && !snd_wanted) {
                sq_amp(false);                        // been quiet: the speaker off again
                esp_codec_dev_close(out_dev);
                out_only = false;
            }
            continue;
        }
        if (esp_codec_dev_read(in_dev, raw, chunk * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) { vTaskDelay(1); continue; }
        for (int i = 0; i < chunk; i++) { mic[i] = raw[2 * i]; ref[i] = raw[2 * i + 1]; }
        aec_process(aec, mic, ref, clean);
        level_update(&mic_lvl, clean, chunk);
        if (mic_cb) mic_cb(clean, chunk);
    }
}

bool audio_available(void) { return true; }

static bool task_up(void)
{
    if (task) return true;
    if (!hw_init()) { ESP_LOGE(TAG, "the speaker and microphones didn't come up"); hw_undo(); return false; }   // (next tap tries again)
    make_click();
    wake = xSemaphoreCreateBinary();
    opened = xSemaphoreCreateBinary();
    ring_lock = xSemaphoreCreateMutex();
    play_wake = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCoreWithCaps(play_task, "speaker", 3072, NULL, 10, NULL, 1, MALLOC_CAP_SPIRAM);
    // (above the drawing, which uses both cores in bursts: a late frame here is a click in the speaker; its stack in
    // PSRAM, as the square runs its code from there anyway: internal RAM is short)
    xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", 6144, NULL, 9, &task, 1, MALLOC_CAP_SPIRAM);
    return true;
}

void audio_click(void)
{
    if (!task_up()) return;
    snd_wanted = SND_CLICK;
    xSemaphoreGive(wake);
}

void audio_chime(void)
{
    if (!task_up()) return;
    snd_wanted = SND_CHIME;
    xSemaphoreGive(wake);
}

bool audio_start(audio_mic_cb cb)
{
    if (!task_up()) return false;
    mic_cb = cb;
    if (running) return true;
    if (!session_alloc()) { ESP_LOGE(TAG, "not enough memory for a chat"); session_free(); return false; }
    xSemaphoreTake(opened, 0);
    want = true;
    xSemaphoreGive(wake);
    xSemaphoreTake(opened, pdMS_TO_TICKS(1500));
    if (!open_ok) { ESP_LOGE(TAG, "couldn't open the codecs"); if (!running) session_free(); }
    return open_ok;
}

void audio_stop(void)
{
    if (!task || !running) return;
    xSemaphoreTake(opened, 0);
    want = false;
    xSemaphoreTake(opened, pdMS_TO_TICKS(500));
    if (!running) session_free();                     // (the audio task has let go of them)
}

void audio_play(const int16_t *pcm, int n)
{
    if (!ring_lock) return;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    if (!ring) n = 0;
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
void audio_chime(void) {}
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
void audio_chime(void) {}
float audio_mic_level(void) { return 0; }
float audio_out_level(void) { return 0; }
static inline void level_unused(void) { (void)level_update; }
#endif
