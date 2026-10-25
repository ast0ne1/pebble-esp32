/* Pebble ↔ StonePi PROTOCOL.md v1 HTTP client (worker task). */
#include "pebble_net.h"
#include "app.h"
#include "wake.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PEBBLE_SIM

void pebble_net_start(void) {}
void port_device_id(char *out, size_t n)
{
    if (out && n) snprintf(out, n, "pbl-sim00000001");
}
void port_pair_ensure_host_edit(pebble_pair_t *p)
{
    if (!p) return;
    if (!p->host_edit[0] && PEBBLE_DEFAULT_HOST[0])
        snprintf(p->host_edit, sizeof(p->host_edit), "%s", PEBBLE_DEFAULT_HOST);
}
void port_pair_set_host_edit(const char *host)
{
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    port_pair_load(&p);
    if (host && host[0])
        snprintf(p.host_edit, sizeof(p.host_edit), "%s", host);
    else if (PEBBLE_DEFAULT_HOST[0])
        snprintf(p.host_edit, sizeof(p.host_edit), "%s", PEBBLE_DEFAULT_HOST);
    else
        p.host_edit[0] = 0;
    port_pair_save(&p);
}
void port_pair_apply_config_json(pebble_pair_t *p, const char *json_obj)
{
    (void)p;
    (void)json_obj;
    wake_on_config_changed();
}
void pebble_net_claim_kick(void) {}
bool pebble_net_claim_busy(void) { return false; }
bool pebble_net_claim_just_paired(void) { return false; }
bool pebble_net_claim_need_new_code(void) { return false; }
int  pebble_net_claim_poll_after_s(void) { return 3; }
bool pebble_net_claim_take_status(char *msg, size_t msg_n)
{
    if (msg && msg_n) msg[0] = 0;
    return false;
}
void pebble_net_unpair(void) { port_pair_clear(); }
bool pebble_net_ask_start(const int16_t *pcm_16k, size_t nsamp)
{
    (void)pcm_16k;
    (void)nsamp;
    return false;
}
bool pebble_net_ask_busy(void) { return false; }
bool pebble_net_ask_take(pebble_ask_result_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    return false;
}
void pebble_net_mood_kick(void) {}
bool pebble_net_mood_busy(void) { return false; }
void pebble_net_base_mood(char *mood, size_t mood_n, char *caption, size_t cap_n)
{
    if (mood && mood_n) snprintf(mood, mood_n, "content");
    if (caption && cap_n) caption[0] = 0;
}
int pebble_net_mood_poll_s(void) { return 30; }
bool pebble_net_take_notice(char *msg, size_t msg_n)
{
    if (msg && msg_n) msg[0] = 0;
    return false;
}
void pebble_pct_decode(char *dst, size_t dst_n, const char *src)
{
    if (!dst || !dst_n) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_n; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
}

#else /* device */

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pebble_net";

enum {
    JOB_CLAIM = 1,
    JOB_UNPAIR,
    JOB_ASK,
    JOB_MOOD,
};

typedef struct {
    int      kind;
    int16_t *ask_pcm;
    size_t   ask_nsamp;
} net_job_t;

static QueueHandle_t     s_q;
static SemaphoreHandle_t s_lock;
static volatile bool     s_claim_busy;
static volatile bool     s_ask_busy;
static volatile bool     s_mood_busy;
static volatile bool     s_just_paired;
static volatile bool     s_need_new_code;
static volatile int      s_poll_after_s = 3;
static char              s_base_mood[16] = "content";
static char              s_base_caption[48];
static char              s_notice[64];
static bool              s_notice_pending;
static char              s_claim_status[80];
static bool              s_claim_status_pending;
static pebble_ask_result_t s_ask_res;
static bool              s_ask_ready;

void pebble_pct_decode(char *dst, size_t dst_n, const char *src)
{
    if (!dst || !dst_n) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_n; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
}

void port_device_id(char *out, size_t n)
{
    if (!out || !n) return;
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, n, "pbl-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void port_pair_ensure_host_edit(pebble_pair_t *p)
{
    if (!p) return;
    if (!p->host_edit[0] && PEBBLE_DEFAULT_HOST[0])
        snprintf(p->host_edit, sizeof(p->host_edit), "%s", PEBBLE_DEFAULT_HOST);
}

/* Ensure http(s)://, collapse accidental ".." (e.g. typed .225 after …0.), trim /. */
static void normalize_host_edit(char *host, size_t n)
{
    if (!host || n < 8) return;
    size_t L = strlen(host);
    while (L && (host[L - 1] == ' ' || host[L - 1] == '\n' || host[L - 1] == '/'))
        host[--L] = 0;
    size_t lead = 0;
    while (host[lead] == ' ') lead++;
    if (lead) {
        memmove(host, host + lead, L - lead + 1);
        L -= lead;
    }
    if (!L) return;

    if (strncmp(host, "http://", 7) != 0 && strncmp(host, "https://", 8) != 0) {
        char tmp[96];
        snprintf(tmp, sizeof(tmp), "http://%s", host);
        snprintf(host, n, "%s", tmp);
        L = strlen(host);
    }

    char *auth = strstr(host, "://");
    char *p = auth ? auth + 3 : host;
    for (;;) {
        char *dd = strstr(p, "..");
        if (!dd) break;
        memmove(dd, dd + 1, strlen(dd));
    }
}

/* Device has no mDNS — .local LAN hosts from AgentLink are unusable. */
static bool host_reachable_hint(const char *h)
{
    if (!h || !h[0]) return false;
    if (strstr(h, ".local") || strstr(h, ".LOCAL")) return false;
    return true;
}

/* Keep a working IP from Pairing when config.hosts.lan is empty/.local. */
static void repair_pair_hosts(pebble_pair_t *p)
{
    if (!p) return;
    normalize_host_edit(p->host_edit, sizeof(p->host_edit));
    normalize_host_edit(p->host_primary, sizeof(p->host_primary));
    normalize_host_edit(p->host_failover, sizeof(p->host_failover));

    if (!host_reachable_hint(p->host_primary)) {
        p->host_primary[0] = 0;
        if (host_reachable_hint(p->host_edit))
            snprintf(p->host_primary, sizeof(p->host_primary), "%s", p->host_edit);
    }
    /* Keep host_edit as a durable LAN fallback across reboots. */
    if (host_reachable_hint(p->host_primary) && !p->host_edit[0])
        snprintf(p->host_edit, sizeof(p->host_edit), "%s", p->host_primary);
}

void port_pair_set_host_edit(const char *host)
{
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    if (host && host[0])
        snprintf(p.host_edit, sizeof(p.host_edit), "%s", host);
    else if (PEBBLE_DEFAULT_HOST[0])
        snprintf(p.host_edit, sizeof(p.host_edit), "%s", PEBBLE_DEFAULT_HOST);
    else
        p.host_edit[0] = 0;
    normalize_host_edit(p.host_edit, sizeof(p.host_edit));
    port_pair_save(&p);
}

static void claim_status_set(const char *msg)
{
    if (!msg) return;
    if (!s_lock) {
        snprintf(s_claim_status, sizeof(s_claim_status), "%s", msg);
        s_claim_status_pending = true;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_claim_status, sizeof(s_claim_status), "%s", msg);
    s_claim_status_pending = true;
    xSemaphoreGive(s_lock);
}

static void copy_str(char *dst, size_t n, cJSON *j)
{
    if (!dst || !n) return;
    dst[0] = 0;
    if (!j || !cJSON_IsString(j) || !j->valuestring) return;
    snprintf(dst, n, "%s", j->valuestring);
}

void port_pair_apply_config_json(pebble_pair_t *p, const char *json_obj)
{
    if (!p || !json_obj) return;
    cJSON *root = cJSON_Parse(json_obj);
    if (!root) return;
    cJSON *rev = cJSON_GetObjectItem(root, "rev");
    if (cJSON_IsNumber(rev)) p->config_rev = rev->valueint;
    copy_str(p->nickname, sizeof(p->nickname), cJSON_GetObjectItem(root, "nickname"));
    cJSON *hosts = cJSON_GetObjectItem(root, "hosts");
    if (hosts) {
        cJSON *lan = cJSON_GetObjectItem(hosts, "lan");
        cJSON *away = cJSON_GetObjectItem(hosts, "away");
        char new_lan[96];
        copy_str(new_lan, sizeof(new_lan), lan);
        normalize_host_edit(new_lan, sizeof(new_lan));
        /* Never replace a working IP with stonepi.local (no mDNS on device). */
        if (host_reachable_hint(new_lan))
            snprintf(p->host_primary, sizeof(p->host_primary), "%s", new_lan);
        if (cJSON_IsString(away) && away->valuestring) {
            snprintf(p->host_failover, sizeof(p->host_failover), "%s", away->valuestring);
            normalize_host_edit(p->host_failover, sizeof(p->host_failover));
        } else {
            p->host_failover[0] = 0;
        }
    }
    cJSON *talk = cJSON_GetObjectItem(root, "talk");
    if (talk) {
        cJSON *h = cJSON_GetObjectItem(talk, "hold");
        cJSON *t = cJSON_GetObjectItem(talk, "toggle");
        cJSON *w = cJSON_GetObjectItem(talk, "wake");
        p->talk_hold = cJSON_IsTrue(h) ? 1 : (cJSON_IsFalse(h) ? 0 : 1);
        p->talk_toggle = cJSON_IsTrue(t) ? 1 : 0;
        p->talk_wake = cJSON_IsTrue(w) ? 1 : 0;
        copy_str(p->wake_phrase, sizeof(p->wake_phrase),
                 cJSON_GetObjectItem(talk, "wake_phrase"));
    }
    cJSON *mps = cJSON_GetObjectItem(root, "mood_poll_s");
    if (cJSON_IsNumber(mps)) p->mood_poll_s = mps->valueint;
    if (p->mood_poll_s < 10) p->mood_poll_s = 30;
    cJSON *mas = cJSON_GetObjectItem(root, "max_ask_s");
    if (cJSON_IsNumber(mas)) p->max_ask_s = mas->valueint;
    if (p->max_ask_s <= 0) p->max_ask_s = 15;
    cJSON_Delete(root);
    repair_pair_hosts(p);
    wake_on_config_changed();
}

/* ── HTTP core ──────────────────────────────────────────────── */

typedef struct {
    char    *buf;
    size_t   len;
    size_t   cap;
    int      status;
    char     hdr_transcript[220];
    char     hdr_reply[220];
    char     hdr_mood[32];
} http_accum_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_accum_t *a = (http_accum_t *)evt->user_data;
    if (!a) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value) {
            if (strcasecmp(evt->header_key, "X-Pebble-Transcript") == 0)
                snprintf(a->hdr_transcript, sizeof(a->hdr_transcript), "%s", evt->header_value);
            else if (strcasecmp(evt->header_key, "X-Pebble-Reply") == 0)
                snprintf(a->hdr_reply, sizeof(a->hdr_reply), "%s", evt->header_value);
            else if (strcasecmp(evt->header_key, "X-Pebble-Mood") == 0)
                snprintf(a->hdr_mood, sizeof(a->hdr_mood), "%s", evt->header_value);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t need = a->len + (size_t)evt->data_len + 1;
        if (need > a->cap) {
            size_t ncap = a->cap ? a->cap * 2 : 4096;
            while (ncap < need) ncap *= 2;
            if (ncap > 600 * 1024) ncap = 600 * 1024;
            if (need > ncap) return ESP_FAIL;
            char *nb = realloc(a->buf, ncap);
            if (!nb) return ESP_FAIL;
            a->buf = nb;
            a->cap = ncap;
        }
        memcpy(a->buf + a->len, evt->data, (size_t)evt->data_len);
        a->len += (size_t)evt->data_len;
        a->buf[a->len] = 0;
    }
    return ESP_OK;
}

typedef struct {
    int      status;
    char     error[32];
    char     message[64];
    int      retry_after_s;
    char    *body;
    size_t   body_len;
    char     hdr_transcript[220];
    char     hdr_reply[220];
    char     hdr_mood[32];
    bool     transport_fail;
} http_result_t;

static void parse_error_body(http_result_t *r)
{
    if (!r->body || !r->body_len) return;
    cJSON *j = cJSON_Parse(r->body);
    if (!j) return;
    copy_str(r->error, sizeof(r->error), cJSON_GetObjectItem(j, "error"));
    copy_str(r->message, sizeof(r->message), cJSON_GetObjectItem(j, "message"));
    cJSON *ra = cJSON_GetObjectItem(j, "retry_after_s");
    if (cJSON_IsNumber(ra)) r->retry_after_s = ra->valueint;
    cJSON_Delete(j);
}

static void http_result_free(http_result_t *r)
{
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

static bool host_is_https(const char *host)
{
    return host && strncmp(host, "https://", 8) == 0;
}

static int connect_timeout_ms(const char *host, bool is_away)
{
    (void)host;
    return is_away ? 5000 : 1500;
}

/* One attempt against base host URL (no path). path starts with /. */
static void http_once(const char *host, bool is_away, const char *method, const char *path,
                      const char *bearer, const void *body, size_t body_len, const char *ctype,
                      int read_timeout_ms, http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!host || !host[0] || !path) {
        out->transport_fail = true;
        snprintf(out->error, sizeof(out->error), "bad_request");
        snprintf(out->message, sizeof(out->message), "No host");
        return;
    }

    char url[256];
    size_t hlen = strlen(host);
    while (hlen && host[hlen - 1] == '/') hlen--;
    if (hlen + strlen(path) + 1 >= sizeof(url)) {
        out->transport_fail = true;
        return;
    }
    snprintf(url, sizeof(url), "%.*s%s%s", (int)hlen, host, PEBBLE_BASE_PATH, path);

    char device_id[24];
    port_device_id(device_id, sizeof(device_id));
    char batt[8] = "";
    int bp = port_battery_percent();
    if (bp >= 0) snprintf(batt, sizeof(batt), "%d", bp);

    http_accum_t acc = {0};
    int t_connect = connect_timeout_ms(host, is_away);
    int t_overall = read_timeout_ms;
    /* Overall budget ≥ connect; LAN stays snappy so failover can run. */
    if (t_overall < t_connect + 500) t_overall = t_connect + 500;
    if (!is_away && t_overall > 4000) t_overall = 4000; /* lan: fail fast, try next host */
    if (read_timeout_ms >= 40000) t_overall = read_timeout_ms; /* ask keeps long read */

    esp_http_client_config_t cfg = {
        .url = url,
        .method = (strcmp(method, "GET") == 0) ? HTTP_METHOD_GET : HTTP_METHOD_POST,
        .timeout_ms = t_overall,
        .event_handler = http_evt,
        .user_data = &acc,
        .keep_alive_enable = false,
    };
    if (host_is_https(host)) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    port_pm_hold(true);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        port_pm_hold(false);
        out->transport_fail = true;
        return;
    }

    esp_http_client_set_header(client, "X-Pebble-Device", device_id);
    esp_http_client_set_header(client, "X-Pebble-Fw", PEBBLE_FW_VERSION);
    if (batt[0]) esp_http_client_set_header(client, "X-Pebble-Battery", batt);
    if (bearer && bearer[0]) {
        char auth[80];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (ctype) esp_http_client_set_header(client, "Content-Type", ctype);
    if (body && body_len)
        esp_http_client_set_post_field(client, (const char *)body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    out->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    port_pm_hold(false);

    out->body = acc.buf;
    out->body_len = acc.len;
    acc.buf = NULL;
    memcpy(out->hdr_transcript, acc.hdr_transcript, sizeof(out->hdr_transcript));
    memcpy(out->hdr_reply, acc.hdr_reply, sizeof(out->hdr_reply));
    memcpy(out->hdr_mood, acc.hdr_mood, sizeof(out->hdr_mood));

    if (err != ESP_OK) {
        out->transport_fail = true;
        out->status = 0;
        snprintf(out->error, sizeof(out->error), "internal");
        snprintf(out->message, sizeof(out->message), "Network error");
        return;
    }
    if (out->status == 502 || out->status == 504) {
        out->transport_fail = true;
        return;
    }
    if (out->status < 200 || out->status >= 300)
        parse_error_body(out);
}

static void handle_401(http_result_t *r)
{
    if (r->status == 401) {
        port_log(TAG, "401 — clear pairing");
        port_pair_clear();
        s_base_mood[0] = 0;
        snprintf(s_base_mood, sizeof(s_base_mood), "content");
        s_base_caption[0] = 0;
    }
}

static void handle_403(http_result_t *r)
{
    if (r->status != 403) return;
    /* Keep pairing for away_disabled and owner_disabled. */
    if (r->message[0] && s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_notice, sizeof(s_notice), "%s", r->message);
        s_notice_pending = true;
        xSemaphoreGive(s_lock);
    }
    if (strcmp(r->error, "away_disabled") == 0) {
        pebble_pair_t p;
        memset(&p, 0, sizeof(p));
        if (port_pair_load(&p)) {
            p.last_host = 0; /* prefer lan next */
            port_pair_save(&p);
        }
        port_log(TAG, "403 away_disabled — keep pair, prefer lan");
    } else if (strcmp(r->error, "owner_disabled") == 0) {
        port_log(TAG, "403 owner_disabled — keep pair");
    } else {
        port_log(TAG, "403 %s — keep pair", r->error[0] ? r->error : "?");
    }
}

static void handle_status(http_result_t *r)
{
    handle_401(r);
    handle_403(r);
}

/* Try last-good host, then LAN fallbacks, then away — once each on transport/502/504. */
static void http_with_failover(const char *method, const char *path, bool use_auth,
                               const void *body, size_t body_len, const char *ctype,
                               int read_timeout_ms, bool claim_mode, http_result_t *out)
{
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    port_pair_ensure_host_edit(&p);
    {
        char pri_before[96], edit_before[96];
        snprintf(pri_before, sizeof(pri_before), "%s", p.host_primary);
        snprintf(edit_before, sizeof(edit_before), "%s", p.host_edit);
        repair_pair_hosts(&p);
        if (strcmp(pri_before, p.host_primary) != 0 || strcmp(edit_before, p.host_edit) != 0)
            port_pair_save(&p);
    }

    const char *candidates[3];
    bool away_flag[3];
    int n = 0;

    if (claim_mode) {
        if (p.host_edit[0]) {
            candidates[n] = p.host_edit;
            away_flag[n++] = false;
        }
    } else {
        /* Prefer last successful path, but always keep host_edit as LAN backup.
         * AgentLink often sets hosts.lan to *.local which this board cannot resolve. */
        if (p.last_host == 1 && p.host_failover[0]) {
            candidates[n] = p.host_failover;
            away_flag[n++] = true;
        }
        if (host_reachable_hint(p.host_primary)) {
            candidates[n] = p.host_primary;
            away_flag[n++] = false;
        }
        if (host_reachable_hint(p.host_edit) &&
            (!host_reachable_hint(p.host_primary) ||
             strcmp(p.host_edit, p.host_primary) != 0)) {
            candidates[n] = p.host_edit;
            away_flag[n++] = false;
        }
        if (p.last_host != 1 && p.host_failover[0]) {
            candidates[n] = p.host_failover;
            away_flag[n++] = true;
        }
        /* Cap at 3 */
        if (n > 3) n = 3;
    }

    const char *bearer = (use_auth && p.token[0]) ? p.token : NULL;
    if (n == 0) {
        memset(out, 0, sizeof(*out));
        out->transport_fail = true;
        snprintf(out->error, sizeof(out->error), "bad_request");
        snprintf(out->message, sizeof(out->message), "No host");
        return;
    }

    for (int i = 0; i < n; i++) {
        if (i > 0) http_result_free(out);
        http_once(candidates[i], away_flag[i], method, path, bearer, body, body_len, ctype,
                  read_timeout_ms, out);
        if (!out->transport_fail) {
            handle_status(out);
            if (!claim_mode && out->status >= 200 && out->status < 300) {
                char used[96];
                snprintf(used, sizeof(used), "%s", candidates[i] ? candidates[i] : "");
                memset(&p, 0, sizeof(p));
                port_pair_load(&p);
                p.last_host = away_flag[i] ? 1 : 0;
                /* Remember which LAN URL worked. */
                if (!away_flag[i] && used[0]) {
                    if (strcmp(p.host_primary, used) != 0)
                        snprintf(p.host_primary, sizeof(p.host_primary), "%s", used);
                    if (!p.host_edit[0])
                        snprintf(p.host_edit, sizeof(p.host_edit), "%s", used);
                }
                port_pair_save(&p);
                port_log(TAG, "%s ok via %s", path, used);
            }
            return;
        }
        port_log(TAG, "%s fail via %s — try next", path,
                 candidates[i] ? candidates[i] : "?");
    }
}

static void do_claim(void)
{
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    if (!port_pair_load(&p) || !p.claim[0] || p.token[0]) {
        s_claim_busy = false;
        return;
    }
    port_pair_ensure_host_edit(&p);
    if (!p.host_edit[0]) {
        claim_status_set("Set StonePi host first");
        s_claim_busy = false;
        return;
    }

    char device_id[24];
    port_device_id(device_id, sizeof(device_id));
    char body[192];
    snprintf(body, sizeof(body),
             "{\"code\":\"%s\",\"device_id\":\"%s\",\"fw\":\"%s\",\"model\":\"%s\"}",
             p.claim, device_id, PEBBLE_FW_VERSION, PEBBLE_MODEL);

    http_result_t r;
    http_with_failover("POST", "/claim/poll", false, body, strlen(body), "application/json",
                       10000, true, &r);

    if (r.transport_fail) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Can't reach %s", p.host_edit);
        claim_status_set(msg);
        port_log(TAG, "claim transport fail host=%s", p.host_edit);
    } else if (r.status == 202 && r.body) {
        cJSON *j = cJSON_Parse(r.body);
        if (j) {
            cJSON *pa = cJSON_GetObjectItem(j, "poll_after_s");
            if (cJSON_IsNumber(pa) && pa->valueint >= 1) s_poll_after_s = pa->valueint;
            else s_poll_after_s = 3;
            cJSON_Delete(j);
        }
        claim_status_set("Waiting...");
    } else if (r.status == 200 && r.body) {
        cJSON *j = cJSON_Parse(r.body);
        if (j) {
            cJSON *tok = cJSON_GetObjectItem(j, "token");
            cJSON *cfg = cJSON_GetObjectItem(j, "config");
            if (cJSON_IsString(tok) && tok->valuestring) {
                char edit_keep[96];
                memset(&p, 0, sizeof(p));
                port_pair_load(&p);
                snprintf(edit_keep, sizeof(edit_keep), "%s", p.host_edit);
                snprintf(p.token, sizeof(p.token), "%s", tok->valuestring);
                p.claim[0] = 0;
                if (cfg) {
                    char *cs = cJSON_PrintUnformatted(cfg);
                    if (cs) {
                        port_pair_apply_config_json(&p, cs);
                        free(cs);
                    }
                }
                /* Restore typed LAN IP if config only gave *.local / empty. */
                if (edit_keep[0])
                    snprintf(p.host_edit, sizeof(p.host_edit), "%s", edit_keep);
                repair_pair_hosts(&p);
                port_pair_save(&p);
                s_just_paired = true;
                port_log(TAG, "claimed — paired lan=%s edit=%s",
                         p.host_primary, p.host_edit);
            }
            cJSON_Delete(j);
        }
    } else if (r.status == 409 || r.status == 410) {
        s_need_new_code = true;
        claim_status_set(r.message[0] ? r.message : "New code — try again");
    } else if (r.status == 429) {
        claim_status_set("Too many codes, wait");
        if (r.retry_after_s > 0) s_poll_after_s = r.retry_after_s;
        else s_poll_after_s = 5;
    } else if (r.status == 400) {
        claim_status_set("Code rejected");
    } else if (r.status == 404) {
        /* Reachable host, wrong/missing /agent/pebble/v1 (nginx or AgentLink). */
        claim_status_set("No Pebble API (404)");
        port_log(TAG, "claim 404 host=%s", p.host_edit);
    } else if (r.message[0]) {
        claim_status_set(r.message);
    } else if (r.status > 0) {
        claim_status_set("Code rejected");
    }
    http_result_free(&r);
    s_claim_busy = false;
}

static char s_unpair_token[48];
static char s_unpair_host[96];

static void do_unpair(void)
{
    /* Local pair already cleared for UI; POST with snapped creds. */
    if (s_unpair_token[0] && s_unpair_host[0]) {
        http_result_t r;
        http_once(s_unpair_host, false, "POST", "/unpair", s_unpair_token, "{}", 2,
                  "application/json", 10000, &r);
        http_result_free(&r);
    }
    s_unpair_token[0] = 0;
    s_unpair_host[0] = 0;
}

static int16_t *wav_to_pcm22050(const uint8_t *wav, size_t len, size_t *nsamp_out)
{
    *nsamp_out = 0;
    if (!wav || len < 44) return NULL;
    /* Minimal RIFF parse: find 'data' chunk. */
    if (memcmp(wav, "RIFF", 4) != 0) return NULL;
    size_t pos = 12;
    uint16_t channels = 1, bps = 16;
    uint32_t rate = 22050;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    while (pos + 8 <= len) {
        uint32_t id = (uint32_t)wav[pos] | ((uint32_t)wav[pos + 1] << 8) |
                      ((uint32_t)wav[pos + 2] << 16) | ((uint32_t)wav[pos + 3] << 24);
        uint32_t sz = (uint32_t)wav[pos + 4] | ((uint32_t)wav[pos + 5] << 8) |
                      ((uint32_t)wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
        pos += 8;
        if (id == 0x20746d66 /* fmt */) {
            if (pos + 16 <= len) {
                channels = (uint16_t)(wav[pos + 2] | (wav[pos + 3] << 8));
                rate = (uint32_t)wav[pos + 4] | ((uint32_t)wav[pos + 5] << 8) |
                       ((uint32_t)wav[pos + 6] << 16) | ((uint32_t)wav[pos + 7] << 24);
                bps = (uint16_t)(wav[pos + 14] | (wav[pos + 15] << 8));
            }
        } else if (id == 0x61746164 /* data */) {
            data = wav + pos;
            data_len = sz;
            if (pos + data_len > len) data_len = len - pos;
            break;
        }
        pos += sz + (sz & 1);
    }
    if (!data || bps != 16 || channels != 1 || rate != 22050) {
        /* Still try to play if 22050-ish mono 16-bit. */
        if (!data || bps != 16 || channels != 1) return NULL;
    }
    size_t ns = data_len / 2;
    int16_t *pcm = (int16_t *)malloc(ns * sizeof(int16_t));
    if (!pcm) return NULL;
    memcpy(pcm, data, ns * 2);
    *nsamp_out = ns;
    return pcm;
}

static uint8_t *pcm16k_to_wav(const int16_t *pcm, size_t nsamp, size_t *out_len)
{
    size_t data_bytes = nsamp * 2;
    size_t total = 44 + data_bytes;
    uint8_t *w = (uint8_t *)malloc(total);
    if (!w) return NULL;
    memset(w, 0, 44);
    memcpy(w, "RIFF", 4);
    uint32_t chunk = (uint32_t)(36 + data_bytes);
    memcpy(w + 4, &chunk, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    uint32_t fmt_sz = 16;
    memcpy(w + 16, &fmt_sz, 4);
    uint16_t audio = 1, ch = 1, bps = 16, ba = 2;
    uint32_t rate = 16000, br = 32000;
    memcpy(w + 20, &audio, 2);
    memcpy(w + 22, &ch, 2);
    memcpy(w + 24, &rate, 4);
    memcpy(w + 28, &br, 4);
    memcpy(w + 32, &ba, 2);
    memcpy(w + 34, &bps, 2);
    memcpy(w + 36, "data", 4);
    uint32_t dsz = (uint32_t)data_bytes;
    memcpy(w + 40, &dsz, 4);
    memcpy(w + 44, pcm, data_bytes);
    *out_len = total;
    return w;
}

static void do_ask(int16_t *pcm, size_t nsamp)
{
    size_t wav_len = 0;
    uint8_t *wav = pcm16k_to_wav(pcm, nsamp, &wav_len);
    free(pcm);
    if (!wav) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_ask_res, 0, sizeof(s_ask_res));
        s_ask_res.ready = true;
        s_ask_res.ok = false;
        snprintf(s_ask_res.error, sizeof(s_ask_res.error), "internal");
        snprintf(s_ask_res.message, sizeof(s_ask_res.message), "Out of memory");
        s_ask_ready = true;
        xSemaphoreGive(s_lock);
        s_ask_busy = false;
        return;
    }

    http_result_t r;
    http_with_failover("POST", "/ask", true, wav, wav_len, "audio/wav", 40000, false, &r);
    free(wav);

    pebble_ask_result_t res;
    memset(&res, 0, sizeof(res));
    res.ready = true;
    if (r.status == 200 && r.body && r.body_len) {
        res.ok = true;
        pebble_pct_decode(res.transcript, sizeof(res.transcript), r.hdr_transcript);
        pebble_pct_decode(res.reply, sizeof(res.reply), r.hdr_reply);
        pebble_pct_decode(res.mood, sizeof(res.mood), r.hdr_mood);
        res.pcm = wav_to_pcm22050((const uint8_t *)r.body, r.body_len, &res.nsamp);
        if (!res.pcm) {
            res.ok = false;
            snprintf(res.error, sizeof(res.error), "unsupported_audio");
            snprintf(res.message, sizeof(res.message), "Bad reply audio");
        }
    } else {
        res.ok = false;
        snprintf(res.error, sizeof(res.error), "%s", r.error[0] ? r.error : "internal");
        snprintf(res.message, sizeof(res.message), "%s",
                 r.message[0] ? r.message : "Ask failed");
        /* 401/403 already handled in http_with_failover */
    }
    http_result_free(&r);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ask_res.pcm) free(s_ask_res.pcm);
    s_ask_res = res;
    s_ask_ready = true;
    xSemaphoreGive(s_lock);
    s_ask_busy = false;
}

static void do_mood(void)
{
    http_result_t r;
    http_with_failover("GET", "/mood", true, NULL, 0, NULL, 10000, false, &r);
    if (r.status == 200 && r.body) {
        cJSON *j = cJSON_Parse(r.body);
        if (j) {
            cJSON *mood = cJSON_GetObjectItem(j, "mood");
            cJSON *cap = cJSON_GetObjectItem(j, "caption");
            cJSON *crev = cJSON_GetObjectItem(j, "config_rev");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (cJSON_IsString(mood) && mood->valuestring)
                snprintf(s_base_mood, sizeof(s_base_mood), "%s", mood->valuestring);
            else
                snprintf(s_base_mood, sizeof(s_base_mood), "content");
            if (cJSON_IsString(cap) && cap->valuestring)
                snprintf(s_base_caption, sizeof(s_base_caption), "%s", cap->valuestring);
            else
                s_base_caption[0] = 0;
            xSemaphoreGive(s_lock);

            if (cJSON_IsNumber(crev)) {
                pebble_pair_t p;
                memset(&p, 0, sizeof(p));
                if (port_pair_load(&p) && crev->valueint != p.config_rev) {
                    http_result_t cr;
                    http_with_failover("GET", "/config", true, NULL, 0, NULL, 10000, false, &cr);
                    if (cr.status == 200 && cr.body) {
                        port_pair_apply_config_json(&p, cr.body);
                        port_pair_save(&p);
                    }
                    /* 403 handled inside http_with_failover via handle_status */
                    http_result_free(&cr);
                }
            }
            cJSON_Delete(j);
        }
    }
    /* 401/403 already handled in http_with_failover */
    http_result_free(&r);
    s_mood_busy = false;
}

static void worker(void *arg)
{
    (void)arg;
    net_job_t job;
    for (;;) {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) != pdTRUE) continue;
        switch (job.kind) {
        case JOB_CLAIM:
            do_claim();
            break;
        case JOB_UNPAIR:
            do_unpair();
            break;
        case JOB_ASK:
            do_ask(job.ask_pcm, job.ask_nsamp);
            break;
        case JOB_MOOD:
            do_mood();
            break;
        default:
            break;
        }
    }
}

void pebble_net_start(void)
{
    if (s_q) return;
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(4, sizeof(net_job_t));
    xTaskCreate(worker, "pebble_net", 8192, NULL, 5, NULL);
    port_log(TAG, "worker up");
}

void pebble_net_claim_kick(void)
{
    if (!s_q || s_claim_busy) return;
    if (!port_wifi_connected()) return;
    if (port_pair_is_paired()) return;
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    (void)port_pair_load(&p);
    if (!p.host_edit[0]) {
        claim_status_set("Set StonePi host first");
        return;
    }
    s_claim_busy = true;
    net_job_t j = { .kind = JOB_CLAIM };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) s_claim_busy = false;
}

bool pebble_net_claim_busy(void) { return s_claim_busy; }

bool pebble_net_claim_just_paired(void)
{
    bool v = s_just_paired;
    s_just_paired = false;
    return v;
}

bool pebble_net_claim_need_new_code(void)
{
    bool v = s_need_new_code;
    s_need_new_code = false;
    return v;
}

int pebble_net_claim_poll_after_s(void)
{
    return s_poll_after_s > 0 ? s_poll_after_s : 3;
}

bool pebble_net_claim_take_status(char *msg, size_t msg_n)
{
    if (!s_lock) {
        if (msg && msg_n) msg[0] = 0;
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool hit = s_claim_status_pending;
    if (hit && msg && msg_n)
        snprintf(msg, msg_n, "%s", s_claim_status);
    else if (msg && msg_n)
        msg[0] = 0;
    s_claim_status_pending = false;
    xSemaphoreGive(s_lock);
    return hit;
}

void pebble_net_unpair(void)
{
    /* Clear locally first so UI shows unpaired immediately (PROTOCOL: clear
     * even if POST /unpair fails). Worker still notifies the Pi when online. */
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    bool paired = port_pair_load(&p) && p.token[0];
    if (paired) {
        snprintf(s_unpair_token, sizeof(s_unpair_token), "%s", p.token);
        const char *h = host_reachable_hint(p.host_primary) ? p.host_primary
                        : (p.host_edit[0] ? p.host_edit : p.host_primary);
        snprintf(s_unpair_host, sizeof(s_unpair_host), "%s", h ? h : "");
    }
    port_pair_clear();
    if (paired && s_q && port_wifi_connected() && s_unpair_token[0] && s_unpair_host[0]) {
        net_job_t j = { .kind = JOB_UNPAIR };
        if (xQueueSend(s_q, &j, pdMS_TO_TICKS(100)) != pdTRUE) {
            s_unpair_token[0] = 0;
            s_unpair_host[0] = 0;
        }
    } else {
        s_unpair_token[0] = 0;
        s_unpair_host[0] = 0;
    }
}

bool pebble_net_ask_start(const int16_t *pcm_16k, size_t nsamp)
{
    if (!s_q || s_ask_busy || !pcm_16k || !nsamp) return false;
    if (!port_pair_is_paired()) return false;
    int16_t *copy = (int16_t *)malloc(nsamp * sizeof(int16_t));
    if (!copy) return false;
    memcpy(copy, pcm_16k, nsamp * sizeof(int16_t));
    s_ask_busy = true;
    s_ask_ready = false;
    net_job_t j = { .kind = JOB_ASK, .ask_pcm = copy, .ask_nsamp = nsamp };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        free(copy);
        s_ask_busy = false;
        return false;
    }
    return true;
}

bool pebble_net_ask_busy(void) { return s_ask_busy || s_ask_ready; }

bool pebble_net_ask_take(pebble_ask_result_t *out)
{
    if (!out) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ask_ready) {
        xSemaphoreGive(s_lock);
        return false;
    }
    *out = s_ask_res;
    memset(&s_ask_res, 0, sizeof(s_ask_res));
    s_ask_ready = false;
    xSemaphoreGive(s_lock);
    return true;
}

void pebble_net_mood_kick(void)
{
    if (!s_q || s_mood_busy || !port_pair_is_paired()) return;
    if (!port_wifi_connected()) return;
    s_mood_busy = true;
    net_job_t j = { .kind = JOB_MOOD };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) s_mood_busy = false;
}

bool pebble_net_mood_busy(void) { return s_mood_busy; }

void pebble_net_base_mood(char *mood, size_t mood_n, char *caption, size_t cap_n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mood && mood_n)
        snprintf(mood, mood_n, "%s", s_base_mood[0] ? s_base_mood : "content");
    if (caption && cap_n)
        snprintf(caption, cap_n, "%s", s_base_caption);
    xSemaphoreGive(s_lock);
}

int pebble_net_mood_poll_s(void)
{
    pebble_pair_t p;
    if (!port_pair_load(&p) || p.mood_poll_s < 10) return 30;
    return p.mood_poll_s;
}

bool pebble_net_take_notice(char *msg, size_t msg_n)
{
    if (!s_lock) {
        if (msg && msg_n) msg[0] = 0;
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool hit = s_notice_pending;
    if (hit && msg && msg_n)
        snprintf(msg, msg_n, "%s", s_notice);
    else if (msg && msg_n)
        msg[0] = 0;
    s_notice_pending = false;
    s_notice[0] = 0;
    xSemaphoreGive(s_lock);
    return hit;
}

#endif /* !PEBBLE_SIM */
