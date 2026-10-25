/* PTT mic → speaker loopback / StonePi ask + short nudge chirps (1.75C). */
#include "voice.h"
#include "app.h"
#include "pebble_net.h"
#include "port.h"
#include "wake.h"
#include "assets/assets.h"

#include <stdlib.h>
#include <string.h>

#ifdef PEBBLE_SIM

static uint32_t s_busy_until;
static bool     s_ask_flight;

void voice_ptt_down(void)
{
    s_busy_until = port_millis() + 60000;
    s_ask_flight = false;
}

void voice_ptt_up(void)
{
    s_busy_until = port_millis() + 800;
    s_ask_flight = port_pair_is_paired();
}

bool voice_is_busy(void)
{
    return port_millis() < s_busy_until || s_ask_flight;
}

bool voice_ask_in_flight(void) { return s_ask_flight; }

void voice_ask_clear(void) { s_ask_flight = false; }

bool voice_play_pcm22050(int16_t *pcm, size_t nsamp)
{
    (void)nsamp;
    free(pcm);
    s_ask_flight = false;
    s_busy_until = port_millis() + 600;
    return true;
}

void voice_play_nudge(unsigned variant)
{
    (void)variant;
}

#else /* device */

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice";

#define SAMPLE_HZ       22050
#define MAX_SEC         10
#define BYTES_PER_SAMP  2
#define MAX_BYTES       (SAMPLE_HZ * MAX_SEC * BYTES_PER_SAMP)
#define CHUNK_BYTES     1024
#define MIN_PLAY_BYTES  (SAMPLE_HZ / 5 * BYTES_PER_SAMP)
/* Match chirp assets (peak-normalized to ~24000). Mic speech is quieter. */
#define PLAY_PEAK_TARGET 24000
#define PLAY_BOOST_MAX_X 12 /* cap AGC so hush/noise doesn't explode */

/* Codec HW/soft-vol left at 100; we scale PCM so every play is the same level. */
static int pcm_gain_q15(void)
{
    int v = settings_get_volume();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    /* Map 0..100 → Q15 (0..32767). Floor so mid settings stay audible. */
    if (v > 0 && v < 40) v = 40;
    return (v * 32767) / 100;
}

/* Bring recorded speech up (or down) to chirp loudness before write_pcm. */
static void pcm_peak_normalize(int16_t *pcm, size_t nsamp)
{
    int peak = 1;
    for (size_t i = 0; i < nsamp; i++) {
        int a = pcm[i];
        if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    if (peak < 200) return; /* near silence — leave alone */
    int32_t num = PLAY_PEAK_TARGET;
    int32_t den = peak;
    /* Cap boost so near-silence hush doesn't become a roar. */
    if (den < (PLAY_PEAK_TARGET + PLAY_BOOST_MAX_X - 1) / PLAY_BOOST_MAX_X) {
        den = (PLAY_PEAK_TARGET + PLAY_BOOST_MAX_X - 1) / PLAY_BOOST_MAX_X;
    }
    if (den == num) return;
    for (size_t i = 0; i < nsamp; i++) {
        int32_t s = ((int32_t)pcm[i] * num) / den;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        pcm[i] = (int16_t)s;
    }
    ESP_LOGI(TAG, "loopback peak %d → target %d (%ld/%ld)", peak, PLAY_PEAK_TARGET,
             (long)num, (long)den);
}

static esp_codec_dev_handle_t s_mic;
static esp_codec_dev_handle_t s_spk;
static int16_t               *s_pcm;
static volatile size_t        s_bytes;
static volatile bool          s_recording;
static volatile bool          s_playing;
static volatile bool          s_sfx;
static volatile bool          s_stop_rec;
static volatile bool          s_ask_flight;
static bool                   s_spk_open;
static TaskHandle_t           s_rec_task;
static TaskHandle_t           s_play_task;
static TaskHandle_t           s_sfx_task;

static const int16_t *s_sfx_pcm;
static size_t         s_sfx_len;
static int16_t       *s_ext_pcm; /* owned play buffer (ask reply) */
static size_t         s_ext_nsamp;

static esp_codec_dev_sample_info_t s_fs = {
    .bits_per_sample = 16,
    .channel = 1,
    .channel_mask = 0,
    .sample_rate = SAMPLE_HZ,
};

static bool ensure_spk(void)
{
    (void)bsp_i2c_init();
    if (!s_spk) s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed");
        return false;
    }
    return true;
}

/* Keep speaker open across chirps — close/reopen was one-shot on this BSP. */
static bool spk_ensure_open(void)
{
    if (!ensure_spk()) return false;
    if (s_spk_open) {
        (void)esp_codec_dev_set_out_mute(s_spk, false);
        return true;
    }
    if (esp_codec_dev_open(s_spk, &s_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "spk open failed");
        return false;
    }
    s_spk_open = true;
    /* Unity codec gain; level comes from pcm_gain_q15() so soft-vol fade
     * can't make the 1st chirp quieter than the next. */
    (void)esp_codec_dev_set_out_vol(s_spk, 100);
    (void)esp_codec_dev_set_out_mute(s_spk, false);
    /* Drain ~50 ms soft-vol ramp with silence so cur == gain before audio. */
    {
        uint8_t z[CHUNK_BYTES];
        memset(z, 0, sizeof(z));
        int need = (SAMPLE_HZ * 50 / 1000) * BYTES_PER_SAMP;
        while (need > 0) {
            int n = need > (int)sizeof(z) ? (int)sizeof(z) : need;
            (void)esp_codec_dev_write(s_spk, z, n);
            need -= n;
        }
    }
    return true;
}

static bool ensure_ready(void)
{
    if (!ensure_spk()) return false;
    if (!s_mic) s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "mic init failed");
        return false;
    }
    if (!s_pcm) {
        s_pcm = heap_caps_malloc(MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm) {
            ESP_LOGE(TAG, "PSRAM pcm alloc failed (%u)", (unsigned)MAX_BYTES);
            return false;
        }
        ESP_LOGI(TAG, "ready %d Hz mono, max %d s", SAMPLE_HZ, MAX_SEC);
    }
    return true;
}

static void write_pcm(const void *data, size_t nbytes)
{
    const int gain = pcm_gain_q15();
    size_t off = 0;
    while (off < nbytes) {
        size_t chunk = nbytes - off;
        if (chunk > CHUNK_BYTES) chunk = CHUNK_BYTES;
        int16_t tmp[CHUNK_BYTES / 2];
        const int16_t *src = (const int16_t *)((const uint8_t *)data + off);
        size_t ns = chunk / 2;
        for (size_t i = 0; i < ns; i++) {
            int32_t s = ((int32_t)src[i] * gain) >> 15;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            tmp[i] = (int16_t)s;
        }
        int err = esp_codec_dev_write(s_spk, tmp, (int)chunk);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "spk write %d", err);
            break;
        }
        off += chunk;
    }
}

static void maybe_wake_resume(void);

static void rec_task(void *arg)
{
    (void)arg;
    uint8_t chunk[CHUNK_BYTES];
    if (esp_codec_dev_open(s_mic, &s_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        s_recording = false;
        s_rec_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* ES7210: more headroom for quiet speech; play_task still peak-norms. */
    (void)esp_codec_dev_set_in_gain(s_mic, 36.0f);

    while (!s_stop_rec && s_bytes + CHUNK_BYTES <= MAX_BYTES) {
        int err = esp_codec_dev_read(s_mic, chunk, CHUNK_BYTES);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read %d", err);
            break;
        }
        memcpy((uint8_t *)s_pcm + s_bytes, chunk, CHUNK_BYTES);
        s_bytes += CHUNK_BYTES;
    }

    esp_codec_dev_close(s_mic);
    s_recording = false;
    s_rec_task = NULL;
    ESP_LOGI(TAG, "rec done %u bytes", (unsigned)s_bytes);
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    (void)arg;
    int16_t *ext = s_ext_pcm;
    size_t ext_n = s_ext_nsamp;
    s_ext_pcm = NULL;
    s_ext_nsamp = 0;

    size_t n;
    int16_t *src;
    if (ext && ext_n) {
        src = ext;
        n = ext_n * BYTES_PER_SAMP;
    } else {
        src = s_pcm;
        n = s_bytes;
        if (n < MIN_PLAY_BYTES) {
            ESP_LOGI(TAG, "clip too short (%u) - skip play", (unsigned)n);
            s_playing = false;
            port_pm_hold(false);
            s_play_task = NULL;
            maybe_wake_resume();
            vTaskDelete(NULL);
            return;
        }
        pcm_peak_normalize(src, n / BYTES_PER_SAMP);
    }

    if (!spk_ensure_open()) {
        free(ext);
        s_playing = false;
        port_pm_hold(false);
        s_play_task = NULL;
        maybe_wake_resume();
        vTaskDelete(NULL);
        return;
    }

    write_pcm(src, n);
    free(ext);

    s_playing = false;
    port_pm_hold(false);
    s_play_task = NULL;
    ESP_LOGI(TAG, "play done");
    maybe_wake_resume();
    vTaskDelete(NULL);
}

/* Linear resample 22050 → 16000 mono. */
static int16_t *resample_to_16k(const int16_t *in, size_t in_nsamp, size_t *out_nsamp)
{
    *out_nsamp = 0;
    if (!in || !in_nsamp) return NULL;
    size_t out_n = (size_t)((uint64_t)in_nsamp * 16000ull / (uint64_t)SAMPLE_HZ);
    if (!out_n) return NULL;
    int16_t *out = (int16_t *)malloc(out_n * sizeof(int16_t));
    if (!out) return NULL;
    for (size_t i = 0; i < out_n; i++) {
        double pos = (double)i * (double)SAMPLE_HZ / 16000.0;
        size_t j = (size_t)pos;
        double frac = pos - (double)j;
        int16_t a = in[j < in_nsamp ? j : in_nsamp - 1];
        int16_t b = in[(j + 1) < in_nsamp ? (j + 1) : in_nsamp - 1];
        out[i] = (int16_t)(a + (b - a) * frac);
    }
    *out_nsamp = out_n;
    return out;
}

static void sfx_task(void *arg)
{
    (void)arg;
    const int16_t *pcm = s_sfx_pcm;
    size_t n = s_sfx_len * BYTES_PER_SAMP;
    port_pm_hold(true);

    if (!spk_ensure_open()) {
        s_sfx = false;
        port_pm_hold(false);
        s_sfx_task = NULL;
        maybe_wake_resume();
        vTaskDelete(NULL);
        return;
    }

    write_pcm(pcm, n);

    s_sfx = false;
    port_pm_hold(false);
    s_sfx_task = NULL;
    maybe_wake_resume();
    vTaskDelete(NULL);
}

static void maybe_wake_resume(void)
{
    if (!s_recording && !s_playing && !s_sfx && !s_ask_flight &&
        !pebble_net_ask_busy())
        wake_resume();
}

void voice_ptt_down(void)
{
    if (s_playing || s_recording || s_sfx) return;
    wake_pause();
    if (!ensure_ready()) {
        maybe_wake_resume();
        return;
    }

    port_pm_hold(true);
    s_stop_rec = false;
    s_bytes = 0;
    s_recording = true;
    if (xTaskCreate(rec_task, "voice_rec", 4096, NULL, 5, &s_rec_task) != pdPASS) {
        s_recording = false;
        port_pm_hold(false);
        maybe_wake_resume();
        ESP_LOGE(TAG, "rec task create failed");
    } else {
        port_log(TAG, "rec start");
    }
}

void voice_ptt_up(void)
{
    if (!s_recording && !s_rec_task) return;
    s_stop_rec = true;
    for (int i = 0; i < 50 && s_recording; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_recording = false;

    /* Cap ask length at protocol max (~15 s). */
    size_t max_ask_bytes = (size_t)SAMPLE_HZ * 15u * BYTES_PER_SAMP;
    if (s_bytes > max_ask_bytes) s_bytes = max_ask_bytes;

    if (port_pair_is_paired() && s_pcm && s_bytes >= MIN_PLAY_BYTES) {
        size_t ns16 = 0;
        int16_t *pcm16 = resample_to_16k(s_pcm, s_bytes / BYTES_PER_SAMP, &ns16);
        port_pm_hold(false);
        if (pcm16 && pebble_net_ask_start(pcm16, ns16)) {
            s_ask_flight = true; /* stay wake-paused until reply play ends */
            port_log(TAG, "ask start %u samples @16k", (unsigned)ns16);
        } else {
            port_log(TAG, "ask start failed — loopback");
            free(pcm16);
            goto loopback;
        }
        free(pcm16);
        return;
    }

loopback:
    if (s_playing) return;
    if (s_bytes < MIN_PLAY_BYTES) {
        maybe_wake_resume();
        return;
    }
    s_playing = true;
    port_pm_hold(true);
    if (xTaskCreate(play_task, "voice_play", 4096, NULL, 5, &s_play_task) != pdPASS) {
        s_playing = false;
        port_pm_hold(false);
        maybe_wake_resume();
        ESP_LOGE(TAG, "play task create failed");
    } else {
        port_log(TAG, "play start");
    }
}

bool voice_ask_in_flight(void) { return s_ask_flight; }

void voice_ask_clear(void)
{
    s_ask_flight = false;
    maybe_wake_resume();
}

bool voice_play_pcm22050(int16_t *pcm, size_t nsamp)
{
    s_ask_flight = false;
    if (!pcm || !nsamp) {
        free(pcm);
        maybe_wake_resume();
        return false;
    }
    if (s_playing || s_recording) {
        free(pcm);
        return false;
    }
    wake_pause();
    s_ext_pcm = pcm;
    s_ext_nsamp = nsamp;
    s_playing = true;
    port_pm_hold(true);
    if (xTaskCreate(play_task, "voice_play", 4096, NULL, 5, &s_play_task) != pdPASS) {
        s_playing = false;
        free(s_ext_pcm);
        s_ext_pcm = NULL;
        s_ext_nsamp = 0;
        port_pm_hold(false);
        maybe_wake_resume();
        return false;
    }
    return true;
}

bool voice_is_busy(void)
{
    return s_recording || s_playing || s_sfx || s_ask_flight || pebble_net_ask_busy();
}

void voice_play_nudge(unsigned variant)
{
    if (!settings_sounds_on()) return;
    if (s_recording || s_playing) return;
    /* Allow re-trigger: if a chirp is mid-play, drop this one quietly. */
    if (s_sfx) return;
    if (!ensure_spk()) return;

    switch (variant % 3) {
    case 1:
        s_sfx_pcm = pcm_nudge1;
        s_sfx_len = pcm_nudge1_len;
        break;
    case 2:
        s_sfx_pcm = pcm_nudge2;
        s_sfx_len = pcm_nudge2_len;
        break;
    default:
        s_sfx_pcm = pcm_nudge0;
        s_sfx_len = pcm_nudge0_len;
        break;
    }
    if (!s_sfx_pcm || !s_sfx_len) return;

    wake_pause();
    s_sfx = true;
    if (xTaskCreate(sfx_task, "voice_sfx", 3072, NULL, 5, &s_sfx_task) != pdPASS) {
        s_sfx = false;
        maybe_wake_resume();
        ESP_LOGE(TAG, "sfx task create failed");
    }
}

#endif
