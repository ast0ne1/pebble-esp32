#include "app.h"
#include "keypad.h"
#include "pebble_net.h"
#include "port.h"

#include <stdio.h>
#include <string.h>

/* http:// + Wi‑Fi subnet (a.b.c.) so the user only types the Pi’s last octet. */
static void host_lan_prefix(char *out, size_t n)
{
    char ip[24];
    unsigned a = 0, b = 0, c = 0, d = 0;
    port_wifi_ip(ip, sizeof(ip));
    if (ip[0] && sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        snprintf(out, n, "http://%u.%u.%u.", a, b, c);
        return;
    }
    snprintf(out, n, "http://192.168.0.");
}

#define COL_ROW  0x2A2622
#define COL_TEXT 0xE8DCC8
#define COL_MUTE 0x9A8F7E
#define COL_ACC  0xC7BBA8

static lv_obj_t *s_status;
static lv_obj_t *s_code_lab;
static lv_timer_t *s_poll;
static lv_obj_t *s_host_page;
static lv_obj_t *s_host_ta;
static lv_obj_t *s_host_kp;
static lv_obj_t *s_panel_parent;
static uint32_t s_next_poll_ms;

static void stop_poll(void)
{
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
}

static void hide_host_edit(void)
{
    if (s_host_page) lv_obj_add_flag(s_host_page, LV_OBJ_FLAG_HIDDEN);
}

static void unpair_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pebble_net_unpair();
    settings_close_panel();
    settings_open_panel("pairing");
}

static void refresh_code_label(void)
{
    if (!s_code_lab) return;
    const char *code = port_pair_claim_code();
    lv_label_set_text(s_code_lab, code ? code : "----");
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = port_millis();
    if (now < s_next_poll_ms) return;

    if (pebble_net_claim_need_new_code()) {
        port_pair_new_claim();
        refresh_code_label();
        if (s_status) lv_label_set_text(s_status, "New code — try again");
    }

    if (port_pair_poll_claim() || pebble_net_claim_just_paired()) {
        stop_poll();
        settings_close_panel();
        settings_open_panel("pairing");
        shell_refresh_settings();
        return;
    }

    char claim_msg[80];
    if (pebble_net_claim_take_status(claim_msg, sizeof(claim_msg)) && claim_msg[0] && s_status) {
        lv_label_set_text(s_status, claim_msg);
        bool err = strncmp(claim_msg, "Can't", 5) == 0 || strncmp(claim_msg, "Code", 4) == 0 ||
                   strncmp(claim_msg, "Too many", 8) == 0 || strncmp(claim_msg, "Set ", 4) == 0 ||
                   strncmp(claim_msg, "New code", 8) == 0 || strncmp(claim_msg, "No Pebble", 9) == 0;
        lv_obj_set_style_text_color(s_status, lv_color_hex(err ? 0xE8A0A0 : 0xC8B8A0), 0);
    } else if (s_status && pebble_net_claim_busy()) {
        lv_label_set_text(s_status, "Checking...");
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xC8B8A0), 0);
    }

    int after = pebble_net_claim_poll_after_s();
    if (after < 1) after = 3;
    s_next_poll_ms = now + (uint32_t)after * 1000u;
}

static void host_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_host_edit();
}

static void host_ready_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;
    const char *host = s_host_ta ? lv_textarea_get_text(s_host_ta) : "";
    port_pair_set_host_edit(host);
    hide_host_edit();
    settings_close_panel();
    settings_open_panel("pairing");
}

static void ensure_host_page(lv_obj_t *parent)
{
    if (s_host_page) return;

    s_host_page = lv_obj_create(parent);
    lv_obj_set_size(s_host_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_host_page, lv_color_hex(0x15181A), 0);
    lv_obj_set_style_bg_opa(s_host_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_host_page, 0, 0);
    lv_obj_set_style_pad_all(s_host_page, 0, 0);
    lv_obj_add_flag(s_host_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_host_page);
    lv_obj_set_size(back, 48, 48);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -140, 44);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, host_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_host_page);
    lv_label_set_text(title, "StonePi host");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *hint = lv_label_create(s_host_page);
    lv_label_set_text(hint, "Type last number only (e.g. 225)");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 28);

    s_host_ta = lv_textarea_create(s_host_page);
    lv_obj_set_size(s_host_ta, 280, 44);
    lv_obj_align(s_host_ta, LV_ALIGN_TOP_MID, 0, 46);
    lv_textarea_set_one_line(s_host_ta, true);
    lv_textarea_set_max_length(s_host_ta, 95);
    lv_textarea_set_placeholder_text(s_host_ta, "http://192.168.0.");
    lv_obj_set_style_text_font(s_host_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(s_host_ta, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_text_color(s_host_ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_color(s_host_ta, lv_color_hex(COL_ACC), 0);
    lv_obj_set_style_border_width(s_host_ta, 2, 0);
    lv_obj_set_style_radius(s_host_ta, 14, 0);
    lv_obj_set_style_pad_ver(s_host_ta, 10, 0);
    lv_obj_set_style_pad_hor(s_host_ta, 10, 0);

    s_host_kp = keypad_create(s_host_page, s_host_ta, true);
    lv_obj_set_size(s_host_kp, 466, 466 - 98);
    lv_obj_align(s_host_kp, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_pad_bottom(s_host_kp, LV_DPI_DEF / 10, 0);
    lv_obj_add_event_cb(s_host_kp, host_ready_cb, LV_EVENT_READY, NULL);
}

static void edit_host_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_panel_parent) return;
    ensure_host_page(s_panel_parent);
    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    port_pair_load(&p);
    port_pair_ensure_host_edit(&p);
    if (p.host_edit[0]) {
        lv_textarea_set_text(s_host_ta, p.host_edit);
        keypad_reset(s_host_kp, false);
    } else {
        char pref[40];
        host_lan_prefix(pref, sizeof(pref));
        lv_textarea_set_text(s_host_ta, pref);
        keypad_reset(s_host_kp, false);
        keypad_use_digits(s_host_kp);
    }
    lv_textarea_set_cursor_pos(s_host_ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_clear_flag(s_host_page, LV_OBJ_FLAG_HIDDEN);
    shell_note_activity();
}

void pairing_panel_close(void)
{
    stop_poll();
    s_status = NULL;
    s_code_lab = NULL;
    s_host_page = NULL;
    s_host_ta = NULL;
    s_host_kp = NULL;
    s_panel_parent = NULL;
}

void pairing_panel_build(lv_obj_t *parent)
{
    stop_poll();
    s_status = NULL;
    s_code_lab = NULL;
    s_panel_parent = parent;
    s_host_page = NULL;
    s_host_ta = NULL;
    s_host_kp = NULL;

    if (port_pair_is_paired()) {
        pebble_pair_t p;
        memset(&p, 0, sizeof(p));
        port_pair_load(&p);

        lv_obj_t *r1 = lv_obj_create(parent);
        lv_obj_set_size(r1, 280, 40);
        lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_set_style_radius(r1, 12, 0);
        lv_obj_set_style_bg_color(r1, lv_color_hex(COL_ROW), 0);
        lv_obj_set_style_border_width(r1, 0, 0);
        lv_obj_t *a = lv_label_create(r1);
        lv_label_set_text(a, "Status");
        lv_obj_set_style_text_color(a, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *b = lv_label_create(r1);
        lv_label_set_text(b, "Paired");
        lv_obj_set_style_text_color(b, lv_color_hex(COL_MUTE), 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);

        char name[40];
        snprintf(name, sizeof(name), "%s", p.nickname[0] ? p.nickname : "Pebble");
        lv_obj_t *r2 = lv_obj_create(parent);
        lv_obj_set_size(r2, 280, 40);
        lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 120);
        lv_obj_set_style_radius(r2, 12, 0);
        lv_obj_set_style_bg_color(r2, lv_color_hex(COL_ROW), 0);
        lv_obj_set_style_border_width(r2, 0, 0);
        lv_obj_t *n1 = lv_label_create(r2);
        lv_label_set_text(n1, "Name");
        lv_obj_set_style_text_color(n1, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(n1, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *n2 = lv_label_create(r2);
        lv_label_set_text(n2, name);
        lv_obj_set_style_text_color(n2, lv_color_hex(COL_MUTE), 0);
        lv_obj_align(n2, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_t *r3 = lv_obj_create(parent);
        lv_obj_set_size(r3, 280, 40);
        lv_obj_align(r3, LV_ALIGN_TOP_MID, 0, 168);
        lv_obj_set_style_radius(r3, 12, 0);
        lv_obj_set_style_bg_color(r3, lv_color_hex(COL_ROW), 0);
        lv_obj_set_style_border_width(r3, 0, 0);
        lv_obj_t *h1 = lv_label_create(r3);
        lv_label_set_text(h1, "Host");
        lv_obj_set_style_text_color(h1, lv_color_hex(COL_TEXT), 0);
        lv_obj_align(h1, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *h2 = lv_label_create(r3);
        lv_label_set_text(h2, p.host_primary[0] ? p.host_primary : "-");
        lv_obj_set_style_text_font(h2, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(h2, lv_color_hex(COL_MUTE), 0);
        lv_obj_set_width(h2, 160);
        lv_label_set_long_mode(h2, LV_LABEL_LONG_DOT);
        lv_obj_align(h2, LV_ALIGN_RIGHT_MID, -12, 0);

        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_size(btn, 200, 44);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -48);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A2A2A), 0);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, "Unpair");
        lv_obj_set_style_text_color(lab, lv_color_hex(0xE8A0A0), 0);
        lv_obj_center(lab);
        lv_obj_add_event_cb(btn, unpair_cb, LV_EVENT_CLICKED, NULL);
        return;
    }

    if (!port_wifi_connected()) {
        lv_obj_t *hint = lv_label_create(parent);
        lv_label_set_text(hint, "Connect Wi-Fi first");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xE8A0A0), 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    pebble_pair_t p;
    memset(&p, 0, sizeof(p));
    port_pair_load(&p);
    port_pair_ensure_host_edit(&p);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "On StonePi:\nNotify > Sidekick > Add Pebble");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 68);

    const char *code = port_pair_claim_code();
    lv_obj_t *code_box = lv_obj_create(parent);
    lv_obj_set_size(code_box, 220, 64);
    lv_obj_align(code_box, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(code_box, 16, 0);
    lv_obj_set_style_bg_color(code_box, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_border_width(code_box, 0, 0);

    s_code_lab = lv_label_create(code_box);
    lv_label_set_text(s_code_lab, code);
    lv_obj_set_style_text_font(s_code_lab, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_code_lab, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_code_lab, 6, 0);
    lv_obj_center(s_code_lab);

    lv_obj_t *host_btn = lv_button_create(parent);
    lv_obj_set_size(host_btn, 280, 40);
    lv_obj_align(host_btn, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_radius(host_btn, 12, 0);
    lv_obj_set_style_bg_color(host_btn, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_shadow_width(host_btn, 0, 0);
    lv_obj_add_event_cb(host_btn, edit_host_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hl = lv_label_create(host_btn);
    lv_label_set_text(hl, "Host");
    lv_obj_set_style_text_color(hl, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *hv = lv_label_create(host_btn);
    lv_label_set_text(hv, p.host_edit[0] ? p.host_edit : "Tap to set");
    lv_obj_set_style_text_font(hv, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hv, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_width(hv, 180);
    lv_label_set_long_mode(hv, LV_LABEL_LONG_DOT);
    lv_obj_align(hv, LV_ALIGN_RIGHT_MID, -12, 0);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "Waiting...");
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xC8B8A0), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -56);

    s_next_poll_ms = port_millis();
    s_poll = lv_timer_create(poll_cb, 500, NULL);
}
