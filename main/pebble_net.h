#pragma once
#include "port.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PEBBLE_FW_VERSION "0.1.0"
#define PEBBLE_MODEL      "waveshare-amoled-175c"
/* Empty: user must enter the Pi LAN URL (http://<Pi-IP>). No :8015 on the
 * appliance (nginx :80). Dev-only :8015 is for PC run-dev AgentLink. */
#define PEBBLE_DEFAULT_HOST ""
#define PEBBLE_BASE_PATH  "/agent/pebble/v1"

void pebble_net_start(void);

/* pbl- + Wi-Fi MAC (lowercase hex, no colons). */
void port_device_id(char *out, size_t n);

/* Ensure host_edit has a default; load/save helpers for unpaired host. */
void port_pair_ensure_host_edit(pebble_pair_t *p);
void port_pair_set_host_edit(const char *host);

/* Apply GET /config (or claim.config) into pair blob. */
void port_pair_apply_config_json(pebble_pair_t *p, const char *json_obj);

/* ── Claim (async; never call from LVGL with blocking HTTP) ─── */
void pebble_net_claim_kick(void);          /* start one poll if idle */
bool pebble_net_claim_busy(void);
bool pebble_net_claim_just_paired(void);   /* latch; clears on read */
bool pebble_net_claim_need_new_code(void); /* 409/410 latch; clears on read */
int  pebble_net_claim_poll_after_s(void);  /* from last 202; default 3 */
/* Last claim error for pairing UI; latch, clears on read. */
bool pebble_net_claim_take_status(char *msg, size_t msg_n);

/* ── Unpair ─────────────────────────────────────────────────── */
void pebble_net_unpair(void); /* POST /unpair then clear local (always) */

/* ── Ask ────────────────────────────────────────────────────── */
typedef struct {
    bool     ready;
    bool     ok;
    char     error[32];
    char     message[64];
    char     transcript[96];
    char     reply[96];
    char     mood[16];
    int16_t *pcm;       /* 22050 Hz mono reply samples (malloc); caller frees */
    size_t   nsamp;
} pebble_ask_result_t;

/* pcm_16k = mono int16 @ 16 kHz. Returns false if busy / not paired. */
bool pebble_net_ask_start(const int16_t *pcm_16k, size_t nsamp);
bool pebble_net_ask_busy(void);
/* If ready, fills *out (transfers pcm ownership). Returns true when ready. */
bool pebble_net_ask_take(pebble_ask_result_t *out);

/* ── Mood / config poll (idle) ──────────────────────────────── */
void pebble_net_mood_kick(void);
bool pebble_net_mood_busy(void);
/* Latest base mood from Pi (not listening/thinking/speaking). */
void pebble_net_base_mood(char *mood, size_t mood_n, char *caption, size_t cap_n);
int  pebble_net_mood_poll_s(void);

/* Soft 403 notice (away_disabled / owner_disabled) — pairing kept. */
bool pebble_net_take_notice(char *msg, size_t msg_n);

/* Percent-decode into dst (NUL-terminated). */
void pebble_pct_decode(char *dst, size_t dst_n, const char *src);
