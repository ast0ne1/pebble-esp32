/* PC half of port.h — same UI as the board, no ESP-IDF. */
#include "port.h"
#include "pebble_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

uint32_t g_sim_us;

void port_lock(void) {}
void port_unlock(void) {}

void port_log(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag ? tag : "sim");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void kv_dir(char *out, size_t n)
{
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (!base) base = ".";
    snprintf(out, n, "%s\\pebble_sim_kv", base);
#else
    snprintf(out, n, "/tmp/pebble_sim_kv");
#endif
}

static void kv_path(const char *key, char *out, size_t n)
{
    char dir[256];
    kv_dir(dir, sizeof(dir));
    MKDIR(dir);
#ifdef _WIN32
    snprintf(out, n, "%s\\%s.bin", dir, key);
#else
    snprintf(out, n, "%s/%s.bin", dir, key);
#endif
}

bool port_kv_read(const char *key, void *out, size_t len)
{
    char path[320];
    kv_path(key, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(out, 1, len, f);
    fclose(f);
    return got == len;
}

void port_kv_write(const char *key, const void *in, size_t len)
{
    char path[320];
    kv_path(key, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(in, 1, len, f);
    fclose(f);
}

void port_kv_erase(const char *key)
{
    char path[320];
    kv_path(key, path, sizeof(path));
    remove(path);
}

uint32_t port_millis(void) { return g_sim_us / 1000u; }

void port_delay_ms(uint32_t ms) { g_sim_us += ms * 1000u; }

void port_power_off(void) { fprintf(stderr, "[power] off (sim)\n"); }

int port_battery_percent(void) { return 62; }
bool port_battery_charging(void) { return false; }
bool port_battery_plugged(void) { return false; }

static int s_bright = 45;
static bool s_disp_on = true;

void port_brightness_set(int percent)
{
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    s_bright = percent;
}

int  port_brightness_get(void) { return s_bright; }
void port_pm_hold(bool on) { (void)on; }
void port_display_on(bool on) { s_disp_on = on; }
bool port_display_is_on(void) { return s_disp_on; }

/* Fake Wi-Fi — scan/join mirror the device UI path. */
static bool s_wifi = true;
static int  s_scan_ticks;
static char s_ssid[33] = "StonePi-LAN";
static char s_ip[24] = "192.168.1.42";
static int  s_try_state = WIFI_TRY_OK;

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

static const struct {
    const char *ssid;
    int8_t rssi;
    uint8_t open;
} s_fake_aps[] = {
    { "StonePi-LAN", -42, 0 },
    { "Cafe Guest",  -61, 1 },
    { "IoT-Lab",     -55, 0 },
    { "Printer",     -78, 0 },
};

void port_wifi_scan_start(void) { s_scan_ticks = 2; }

int port_wifi_scan_result(wifi_found_t *out, int max)
{
    if (s_scan_ticks > 0) {
        s_scan_ticks--;
        if (s_scan_ticks > 0) return WIFI_SCAN_RUNNING;
    }
    if (!out || max <= 0) return 0;

    wifi_cred_t cred;
    memset(&cred, 0, sizeof(cred));
    (void)port_kv_read("wificfg", &cred, sizeof(cred));

    int n = (int)(sizeof(s_fake_aps) / sizeof(s_fake_aps[0]));
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].ssid, s_fake_aps[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = 0;
        out[i].rssi = s_fake_aps[i].rssi;
        out[i].open = s_fake_aps[i].open;
        out[i].saved = (cred.ssid[0] && strcmp(cred.ssid, out[i].ssid) == 0) ? 1 : 0;
    }
    return n;
}

void port_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = 0;
    snprintf(s_ip, sizeof(s_ip), "192.168.1.%u", 40u + (unsigned)(ssid[0] & 15));
    s_wifi = true;
    s_try_state = WIFI_TRY_OK;

    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    strncpy(c.ssid, ssid, sizeof(c.ssid) - 1);
    if (pass) strncpy(c.pass, pass, sizeof(c.pass) - 1);
    port_kv_write("wificfg", &c, sizeof(c));
    (void)pass;
}

void port_wifi_boot_connect(void)
{
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    if (!port_kv_read("wificfg", &c, sizeof(c)) || !c.ssid[0]) return;
    port_wifi_connect(c.ssid, c.pass);
}

int  port_wifi_try_state(void) { return s_try_state; }
bool port_wifi_connected(void) { return s_wifi; }

void port_wifi_ssid(char *out, size_t n)
{
    if (!out || !n) return;
    strncpy(out, s_ssid, n - 1);
    out[n - 1] = 0;
}

void port_wifi_ip(char *out, size_t n)
{
    if (!out || !n) return;
    strncpy(out, s_ip, n - 1);
    out[n - 1] = 0;
}

/* Pairing — same NVS blob shape; claim poll is a no-op on sim. */
static void make_claim(char *out, size_t n)
{
    static const char alphabet[] = "ACDEFHJKMNPRTUVWXY34679";
    const size_t alen = sizeof(alphabet) - 1;
    if (n < 5) {
        if (n) out[0] = 0;
        return;
    }
    unsigned r = (unsigned)time(NULL) ^ (unsigned)(g_sim_us * 2654435761u);
    for (int i = 0; i < 4; i++) out[i] = alphabet[(r >> (i * 3)) % alen];
    out[4] = 0;
}

bool port_pair_load(pebble_pair_t *out)
{
    if (!out) return false;
    if (!port_kv_read("pair", out, sizeof(*out))) return false;
    if (!out->token[0] && out->host_edit[0] &&
        (strstr(out->host_edit, ".local") || strstr(out->host_edit, ".LOCAL"))) {
        out->host_edit[0] = 0;
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
        strncpy(code, p.claim, sizeof(code) - 1);
        return code;
    }
    make_claim(code, sizeof(code));
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    strncpy(p.claim, code, sizeof(p.claim) - 1);
    p.token[0] = 0;
    port_pair_ensure_host_edit(&p);
    port_pair_save(&p);
    return code;
}

bool port_pair_poll_claim(void) { return false; }

bool port_talk_toggle(void)
{
    pebble_pair_t p;
    if (!port_pair_load(&p)) return false;
    return p.talk_toggle != 0;
}

bool port_talk_hold(void)
{
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

/* Debug helpers for server / tests */
void sim_wifi_set(bool on) { s_wifi = on; }
