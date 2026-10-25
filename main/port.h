#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Thin board seam — UI/apps call only this (and LVGL). Mirrors The-Badge's
 * port.h idea so a PC sim can share shell code later. */

void port_lock(void);
void port_unlock(void);
void port_log(const char *tag, const char *fmt, ...);

bool port_kv_read(const char *key, void *out, size_t len);
void port_kv_write(const char *key, const void *in, size_t len);
void port_kv_erase(const char *key);

uint32_t port_millis(void);
void     port_delay_ms(uint32_t ms);

/* POSIX TZ string (e.g. "GMT0BST,M3.5.0/1,M10.5.0"). Empty → UTC. */
void port_tz_set(const char *posix_tz);

void port_power_off(void);
int  port_battery_percent(void);   /* -1 if unknown / no pack */
bool port_battery_charging(void);
bool port_battery_plugged(void);

void port_brightness_set(int percent);
int  port_brightness_get(void);

/* Hold light sleep off during Wi-Fi / HTTP / audio. Nested. */
void port_pm_hold(bool on);

/* Display fully off/on (panel sleep), not just brightness 0. */
void port_display_on(bool on);
bool port_display_is_on(void);

/* ── Wi-Fi (settings + pairing need STA) ─────────────────────── */
#define WIFI_SCAN_MAX 16
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t saved;   /* 1 if matches NVS credentials */
    uint8_t open;    /* 1 if no password required */
} wifi_found_t;

void port_wifi_scan_start(void);
#define WIFI_SCAN_RUNNING  (-1)
#define WIFI_SCAN_NO_RADIO (-2)
int  port_wifi_scan_result(wifi_found_t *out, int max);

void port_wifi_connect(const char *ssid, const char *pass);
#define WIFI_TRY_BUSY (-1)
#define WIFI_TRY_FAIL 0
#define WIFI_TRY_OK   1
int  port_wifi_try_state(void);
bool port_wifi_connected(void);
void port_wifi_ssid(char *out, size_t n);   /* empty if none */
void port_wifi_ip(char *out, size_t n);
/* True if NVS has a password (may be empty) for this SSID. */
bool port_wifi_saved_pass(const char *ssid, char *pass_out, size_t pass_n);
/* After boot: if NVS has credentials, start STA join once. */
void port_wifi_boot_connect(void);

/* ── Pairing state (NVS) — PROTOCOL.md v1 ─────────────────────── */
typedef struct {
    char    token[48];          /* pbt_… */
    char    host_primary[96];   /* config.hosts.lan */
    char    host_failover[96];  /* config.hosts.away (empty if null) */
    char    nickname[32];
    char    claim[8];           /* short code while unpaired */
    char    host_edit[96];      /* user-typed poll host while unpaired */
    char    wake_phrase[40];
    int32_t config_rev;
    int32_t mood_poll_s;
    int32_t max_ask_s;
    uint8_t talk_hold;
    uint8_t talk_toggle;
    uint8_t talk_wake;
    uint8_t last_host;          /* 0=lan, 1=away */
    uint8_t _pad[8];
} pebble_pair_t;

bool port_pair_load(pebble_pair_t *out);
void port_pair_save(const pebble_pair_t *in);
void port_pair_clear(void);
bool port_pair_is_paired(void);
/* Fresh claim code into NVS if unpaired; returns pointer into static buffer. */
const char *port_pair_claim_code(void);
/* Rotate claim code (409/410). */
void port_pair_new_claim(void);

/* Kick async claim poll; returns true when newly paired (latched). */
bool port_pair_poll_claim(void);

/* Talk mode from last config (defaults: hold on, toggle/wake off). */
bool port_talk_hold(void);
bool port_talk_toggle(void);
bool port_talk_wake(void);
void port_talk_wake_phrase(char *out, size_t n);
