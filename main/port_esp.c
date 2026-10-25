/* Board side of port.h — AXP / NVS / PM / Wi-Fi stubs.
 * AXP unused-rail shutdown adapted from curisama/The-Badge (MIT). */
#include "port.h"
#include "pebble_net.h"
#include "display.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "port";
#define KV_NS "pebble"

void port_lock(void)   { bsp_display_lock(UINT32_MAX); }
void port_unlock(void) { bsp_display_unlock(); }

void port_log(const char *tag, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(tag ? tag : TAG, "%s", buf);
}

bool port_kv_read(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(KV_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t need = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &need);
    if (err != ESP_OK || need == 0) {
        nvs_close(h);
        return false;
    }
    memset(out, 0, len);
    size_t got = len;
    err = nvs_get_blob(h, key, out, &got);
    nvs_close(h);
    /* Accept shorter blobs (older firmware struct); refuse longer (corrupt). */
    return err == ESP_OK && got <= len;
}

void port_kv_write(const char *key, const void *in, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(KV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, in, len);
    nvs_commit(h);
    nvs_close(h);
}

void port_kv_erase(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(KV_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t port_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void port_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ── brightness / display ───────────────────────────────────── */

static int s_bright = 45;

void port_brightness_set(int percent)
{
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    s_bright = percent;
    pebble_display_brightness(percent);
}

int port_brightness_get(void) { return s_bright; }

void port_display_on(bool on) { pebble_display_on(on); }
bool port_display_is_on(void) { return true; /* refined when shell tracks */ }

/* ── light-sleep hold ───────────────────────────────────────── */

static esp_pm_lock_handle_t s_nosleep;
static int s_nosleep_n;

void port_pm_hold(bool on)
{
    if (on) {
        if (!s_nosleep) {
            if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "pebble", &s_nosleep) != ESP_OK)
                return;
        }
        if (s_nosleep_n++ == 0) esp_pm_lock_acquire(s_nosleep);
    } else if (s_nosleep_n > 0) {
        if (--s_nosleep_n == 0) esp_pm_lock_release(s_nosleep);
    }
}

/* ── AXP2101 ────────────────────────────────────────────────── */

#define AXP_ADDR         0x34
#define AXP_REG_STATUS1  0x00
#define AXP_REG_STATUS2  0x01
#define AXP_REG_BAT_PCT  0xA4
#define AXP_REG_COMMON   0x10

static i2c_master_dev_handle_t s_axp;
static bool s_axp_ready;

static bool axp_rd(uint8_t reg, uint8_t *v)
{
    if (!s_axp) return false;
    return i2c_master_transmit_receive(s_axp, &reg, 1, v, 1, 100) == ESP_OK;
}

static bool axp_wr(uint8_t reg, uint8_t v)
{
    if (!s_axp) return false;
    uint8_t buf[2] = { reg, v };
    return i2c_master_transmit(s_axp, buf, 2, 100) == ESP_OK;
}

static void axp_init(void)
{
    if (s_axp_ready) return;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus for AXP");
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        s_axp = NULL;
        ESP_LOGW(TAG, "AXP2101 not reachable");
        return;
    }
    s_axp_ready = true;

    /* Unused rails off — The-Badge discovery (DCDC1 + ALDO1 only). */
    uint8_t d0 = 0, l0 = 0, l1 = 0;
    axp_rd(0x80, &d0);
    axp_rd(0x90, &l0);
    axp_rd(0x91, &l1);
    ESP_LOGI(TAG, "axp before DCDC=0x%02X LDO=0x%02X/0x%02X", d0, l0, l1);
    axp_wr(0x80, (uint8_t)((d0 & ~0x1E) | 0x01));
    axp_wr(0x90, 0x01);
    axp_wr(0x91, (uint8_t)(l1 & ~0x01));
    axp_rd(0x80, &d0);
    axp_rd(0x90, &l0);
    axp_rd(0x91, &l1);
    ESP_LOGI(TAG, "axp after  DCDC=0x%02X LDO=0x%02X/0x%02X", d0, l0, l1);

    /* Gentler charge defaults (also from The-Badge). */
    uint8_t t = 0, ip = 0, vo = 0;
    axp_rd(0x63, &t);
    axp_wr(0x63, (uint8_t)((t & 0xF0) | 0x01));
    axp_rd(0x61, &ip);
    axp_wr(0x61, (uint8_t)((ip & 0xF0) | 0x01));
    axp_rd(0x24, &vo);
    axp_wr(0x24, (uint8_t)((vo & 0xF8) | 0x04));
}

void port_power_off(void)
{
    axp_init();
    uint8_t v = 0;
    if (!axp_rd(AXP_REG_COMMON, &v)) return;
    ESP_LOGI(TAG, "power off");
    axp_wr(AXP_REG_COMMON, v | 0x01);
}

int port_battery_percent(void)
{
    axp_init();
    uint8_t st1 = 0, pct = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st1)) return -1;
    if (!(st1 & 0x08)) return -1;
    if (!axp_rd(AXP_REG_BAT_PCT, &pct)) return -1;
    return pct > 100 ? 100 : pct;
}

bool port_battery_charging(void)
{
    axp_init();
    uint8_t st = 0;
    if (!axp_rd(AXP_REG_STATUS2, &st)) return false;
    return ((st >> 5) & 0x03) == 0x01;
}

bool port_battery_plugged(void)
{
    axp_init();
    uint8_t st1 = 0;
    if (!axp_rd(AXP_REG_STATUS1, &st1)) return false;
    return (st1 & 0x20) != 0;
}

/* ── Wi-Fi STA: scan / join / NVS / boot reconnect ───────────── */

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

#define WIFI_JOIN_RETRY_MAX 5
#define WIFI_JOIN_TIMEOUT_MS 25000u

static bool s_wifi_inited;
static bool s_wifi_up;
static bool s_scan_running;
static bool s_boot_done;
static char s_ssid[33];
static char s_ip[24];
static char s_pending_pass[65];
static bool s_pending_save;
static int  s_try_state; /* WIFI_TRY_* */
static int  s_pm_wifi;   /* nested hold while radio busy */
/* Muse-style: disconnect while joining → retry, then fail (never hang on Joining). */
static bool     s_joining;
static bool     s_expect_leave; /* ignore ASSOC_LEAVE from our own disconnect() */
static bool     s_sntp_started;
static int      s_join_retry;
static uint32_t s_join_deadline_ms;

void port_tz_set(const char *posix_tz)
{
    const char *tz = (posix_tz && posix_tz[0]) ? posix_tz : "UTC0";
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "TZ=%s", tz);
}

static void sntp_on_sync(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "time synced %04d-%02d-%02d %02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void sntp_start(void)
{
    if (s_sntp_started) {
        /* Already running — nudge another poll after roam / reboot join. */
        esp_sntp_restart();
        return;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(sntp_on_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started");
}

static void wifi_cred_load(wifi_cred_t *out)
{
    memset(out, 0, sizeof(*out));
    (void)port_kv_read("wificfg", out, sizeof(*out));
}

static void wifi_cred_save(const char *ssid, const char *pass)
{
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    if (ssid) snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    if (pass) snprintf(c.pass, sizeof(c.pass), "%s", pass);
    port_kv_write("wificfg", &c, sizeof(c));
}

static void wifi_pm(bool on)
{
    if (on) {
        if (s_pm_wifi++ == 0) port_pm_hold(true);
    } else if (s_pm_wifi > 0) {
        if (--s_pm_wifi == 0) port_pm_hold(false);
    }
}

static void join_fail(const char *why)
{
    s_joining = false;
    s_join_retry = 0;
    s_join_deadline_ms = 0;
    if (s_try_state == WIFI_TRY_BUSY) wifi_pm(false);
    s_try_state = WIFI_TRY_FAIL;
    s_pending_save = false;
    ESP_LOGW(TAG, "wifi join failed (%s)", why ? why : "?");
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_wifi_up = true;
    bool was_busy = (s_try_state == WIFI_TRY_BUSY);
    s_joining = false;
    s_join_retry = 0;
    s_join_deadline_ms = 0;
    s_try_state = WIFI_TRY_OK;
    if (s_pending_save) {
        wifi_cred_save(s_ssid, s_pending_pass);
        s_pending_save = false;
        memset(s_pending_pass, 0, sizeof(s_pending_pass));
    }
    if (was_busy) wifi_pm(false);
    ESP_LOGI(TAG, "wifi ip %s ssid %s", s_ip, s_ssid);
    sntp_start();
}

static void on_wifi_disc(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
    uint8_t reason = ev ? ev->reason : 0;

    s_wifi_up = false;
    s_ip[0] = 0;

    if (s_joining) {
        /* Our own disconnect() before a fresh join — connect() already queued.
         * Counting this as a retry double-connects and burns the budget. */
        if (s_expect_leave && reason == WIFI_REASON_ASSOC_LEAVE) {
            s_expect_leave = false;
            ESP_LOGI(TAG, "wifi leave (switching AP)");
            return;
        }
        s_expect_leave = false;
        /* Real failed assoc — retry like Muse. */
        if (s_join_retry < WIFI_JOIN_RETRY_MAX) {
            s_join_retry++;
            ESP_LOGI(TAG, "wifi retry %d/%d (reason %u)", s_join_retry,
                     WIFI_JOIN_RETRY_MAX, (unsigned)reason);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                join_fail(esp_err_to_name(err));
            }
            return;
        }
        char why[28];
        snprintf(why, sizeof(why), "reason %u", (unsigned)reason);
        join_fail(why);
        return;
    }

    if (s_try_state == WIFI_TRY_OK) {
        s_try_state = WIFI_TRY_FAIL;
        ESP_LOGW(TAG, "wifi dropped reason %u", (unsigned)reason);
    }
}

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_scan_running = false;
    wifi_pm(false);
}

static void wifi_ensure(void)
{
    if (s_wifi_inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disc, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &on_scan_done, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_inited = true;
}

void port_wifi_scan_start(void)
{
    wifi_ensure();
    if (s_scan_running) return;
    wifi_pm(true);
    s_scan_running = true;
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start %s", esp_err_to_name(err));
        s_scan_running = false;
        wifi_pm(false);
    }
}

int port_wifi_scan_result(wifi_found_t *out, int max)
{
    if (s_scan_running) return WIFI_SCAN_RUNNING;
    if (!out || max <= 0) return 0;
    if (!s_wifi_inited) return 0;

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) return 0;
    if (ap_num == 0) return 0;

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(*recs));
    if (!recs) return 0;
    uint16_t n = ap_num;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        return 0;
    }

    wifi_cred_t cred;
    wifi_cred_load(&cred);

    int filled = 0;
    for (uint16_t i = 0; i < n && filled < max; i++) {
        if (!recs[i].ssid[0]) continue;
        /* Dedupe SSID — keep strongest. */
        int dup = -1;
        for (int j = 0; j < filled; j++) {
            if (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = j;
                break;
            }
        }
        if (dup >= 0) {
            if (recs[i].rssi > out[dup].rssi) out[dup].rssi = recs[i].rssi;
            continue;
        }
        snprintf(out[filled].ssid, sizeof(out[filled].ssid), "%s", (const char *)recs[i].ssid);
        out[filled].rssi = recs[i].rssi;
        out[filled].saved = (cred.ssid[0] && strcmp(cred.ssid, out[filled].ssid) == 0) ? 1 : 0;
        out[filled].open = (recs[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
        filled++;
    }
    free(recs);
    return filled;
}

void port_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    wifi_ensure();
    /* Scan holds the radio — abort so set_config/connect can run (Muse). */
    if (s_scan_running) {
        (void)esp_wifi_scan_stop();
        s_scan_running = false;
        wifi_pm(false);
    }
    wifi_pm(true);
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    if (pass) snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);
    /* Weakest acceptable auth — lets WPA/WPA2/WPA3 through (Muse uses OPEN). */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
    if (pass) snprintf(s_pending_pass, sizeof(s_pending_pass), "%s", pass);
    s_pending_save = true;
    s_joining = true;
    s_expect_leave = false;
    s_join_retry = 0;
    s_try_state = WIFI_TRY_BUSY;
    s_wifi_up = false;
    s_ip[0] = 0;
    s_join_deadline_ms = port_millis() + WIFI_JOIN_TIMEOUT_MS;

    /* Only disconnect if associated — ignore that ASSOC_LEAVE in on_wifi_disc. */
    wifi_ap_record_t apinfo;
    if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
        s_expect_leave = true;
        (void)esp_wifi_disconnect();
        /* Let the driver settle before set_config (avoids ESP_ERR_WIFI_STATE). */
        port_delay_ms(120);
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_ERR_WIFI_STATE) {
        port_delay_ms(200);
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (err != ESP_OK) {
        s_expect_leave = false;
        join_fail(esp_err_to_name(err));
        return;
    }
    err = esp_wifi_connect();
    /* ESP_ERR_WIFI_CONN = connect already in flight (e.g. after leave) — OK. */
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_expect_leave = false;
        join_fail(esp_err_to_name(err));
    }
}

void port_wifi_boot_connect(void)
{
    if (s_boot_done) return;
    s_boot_done = true;
    wifi_cred_t c;
    wifi_cred_load(&c);
    if (!c.ssid[0]) return;
    ESP_LOGI(TAG, "wifi boot join %s", c.ssid);
    port_wifi_connect(c.ssid, c.pass);
}

int port_wifi_try_state(void)
{
    if (s_try_state == WIFI_TRY_BUSY && s_join_deadline_ms != 0 &&
        port_millis() >= s_join_deadline_ms) {
        join_fail("timeout");
    }
    return s_try_state;
}

bool port_wifi_connected(void) { return s_wifi_up; }

bool port_wifi_saved_pass(const char *ssid, char *pass_out, size_t pass_n)
{
    if (!ssid || !ssid[0] || !pass_out || pass_n == 0) return false;
    wifi_cred_t c;
    wifi_cred_load(&c);
    if (!c.ssid[0] || strcmp(c.ssid, ssid) != 0) return false;
    snprintf(pass_out, pass_n, "%s", c.pass);
    return true;
}

void port_wifi_ssid(char *out, size_t n)
{
    if (!out || n == 0) return;
    snprintf(out, n, "%s", s_ssid);
}

void port_wifi_ip(char *out, size_t n)
{
    if (!out || n == 0) return;
    snprintf(out, n, "%s", s_ip);
}

/* ── Pairing ────────────────────────────────────────────────── */

static void make_claim(char *out, size_t n)
{
    /* PROTOCOL.md: no lookalikes (0/O, 1/I/L, 2/Z, 5/S, 8/B). */
    static const char alphabet[] = "ACDEFHJKMNPRTUVWXY34679";
    const size_t alen = sizeof(alphabet) - 1;
    if (n < 5) {
        if (n) out[0] = 0;
        return;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = alphabet[esp_random() % alen];
    }
    out[4] = 0;
}

bool port_pair_load(pebble_pair_t *out)
{
    if (!out) return false;
    if (!port_kv_read("pair", out, sizeof(*out))) return false;
    /* Legacy default was http://stonepi.local[:8015]; device has no mdns yet. */
    if (!out->token[0] && out->host_edit[0] &&
        (strstr(out->host_edit, ".local") || strstr(out->host_edit, ".LOCAL"))) {
        out->host_edit[0] = 0;
        port_kv_write("pair", out, sizeof(*out));
    }
    /* Paired but AgentLink stored *.local as hosts.lan — fall back to host_edit. */
    if (out->token[0] && out->host_primary[0] &&
        (strstr(out->host_primary, ".local") || strstr(out->host_primary, ".LOCAL"))) {
        out->host_primary[0] = 0;
        if (out->host_edit[0] &&
            !strstr(out->host_edit, ".local") && !strstr(out->host_edit, ".LOCAL")) {
            snprintf(out->host_primary, sizeof(out->host_primary), "%s", out->host_edit);
        }
        port_kv_write("pair", out, sizeof(*out));
    }
    port_pair_ensure_host_edit(out);
    return true;
}

void port_pair_save(const pebble_pair_t *in)
{
    if (!in) return;
    port_kv_write("pair", in, sizeof(*in));
}

void port_pair_clear(void) { port_kv_erase("pair"); }

bool port_pair_is_paired(void)
{
    pebble_pair_t p;
    if (!port_pair_load(&p)) return false;
    return p.token[0] != 0;
}

void port_pair_new_claim(void)
{
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    if (p.token[0]) return;
    make_claim(p.claim, sizeof(p.claim));
    port_pair_ensure_host_edit(&p);
    port_pair_save(&p);
}

const char *port_pair_claim_code(void)
{
    static char code[8];
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    if (port_pair_load(&p) && p.claim[0] && !p.token[0]) {
        snprintf(code, sizeof(code), "%s", p.claim);
        return code;
    }
    make_claim(code, sizeof(code));
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    snprintf(p.claim, sizeof(p.claim), "%s", code);
    p.token[0] = 0;
    port_pair_ensure_host_edit(&p);
    if (!p.talk_hold && !p.talk_toggle) p.talk_hold = 1;
    if (p.mood_poll_s < 10) p.mood_poll_s = 30;
    if (p.max_ask_s <= 0) p.max_ask_s = 15;
    port_pair_save(&p);
    return code;
}

bool port_pair_poll_claim(void)
{
    if (pebble_net_claim_need_new_code()) {
        port_pair_new_claim();
    }
    if (pebble_net_claim_just_paired()) return true;
    if (!pebble_net_claim_busy()) pebble_net_claim_kick();
    return false;
}

bool port_talk_toggle(void)
{
    pebble_pair_t p;
    if (!port_pair_load(&p)) return false;
    return p.talk_toggle != 0;
}

bool port_talk_hold(void)
{
    /* Default UX is hold-to-talk; toggle mode replaces it when Sidekick enables it. */
    return !port_talk_toggle();
}

bool port_talk_wake(void)
{
    pebble_pair_t p;
    if (!port_pair_load(&p)) return false;
    return p.talk_wake != 0;
}

void port_talk_wake_phrase(char *out, size_t n)
{
    if (!out || !n) return;
    pebble_pair_t p;
    if (!port_pair_load(&p) || !p.wake_phrase[0]) {
        snprintf(out, n, "hey stonepi");
        return;
    }
    snprintf(out, n, "%s", p.wake_phrase);
}

