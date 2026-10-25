#include "app.h"
#include "port.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t bright;
    int32_t volume;
    int32_t timeout;
    int32_t sound_on;
} pebble_cfg_t;

static int  s_volume = 90;
static bool s_sound_on = true; /* chirps / UI sounds */
static lv_obj_t *s_panel;
static lv_obj_t *s_back;

int settings_get_volume(void) { return s_volume; }
bool settings_sounds_on(void) { return s_sound_on; }

void settings_load(void)
{
    pebble_cfg_t v;
    if (!port_kv_read("cfg", &v, sizeof(v))) {
        port_brightness_set(45);
        shell_set_timeout(300);
        return;
    }
    if (v.bright >= 5 && v.bright <= 100) port_brightness_set(v.bright);
    if (v.volume >= 0 && v.volume <= 100) {
        /* Snap to Sound panel presets. */
        static const int presets[] = { 40, 60, 80, 100 };
        int best = presets[0], bd = abs(v.volume - presets[0]);
        for (unsigned i = 1; i < sizeof(presets) / sizeof(presets[0]); i++) {
            int d = abs(v.volume - presets[i]);
            if (d < bd) { bd = d; best = presets[i]; }
        }
        s_volume = best;
    }
    s_sound_on = v.sound_on != 0;
    /* Snap to Off / 3 / 5 / 10 / 30 min */
    {
        const int presets[] = { 0, 180, 300, 600, 1800 };
        int best = 300;
        bool hit = false;
        for (unsigned i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
            if (presets[i] == v.timeout) { best = presets[i]; hit = true; break; }
        }
        shell_set_timeout(hit ? best : 300);
    }
}

static void settings_save(void)
{
    pebble_cfg_t v = {
        .bright = port_brightness_get(),
        .volume = s_volume,
        .timeout = shell_get_timeout(),
        .sound_on = s_sound_on,
    };
    port_kv_write("cfg", &v, sizeof(v));
}

void pairing_panel_build(lv_obj_t *parent);
void pairing_panel_close(void);
void wifi_panel_build(lv_obj_t *parent);
void wifi_panel_close(void);

static void close_panel(void)
{
    pairing_panel_close();
    wifi_panel_close();
    if (s_panel) {
        lv_obj_delete(s_panel);
        s_panel = NULL;
    }
    s_back = NULL;
    shell_refresh_settings();
}

void settings_close_panel(void) { close_panel(); }

static void on_back(void) { close_panel(); }

static void panel_raise_chrome(void)
{
    if (s_back) lv_obj_move_foreground(s_back);
}

static lv_obj_t *panel_base(const char *title)
{
    close_panel();
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x15181A), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    shell_wire_panel_gestures(s_panel);

    s_back = ui_back_btn(s_panel, on_back);

    lv_obj_t *t = lv_label_create(s_panel);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x9A8F7E), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 32);

    return s_panel;
}

static void power_off_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    port_power_off();
}

static void open_power(void)
{
    lv_obj_t *p = panel_base("Power off");
    lv_obj_t *q = lv_label_create(p);
    lv_label_set_text(q, "Turn Pebble off?");
    lv_obj_set_style_text_font(q, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(q, lv_color_hex(0xE8DCC8), 0);
    lv_obj_align(q, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *btn = lv_button_create(p);
    lv_obj_set_size(btn, 200, 44);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4A2A2A), 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, "Power off");
    lv_obj_set_style_text_color(lab, lv_color_hex(0xE8A0A0), 0);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, power_off_cb, LV_EVENT_CLICKED, NULL);
}

static void vol_pick_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_volume = (int)(intptr_t)lv_event_get_user_data(e);
    settings_save();
    shell_note_activity();
    settings_close_panel();
    settings_open_panel("sound");
}

static void sounds_toggle_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_sound_on = !s_sound_on;
    settings_save();
    shell_note_activity();
    settings_close_panel();
    settings_open_panel("sound");
}

static void open_sound(void)
{
    lv_obj_t *p = panel_base("Sound");

    lv_obj_t *sub = lv_label_create(p);
    lv_label_set_text(sub, "Volume");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9A8F7E), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 68);

    static const int opts[] = { 40, 60, 80, 100 };
    for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        bool on = opts[i] == s_volume;

        lv_obj_t *btn = lv_button_create(p);
        lv_obj_set_size(btn, 260, 38);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 90 + (int)i * 40);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(on ? 0x3A342C : 0x2A2622), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        char labuf[8];
        snprintf(labuf, sizeof(labuf), "%d%%", opts[i]);
        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, labuf);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(0xE8DCC8), 0);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 14, 0);

        if (on) {
            lv_obj_t *mark = lv_label_create(btn);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(mark, lv_color_hex(0xC7BBA8), 0);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -14, 0);
        }

        lv_obj_add_event_cb(btn, vol_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)opts[i]);
    }

    lv_obj_t *tog = lv_button_create(p);
    lv_obj_set_size(tog, 260, 40);
    lv_obj_align(tog, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_set_style_radius(tog, 12, 0);
    lv_obj_set_style_bg_color(tog, lv_color_hex(0x2A2622), 0);
    lv_obj_set_style_shadow_width(tog, 0, 0);
    lv_obj_add_event_cb(tog, sounds_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tl = lv_label_create(tog);
    lv_label_set_text(tl, "Chirps");
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xE8DCC8), 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *tr = lv_label_create(tog);
    lv_label_set_text(tr, s_sound_on ? "On" : "Off");
    lv_obj_set_style_text_font(tr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tr, lv_color_hex(0x9A8F7E), 0);
    lv_obj_align(tr, LV_ALIGN_RIGHT_MID, -14, 0);
}

static void sleep_pick_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    shell_set_timeout(sec);
    settings_save();
    shell_note_activity();
    /* Rebuild so the check mark moves. */
    settings_close_panel();
    settings_open_panel("sleep");
}

static void open_sleep(void)
{
    lv_obj_t *p = panel_base("Sleep");

    lv_obj_t *sub = lv_label_create(p);
    lv_label_set_text(sub, "Auto screen off");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9A8F7E), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 68);

    static const struct { const char *label; int sec; } opts[] = {
        { "Off", 0 },
        { "3 min", 180 },
        { "5 min", 300 },
        { "10 min", 600 },
        { "30 min", 1800 },
    };
    int cur = shell_get_timeout();
    for (unsigned i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        bool on = opts[i].sec == cur;
        lv_obj_t *btn = lv_button_create(p);
        lv_obj_set_size(btn, 260, 38);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 90 + (int)i * 40);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(on ? 0x3A342C : 0x2A2622), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, opts[i].label);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(0xE8DCC8), 0);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 14, 0);

        if (on) {
            lv_obj_t *mark = lv_label_create(btn);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(mark, lv_color_hex(0xC7BBA8), 0);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -14, 0);
        }

        lv_obj_add_event_cb(btn, sleep_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)opts[i].sec);
    }
}

static void open_battery(void)
{
    /* Read-only status — not picker rows (those look editable). */
    lv_obj_t *p = panel_base("Battery");
    int pct = port_battery_percent();
    bool plugged = port_battery_plugged();
    bool charging = port_battery_charging();

    char pct_buf[12];
    if (pct >= 0) snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    else snprintf(pct_buf, sizeof(pct_buf), "—");

    const char *status;
    if (pct < 0) status = "No pack detected";
    else if (charging) status = "Charging";
    else if (plugged) status = "Plugged in";
    else if (pct <= 15) status = "Low — plug in soon";
    else status = "On battery";

    lv_obj_t *big = lv_label_create(p);
    lv_label_set_text(big, pct_buf);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(big, lv_color_hex(0xE8DCC8), 0);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -36);

    /* Track + fill (skip when unknown). */
    if (pct >= 0) {
        lv_obj_t *track = lv_obj_create(p);
        lv_obj_set_size(track, 200, 10);
        lv_obj_align(track, LV_ALIGN_CENTER, 0, 18);
        lv_obj_set_style_radius(track, 5, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x2A2622), 0);
        lv_obj_set_style_border_width(track, 0, 0);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

        int fill_w = (200 * pct) / 100;
        if (fill_w < 4 && pct > 0) fill_w = 4;
        lv_obj_t *fill = lv_obj_create(track);
        lv_obj_set_size(fill, fill_w, 10);
        lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(fill, 5, 0);
        lv_obj_set_style_bg_color(fill,
            lv_color_hex(charging ? 0x8A9A7A : (pct <= 15 ? 0xA07060 : 0xC7BBA8)), 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *st = lv_label_create(p);
    lv_label_set_text(st, status);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(0x9A8F7E), 0);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(st, LV_ALIGN_CENTER, 0, pct >= 0 ? 48 : 20);
}

void settings_open_panel(const char *id)
{
    if (!id) return;
    shell_note_activity();
    if (strcmp(id, "wifi") == 0) {
        lv_obj_t *p = panel_base("Wi-Fi");
        wifi_panel_build(p);
    } else if (strcmp(id, "pairing") == 0) {
        lv_obj_t *p = panel_base("Pairing");
        pairing_panel_build(p);
    } else if (strcmp(id, "sound") == 0) {
        open_sound();
    } else if (strcmp(id, "sleep") == 0) {
        open_sleep();
    } else if (strcmp(id, "battery") == 0) {
        open_battery();
    } else if (strcmp(id, "power") == 0) {
        open_power();
    }
    panel_raise_chrome();
}
