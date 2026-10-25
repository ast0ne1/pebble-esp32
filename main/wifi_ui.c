#include "app.h"
#include "keypad.h"
#include "port.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define COL_ROW  0x2A2622
#define COL_TEXT 0xE8DCC8
#define COL_MUTE 0x9A8F7E
#define COL_BG   0x15181A
#define COL_ACC  0xC7BBA8

#define PANEL_W 466
#define PANEL_H 466

typedef struct {
    lv_obj_t *parent;
    lv_obj_t *browse; /* status + scan + list */
    lv_obj_t *status_val;
    lv_obj_t *net_val;
    lv_obj_t *ip_val;
    lv_obj_t *hint;
    lv_obj_t *scan_btn;
    lv_obj_t *list;
    lv_obj_t *join_page;
    lv_obj_t *join_title;
    lv_obj_t *pass_ta;
    lv_obj_t *join_btn;
    lv_obj_t *kp;
    lv_timer_t *scan_tm;
    lv_timer_t *join_tm;
    lv_timer_t *status_tm; /* keep Status/Network/IP live (Muse tick_wifi) */
    char pick_ssid[33];
    wifi_found_t aps[WIFI_SCAN_MAX];
    int ap_n;
} wifi_ui_t;

static wifi_ui_t s_ui;

static void refresh_status(void)
{
    if (!s_ui.status_val) return;
    char ssid[33] = {0};
    char ip[24] = {0};
    port_wifi_ssid(ssid, sizeof(ssid));
    port_wifi_ip(ip, sizeof(ip));

    /* Muse Wi‑Fi page: Joining / Connected / Not connected (no harsh "Failed"). */
    int st = port_wifi_try_state();
    if (st == WIFI_TRY_BUSY) {
        lv_label_set_text(s_ui.status_val, "Joining");
    } else if (port_wifi_connected()) {
        lv_label_set_text(s_ui.status_val, "Connected");
    } else {
        lv_label_set_text(s_ui.status_val, "Not connected");
    }
    lv_label_set_text(s_ui.net_val, ssid[0] ? ssid : "—");
    lv_label_set_text(s_ui.ip_val, ip[0] ? ip : "—");
}

static void make_row(lv_obj_t *parent, int y, const char *left, lv_obj_t **right_out)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, 280, 40);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(r, 12, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_hor(r, 12, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, left);
    lv_obj_set_style_text_color(a, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *b = lv_label_create(r);
    lv_label_set_text(b, "-");
    lv_obj_set_style_text_color(b, lv_color_hex(COL_MUTE), 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(b, 150);
    *right_out = b;
}

static void hide_join(void)
{
    if (s_ui.join_tm) {
        lv_timer_delete(s_ui.join_tm);
        s_ui.join_tm = NULL;
    }
    if (s_ui.join_page) lv_obj_add_flag(s_ui.join_page, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.browse) lv_obj_clear_flag(s_ui.browse, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.hint) lv_obj_clear_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
}

static void join_poll(lv_timer_t *t)
{
    (void)t;
    int st = port_wifi_try_state();
    refresh_status();
    if (st == WIFI_TRY_BUSY) return;
    if (s_ui.join_tm) {
        lv_timer_delete(s_ui.join_tm);
        s_ui.join_tm = NULL;
    }
    if (st == WIFI_TRY_OK || port_wifi_connected()) {
        if (s_ui.hint) lv_label_set_text(s_ui.hint, "Connected");
        hide_join();
        refresh_status();
    } else {
        if (s_ui.join_title && s_ui.pick_ssid[0])
            lv_label_set_text(s_ui.join_title, s_ui.pick_ssid);
        if (s_ui.join_btn) lv_obj_clear_state(s_ui.join_btn, LV_STATE_DISABLED);
        /* Muse: "Couldn't join …" — keep soft copy; Status shows Not connected. */
        if (s_ui.hint) lv_label_set_text(s_ui.hint, "Couldn't join — try again");
        /* Stay on keypad so the password can be fixed. */
        if (s_ui.join_page) {
            lv_obj_clear_flag(s_ui.join_page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ui.join_page);
        }
        if (s_ui.browse) lv_obj_add_flag(s_ui.browse, LV_OBJ_FLAG_HIDDEN);
    }
    shell_note_activity();
}

static void do_join(const char *pass)
{
    if (!s_ui.pick_ssid[0]) return;
    /* Status on the join page itself — hint sits under this overlay. */
    if (s_ui.join_title) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Joining %s", s_ui.pick_ssid);
        lv_label_set_text(s_ui.join_title, buf);
    }
    if (s_ui.join_btn) lv_obj_add_state(s_ui.join_btn, LV_STATE_DISABLED);
    if (s_ui.hint) {
        lv_label_set_text(s_ui.hint, "Joining...");
        lv_obj_clear_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_status();
    port_wifi_connect(s_ui.pick_ssid, pass ? pass : "");
    if (s_ui.join_tm) lv_timer_delete(s_ui.join_tm);
    s_ui.join_tm = lv_timer_create(join_poll, 250, NULL);
    shell_note_activity();
}

static void cancel_join_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_join();
    if (s_ui.hint) lv_label_set_text(s_ui.hint, "Tap a network");
}

static void submit_pass(void)
{
    const char *pass = s_ui.pass_ta ? lv_textarea_get_text(s_ui.pass_ta) : "";
    do_join(pass);
}

static void kp_ready_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;
    submit_pass();
}

static void join_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    submit_pass();
}

static void ensure_join_page(void)
{
    if (s_ui.join_page) return;

    s_ui.join_page = lv_obj_create(s_ui.parent);
    lv_obj_set_size(s_ui.join_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.join_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.join_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.join_page, 0, 0);
    lv_obj_set_style_pad_all(s_ui.join_page, 0, 0);
    lv_obj_clear_flag(s_ui.join_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.join_page, LV_OBJ_FLAG_HIDDEN);

    s_ui.join_title = lv_label_create(s_ui.join_page);
    lv_label_set_text(s_ui.join_title, "");
    lv_obj_set_style_text_font(s_ui.join_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_ui.join_title, lv_color_hex(COL_MUTE), 0);
    lv_obj_set_width(s_ui.join_title, 220);
    lv_label_set_long_mode(s_ui.join_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.join_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.join_title, LV_ALIGN_TOP_MID, 0, 16);

    /* Row: Back | password | Join — back returns to the network list. */
    lv_obj_t *back = lv_button_create(s_ui.join_page);
    lv_obj_set_size(back, 48, 48);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -148, 48);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, cancel_join_cb, LV_EVENT_CLICKED, NULL);

    s_ui.pass_ta = lv_textarea_create(s_ui.join_page);
    lv_obj_set_size(s_ui.pass_ta, 184, 48);
    lv_obj_align(s_ui.pass_ta, LV_ALIGN_TOP_MID, -20, 48);
    lv_textarea_set_one_line(s_ui.pass_ta, true);
    lv_textarea_set_password_mode(s_ui.pass_ta, true);
    lv_textarea_set_placeholder_text(s_ui.pass_ta, "Password");
    lv_textarea_set_max_length(s_ui.pass_ta, 63);
    lv_obj_set_style_text_font(s_ui.pass_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_ver(s_ui.pass_ta, 10, 0);
    lv_obj_set_style_pad_hor(s_ui.pass_ta, 14, 0);
    lv_obj_set_style_bg_color(s_ui.pass_ta, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_text_color(s_ui.pass_ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_color(s_ui.pass_ta, lv_color_hex(COL_ACC), 0);
    lv_obj_set_style_border_width(s_ui.pass_ta, 2, 0);
    lv_obj_set_style_radius(s_ui.pass_ta, 14, 0);
    lv_obj_set_style_text_font(s_ui.pass_ta, &lv_font_montserrat_14, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(s_ui.pass_ta, lv_color_hex(COL_MUTE), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_state(s_ui.pass_ta, LV_STATE_FOCUSED);

    s_ui.join_btn = lv_button_create(s_ui.join_page);
    lv_obj_set_size(s_ui.join_btn, 72, 48);
    lv_obj_align(s_ui.join_btn, LV_ALIGN_TOP_MID, 128, 48);
    lv_obj_set_style_radius(s_ui.join_btn, 14, 0);
    lv_obj_set_style_bg_color(s_ui.join_btn, lv_color_hex(0x3D6B4F), 0);
    lv_obj_set_style_shadow_width(s_ui.join_btn, 0, 0);
    lv_obj_t *jl = lv_label_create(s_ui.join_btn);
    lv_label_set_text(jl, "Join");
    lv_obj_set_style_text_font(jl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(jl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(jl);
    lv_obj_add_event_cb(s_ui.join_btn, join_btn_cb, LV_EVENT_CLICKED, NULL);

    const int kp_top = 108;
    s_ui.kp = keypad_create(s_ui.join_page, s_ui.pass_ta, true);
    lv_obj_set_size(s_ui.kp, PANEL_W, PANEL_H - kp_top);
    lv_obj_align(s_ui.kp, LV_ALIGN_TOP_MID, 0, kp_top);
    lv_obj_set_style_pad_bottom(s_ui.kp, 36, 0);
    /* Password submit is the Join button only (keypad OK hidden in password mode). */
    lv_obj_add_event_cb(s_ui.kp, kp_ready_cb, LV_EVENT_READY, NULL);
}

static void show_join(const char *ssid, bool open_net)
{
    snprintf(s_ui.pick_ssid, sizeof(s_ui.pick_ssid), "%s", ssid);

    if (open_net) {
        if (s_ui.hint) lv_label_set_text(s_ui.hint, "Open network");
        do_join("");
        return;
    }

    ensure_join_page();
    if (s_ui.browse) lv_obj_add_flag(s_ui.browse, LV_OBJ_FLAG_HIDDEN);
    if (s_ui.hint) lv_obj_add_flag(s_ui.hint, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_ui.join_title, ssid);
    lv_textarea_set_password_mode(s_ui.pass_ta, true);
    lv_textarea_set_text(s_ui.pass_ta, "");
    if (s_ui.join_btn) lv_obj_clear_state(s_ui.join_btn, LV_STATE_DISABLED);
    keypad_reset(s_ui.kp, true);
    lv_obj_clear_flag(s_ui.join_page, LV_OBJ_FLAG_HIDDEN);
    /* Above settings back chrome so the full keypad is tappable. */
    lv_obj_move_foreground(s_ui.join_page);
    shell_note_activity();
}

static void ap_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ui.ap_n) return;
    show_join(s_ui.aps[idx].ssid, s_ui.aps[idx].open != 0);
}

static void clear_list(void)
{
    if (!s_ui.list) return;
    lv_obj_clean(s_ui.list);
}

static void fill_list(void)
{
    clear_list();
    if (s_ui.ap_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_ui.list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(COL_MUTE), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_12, 0);
        return;
    }
    for (int i = 0; i < s_ui.ap_n; i++) {
        lv_obj_t *row = lv_button_create(s_ui.list);
        lv_obj_set_size(row, 280, 40);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW), 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);

        lv_obj_t *name = lv_label_create(row);
        char label[48];
        if (s_ui.aps[i].saved)
            snprintf(label, sizeof(label), "%s *", s_ui.aps[i].ssid);
        else
            snprintf(label, sizeof(label), "%s", s_ui.aps[i].ssid);
        lv_label_set_text(name, label);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *rssi = lv_label_create(row);
        char rb[16];
        snprintf(rb, sizeof(rb), "%d", (int)s_ui.aps[i].rssi);
        lv_label_set_text(rssi, rb);
        lv_obj_set_style_text_color(rssi, lv_color_hex(COL_MUTE), 0);
        lv_obj_set_style_text_font(rssi, &lv_font_montserrat_12, 0);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_add_event_cb(row, ap_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void scan_poll(lv_timer_t *t)
{
    (void)t;
    int n = port_wifi_scan_result(s_ui.aps, WIFI_SCAN_MAX);
    if (n == WIFI_SCAN_RUNNING) return;
    if (s_ui.scan_tm) {
        lv_timer_delete(s_ui.scan_tm);
        s_ui.scan_tm = NULL;
    }
    s_ui.ap_n = n < 0 ? 0 : n;
    fill_list();
    if (s_ui.hint) {
        char buf[40];
        snprintf(buf, sizeof(buf), s_ui.ap_n ? "%d networks" : "No networks", s_ui.ap_n);
        lv_label_set_text(s_ui.hint, buf);
    }
    if (s_ui.scan_btn) lv_obj_clear_state(s_ui.scan_btn, LV_STATE_DISABLED);
    shell_note_activity();
}

static void scan_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_join();
    if (s_ui.hint) lv_label_set_text(s_ui.hint, "Scanning...");
    clear_list();
    lv_obj_t *wait = lv_label_create(s_ui.list);
    lv_label_set_text(wait, "Scanning...");
    lv_obj_set_style_text_color(wait, lv_color_hex(COL_MUTE), 0);
    if (s_ui.scan_btn) lv_obj_add_state(s_ui.scan_btn, LV_STATE_DISABLED);
    port_wifi_scan_start();
    if (s_ui.scan_tm) lv_timer_delete(s_ui.scan_tm);
    s_ui.scan_tm = lv_timer_create(scan_poll, 300, NULL);
    shell_note_activity();
}

static void status_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ui.browse || lv_obj_has_flag(s_ui.browse, LV_OBJ_FLAG_HIDDEN)) return;
    refresh_status();
}

void wifi_panel_close(void)
{
    if (s_ui.scan_tm) {
        lv_timer_delete(s_ui.scan_tm);
        s_ui.scan_tm = NULL;
    }
    if (s_ui.join_tm) {
        lv_timer_delete(s_ui.join_tm);
        s_ui.join_tm = NULL;
    }
    if (s_ui.status_tm) {
        lv_timer_delete(s_ui.status_tm);
        s_ui.status_tm = NULL;
    }
    /* join_page / keypad deleted with the settings panel; just drop refs. */
    memset(&s_ui, 0, sizeof(s_ui));
}

void wifi_panel_build(lv_obj_t *parent)
{
    wifi_panel_close();
    s_ui.parent = parent;

    s_ui.browse = lv_obj_create(parent);
    lv_obj_set_size(s_ui.browse, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_ui.browse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.browse, 0, 0);
    lv_obj_set_style_pad_all(s_ui.browse, 0, 0);
    lv_obj_clear_flag(s_ui.browse, LV_OBJ_FLAG_SCROLLABLE);
    /* Don't steal hits from the panel back button underneath empty areas. */
    lv_obj_clear_flag(s_ui.browse, LV_OBJ_FLAG_CLICKABLE);

    make_row(s_ui.browse, 70, "Status", &s_ui.status_val);
    make_row(s_ui.browse, 114, "Network", &s_ui.net_val);
    make_row(s_ui.browse, 158, "IP", &s_ui.ip_val);
    refresh_status();
    s_ui.status_tm = lv_timer_create(status_tick, 500, NULL);

    s_ui.scan_btn = lv_button_create(s_ui.browse);
    lv_obj_set_size(s_ui.scan_btn, 280, 36);
    lv_obj_align(s_ui.scan_btn, LV_ALIGN_TOP_MID, 0, 206);
    lv_obj_set_style_radius(s_ui.scan_btn, 12, 0);
    lv_obj_set_style_bg_color(s_ui.scan_btn, lv_color_hex(0x3D6B4F), 0);
    lv_obj_set_style_shadow_width(s_ui.scan_btn, 0, 0);
    lv_obj_t *sl = lv_label_create(s_ui.scan_btn);
    lv_label_set_text(sl, "Scan");
    lv_obj_set_style_text_color(sl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(s_ui.scan_btn, scan_cb, LV_EVENT_CLICKED, NULL);

    s_ui.list = lv_obj_create(s_ui.browse);
    lv_obj_set_size(s_ui.list, 300, 150);
    lv_obj_align(s_ui.list, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_set_style_bg_opa(s_ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.list, 0, 0);
    lv_obj_set_style_pad_all(s_ui.list, 0, 0);
    lv_obj_set_style_pad_row(s_ui.list, 6, 0);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);

    s_ui.hint = lv_label_create(parent);
    lv_label_set_text(s_ui.hint, "Scan and pick a network");
    lv_obj_set_style_text_font(s_ui.hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_ui.hint, lv_color_hex(COL_MUTE), 0);
    lv_obj_align(s_ui.hint, LV_ALIGN_BOTTOM_MID, 0, -36);
}
