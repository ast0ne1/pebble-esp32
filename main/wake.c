/* Wake word — PROTOCOL talk.wake / wake_phrase via ESP-SR (AFE + WakeNet + MultiNet). */
#include "wake.h"
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef PEBBLE_SIM

static bool s_want;
static volatile bool s_trig;

bool wake_enabled(void) { return s_want; }

void wake_on_config_changed(void)
{
    s_want = port_talk_wake();
    char phrase[40];
    port_talk_wake_phrase(phrase, sizeof(phrase));
    if (s_want)
        port_log("wake", "enabled (phrase \"%s\") — sim stub", phrase);
    else
        port_log("wake", "off");
}

void wake_start(void) { wake_on_config_changed(); }
void wake_stop(void) { s_want = false; }
void wake_pause(void) {}
void wake_resume(void) {}
bool wake_consume_trigger(void)
{
    if (!s_trig) return false;
    s_trig = false;
    return true;
}

#else /* device */

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

static const char *TAG = "wake";

#define WAKE_HZ           16000
#define DEBOUNCE_MS       2500
#define MIC_GAIN_DB       36.0f

static bool                  s_want;
static volatile bool         s_trig;
static volatile bool         s_paused;
static volatile bool         s_run;
static volatile bool         s_phrase_dirty;
static char                  s_phrase[40];
static char                  s_mn_cmd[48];
static uint32_t              s_last_fire_ms;

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t       *s_afe_data;
static srmodel_list_t          *s_models;
static esp_mn_iface_t          *s_mn;
static model_iface_data_t      *s_mn_data;
static bool                     s_mn_ok;

static esp_codec_dev_handle_t   s_mic;
static bool                     s_mic_open;
static TaskHandle_t             s_feed_task;
static TaskHandle_t             s_detect_task;
static TaskHandle_t             s_setup_task;
static SemaphoreHandle_t        s_lock;

static esp_codec_dev_sample_info_t s_fs = {
    .bits_per_sample = 16,
    .channel = 1,
    .channel_mask = 0,
    .sample_rate = WAKE_HZ,
};

bool wake_enabled(void) { return s_want; }

bool wake_consume_trigger(void)
{
    if (!s_trig) return false;
    s_trig = false;
    return true;
}

static void fire_wake(const char *why)
{
    uint32_t now = port_millis();
    if (now - s_last_fire_ms < DEBOUNCE_MS) return;
    s_last_fire_ms = now;
    s_trig = true;
    port_log(TAG, "trigger (%s)", why ? why : "?");
}

/* MultiNet6 English: uppercase graphemes, letters/spaces only. */
static void phrase_to_mn(const char *in, char *out, size_t n)
{
    size_t o = 0;
    bool space = true;
    for (const char *p = in; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalpha(c)) {
            out[o++] = (char)toupper(c);
            space = false;
        } else if ((c == ' ' || c == '-' || c == '_') && !space && o + 1 < n) {
            out[o++] = ' ';
            space = true;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
    /* "STONEPI" → "STONE PI" helps MultiNet tokenize. */
    if (o >= 7) {
        char *sp = strstr(out, "STONEPI");
        if (sp && o + 1 < n) {
            size_t tail = strlen(sp + 7);
            memmove(sp + 8, sp + 7, tail + 1);
            sp[5] = ' ';
            /* STONE PI */
        }
    }
}

static void mic_close(void)
{
    if (s_mic && s_mic_open) {
        (void)esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
}

static bool mic_open(void)
{
    if (s_mic_open) return true;
    (void)bsp_i2c_init();
    if (!s_mic) s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "mic init failed");
        return false;
    }
    if (esp_codec_dev_open(s_mic, &s_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open 16k failed");
        return false;
    }
    (void)esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    s_mic_open = true;
    return true;
}

static void apply_mn_phrase(void)
{
    if (!s_mn_ok || !s_mn || !s_mn_data) return;
    phrase_to_mn(s_phrase, s_mn_cmd, sizeof(s_mn_cmd));
    if (!s_mn_cmd[0]) {
        snprintf(s_mn_cmd, sizeof(s_mn_cmd), "HEY STONE PI");
    }
    esp_mn_commands_clear();
    if (esp_mn_commands_add(1, s_mn_cmd) != ESP_OK) {
        ESP_LOGW(TAG, "mn add \"%s\" failed", s_mn_cmd);
        return;
    }
    /* Also accept spaced variant if we compacted stonepi. */
    if (strstr(s_mn_cmd, "STONEPI")) {
        char alt[48];
        snprintf(alt, sizeof(alt), "%s", s_mn_cmd);
        char *sp = strstr(alt, "STONEPI");
        if (sp && strlen(alt) + 1 < sizeof(alt)) {
            memmove(sp + 8, sp + 7, strlen(sp + 7) + 1);
            sp[5] = ' ';
            (void)esp_mn_commands_add(1, alt);
        }
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        ESP_LOGW(TAG, "mn update had errors");
    }
    ESP_LOGI(TAG, "multinet phrase \"%s\"", s_mn_cmd);
    s_phrase_dirty = false;
}

static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int nch = s_afe->get_feed_channel_num(s_afe_data);
    size_t bytes = (size_t)chunk * (size_t)nch * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(bytes);
    if (!buf) {
        ESP_LOGE(TAG, "feed buf alloc failed");
        s_feed_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_run) {
        if (s_paused || !s_want) {
            mic_close();
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (!mic_open()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int err = esp_codec_dev_read(s_mic, buf, (int)bytes);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read %d", err);
            mic_close();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_afe->feed(s_afe_data, buf);
    }

    mic_close();
    free(buf);
    s_feed_task = NULL;
    vTaskDelete(NULL);
}

static void detect_task(void *arg)
{
    (void)arg;

    while (s_run) {
        if (s_paused || !s_want || !s_afe_data) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (s_phrase_dirty) apply_mn_phrase();

        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED ||
            res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            /* Built-in "Hi ESP" — start ask listen immediately. */
            fire_wake("wakenet");
            if (s_mn_ok) s_mn->clean(s_mn_data);
            continue;
        }

        if (!s_mn_ok) continue;

        /* Continuous MultiNet for the custom wake_phrase. */
        esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r && r->num > 0) fire_wake("multinet");
            s_mn->clean(s_mn_data);
            if (s_afe->enable_wakenet) s_afe->enable_wakenet(s_afe_data);
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            s_mn->clean(s_mn_data);
            if (s_afe->enable_wakenet) s_afe->enable_wakenet(s_afe_data);
        }
    }

    s_detect_task = NULL;
    vTaskDelete(NULL);
}

static void teardown_engine(void)
{
    s_run = false;
    for (int i = 0; i < 100 && (s_feed_task || s_detect_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    mic_close();
    if (s_mn_data && s_mn) {
        s_mn->destroy(s_mn_data);
        s_mn_data = NULL;
    }
    s_mn = NULL;
    s_mn_ok = false;
    if (s_afe && s_afe_data) {
        s_afe->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_afe = NULL;
}

static bool setup_engine(void)
{
    if (s_afe_data) return true;

    if (!s_models) {
        s_models = esp_srmodel_init("model");
        if (!s_models) {
            ESP_LOGE(TAG, "esp_srmodel_init(model) failed — flash model partition?");
            return false;
        }
    }

    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return false;
    }
    /* Prefer HiESP if present in the model list. */
    char *wn = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hiesp");
    if (wn) {
        cfg->wakenet_model_name = wn;
        cfg->wakenet_init = true;
    }
    s_afe = esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "afe create failed");
        return false;
    }

    /* MultiNet create() null-derefs when a mid-load PSRAM alloc fails
     * (~6 KiB item) after LVGL+Wi‑Fi — boot loop. WakeNet HiESP only for now. */
    (void)esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    s_mn_ok = false;
    ESP_LOGW(TAG, "MultiNet disabled — WakeNet HiESP only");

    s_run = true;
    if (xTaskCreatePinnedToCore(feed_task, "wake_feed", 8 * 1024, NULL, 5,
                                &s_feed_task, 0) != pdPASS) {
        s_feed_task = NULL;
        s_run = false;
        teardown_engine();
        return false;
    }
    if (xTaskCreatePinnedToCore(detect_task, "wake_det", 8 * 1024, NULL, 5,
                                &s_detect_task, 1) != pdPASS) {
        s_detect_task = NULL;
        s_run = false;
        teardown_engine();
        return false;
    }
    ESP_LOGI(TAG, "listening (wn=%s mn=%s phrase=\"%s\")",
             wn ? "hiesp" : "none", s_mn_ok ? "en" : "off", s_mn_cmd);
    return true;
}

void wake_pause(void)
{
    s_paused = true;
    /* Give feed task a moment to close the mic before voice reopens at 22k. */
    vTaskDelay(pdMS_TO_TICKS(30));
    mic_close();
}

void wake_resume(void)
{
    if (s_want) s_paused = false;
}

void wake_stop(void)
{
    s_want = false;
    s_paused = true;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    teardown_engine();
    if (s_lock) xSemaphoreGive(s_lock);
    port_log(TAG, "off");
}

/* esp_srmodel_init mmaps flash and freezes cache — must not run on a PSRAM
 * stack (LVGL worker). Persistent setup task uses an internal-RAM stack. */
static void wake_setup_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_want) {
            if (s_lock) xSemaphoreGive(s_lock);
            continue;
        }
        if (!s_afe_data) {
            s_paused = false;
            if (!setup_engine()) {
                s_want = false;
                port_log(TAG, "enable failed");
            } else {
                port_log(TAG, "enabled phrase \"%s\"", s_phrase);
            }
        } else {
            s_phrase_dirty = true;
            s_paused = false;
            port_log(TAG, "enabled phrase \"%s\"", s_phrase);
        }
        if (s_lock) xSemaphoreGive(s_lock);
    }
}

static void kick_setup_task(void)
{
    if (!s_setup_task) {
        /* Prefer a modest internal stack; 12 KiB often fails after LVGL+Wi‑Fi. */
        BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
            wake_setup_task, "wake_setup", 8 * 1024, NULL, 5, &s_setup_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 0);
        if (ok != pdPASS) {
            static StackType_t *stack;
            static StaticTask_t tcb;
            if (!stack) {
                stack = heap_caps_malloc(8 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (stack) {
                s_setup_task = xTaskCreateStaticPinnedToCore(
                    wake_setup_task, "wake_setup", 8 * 1024 / sizeof(StackType_t),
                    NULL, 5, stack, &tcb, 0);
            }
        }
        if (!s_setup_task) {
            ESP_LOGE(TAG, "wake_setup task create failed (int heap %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_want = false;
            return;
        }
    }
    xTaskNotifyGive(s_setup_task);
}

void wake_on_config_changed(void)
{
    bool want = port_talk_wake();
    char phrase[40];
    port_talk_wake_phrase(phrase, sizeof(phrase));
    if (!phrase[0]) snprintf(phrase, sizeof(phrase), "hey stonepi");

    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    if (!want) {
        if (s_want || s_afe_data) wake_stop();
        return;
    }

    s_want = true;
    if (strcmp(s_phrase, phrase) != 0) {
        snprintf(s_phrase, sizeof(s_phrase), "%s", phrase);
        s_phrase_dirty = true;
    }
    kick_setup_task();
}

void wake_start(void)
{
    wake_on_config_changed();
}

#endif /* device */
