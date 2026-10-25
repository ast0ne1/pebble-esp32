/* Device buttons on Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * BOOT = GPIO0 (active low). PWR conditioned state = TCA9554 EXIO4 (active high).
 * BSP_CAPS_BUTTONS is 0 — roll our own Muse-style hold/short poll.
 */
#include "app.h"
#include "port.h"

#ifndef PEBBLE_SIM

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define BOOT_GPIO           GPIO_NUM_0
#define PWR_PIN             IO_EXPANDER_PIN_NUM_4
#define POLL_MS             40
#define PTT_HOLD_MS         220
#define SOFT_SLEEP_MS       1500

typedef struct {
    bool     down;
    bool     claimed_ptt; /* hold became PTT */
    bool     fired_long;
    uint32_t t0;
} btn_track_t;

static esp_io_expander_handle_t s_exp;
static btn_track_t s_boot;
static btn_track_t s_pwr;

static bool boot_pressed(void)
{
    return gpio_get_level(BOOT_GPIO) == 0;
}

static bool pwr_pressed(void)
{
    if (!s_exp) return false;
    uint32_t level = 0;
    if (esp_io_expander_get_level(s_exp, PWR_PIN, &level) != ESP_OK) return false;
    return (level & PWR_PIN) != 0;
}

static void track_boot(bool pressed, uint32_t now)
{
    if (pressed && !s_boot.down) {
        s_boot.down = true;
        s_boot.claimed_ptt = false;
        s_boot.fired_long = false;
        s_boot.t0 = now;
        return;
    }
    if (pressed && s_boot.down && !s_boot.claimed_ptt && !s_boot.fired_long) {
        /* Hold-to-talk: claim PTT after hold. Toggle mode uses short press only. */
        if (!port_talk_toggle() && now - s_boot.t0 >= PTT_HOLD_MS) {
            s_boot.claimed_ptt = true;
            shell_ptt_set(true);
        }
        return;
    }
    if (!pressed && s_boot.down) {
        uint32_t held = now - s_boot.t0;
        s_boot.down = false;
        if (s_boot.claimed_ptt) {
            shell_ptt_set(false);
            s_boot.claimed_ptt = false;
        } else if (held < PTT_HOLD_MS) {
            shell_note_activity();
            if (port_talk_toggle())
                shell_ptt_toggle();
        }
    }
}

static void track_pwr(bool pressed, uint32_t now)
{
    if (pressed && !s_pwr.down) {
        s_pwr.down = true;
        s_pwr.claimed_ptt = false;
        s_pwr.fired_long = false;
        s_pwr.t0 = now;
        return;
    }
    if (pressed && s_pwr.down && !s_pwr.fired_long) {
        if (now - s_pwr.t0 >= SOFT_SLEEP_MS) {
            s_pwr.fired_long = true;
            shell_ptt_set(false);
            shell_screen_off();
            port_log(TAG, "PWR hold - soft sleep");
        }
        return;
    }
    if (!pressed && s_pwr.down) {
        uint32_t held = now - s_pwr.t0;
        s_pwr.down = false;
        if (s_pwr.fired_long) {
            s_pwr.fired_long = false;
            return;
        }
        if (held < SOFT_SLEEP_MS) {
            shell_note_activity();
            shell_show_home();
            port_log(TAG, "PWR short - home");
        }
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = port_millis();
        port_lock();
        track_boot(boot_pressed(), now);
        track_pwr(pwr_pressed(), now);
        port_unlock();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(void)
{
    static bool s_started;
    if (s_started) return;
    s_started = true;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* BSP 3.x has no bsp_io_expander_init. Probe TCA9554 (0x20) then TCA9554A (0x38). */
    (void)bsp_i2c_init();
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    static const uint32_t addrs[] = {
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
        ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000,
    };
    s_exp = NULL;
    if (i2c) {
        for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            if (i2c_master_probe(i2c, (uint16_t)addrs[i], 50) != ESP_OK) continue;
            if (esp_io_expander_new_i2c_tca9554(i2c, addrs[i], &s_exp) != ESP_OK || !s_exp) {
                s_exp = NULL;
                continue;
            }
            esp_io_expander_set_dir(s_exp, PWR_PIN, IO_EXPANDER_INPUT);
            ESP_LOGI(TAG, "TCA9554 at 0x%02lx (PWR=EXIO4)", (unsigned long)addrs[i]);
            break;
        }
    }
    if (!s_exp) ESP_LOGW(TAG, "no TCA9554 - PWR button disabled");

    xTaskCreate(buttons_task, "buttons", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "BOOT=GPIO0 PWR=EXIO4");
}

#else /* PEBBLE_SIM */

void buttons_start(void)
{
    /* Sim uses Talk / chrome via main_sim stdin — same shell_ptt_set / home APIs. */
}

#endif
