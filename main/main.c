#include "app.h"
#include "display.h"
#include "pebble_net.h"
#include "port.h"
#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "pebble";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Own display bring-up so we can truly sleep the panel (The-Badge lesson). */
    if (!pebble_display_start()) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }

    lv_indev_t *indev = pebble_display_indev();
    if (indev) lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    settings_load();
    (void)port_battery_percent(); /* brings AXP up and shuts unused rails */
    port_wifi_boot_connect();     /* NVS creds → STA (scan/join UI if none) */
    pebble_net_start();           /* PROTOCOL.md HTTP worker */
    pebble_display_brightness(0);

    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGW(TAG, "power management config failed");
    }

    bsp_display_lock(UINT32_MAX);
    splash_show();
    bsp_display_unlock();

    /* Fade brightness after first frame settles. */
    vTaskDelay(pdMS_TO_TICKS(80));
    port_brightness_set(port_brightness_get() > 0 ? port_brightness_get() : 45);

    ESP_LOGI(TAG, "Pebble up");
}
