/* Home (Pebble face art) ↔ Settings — swipe + page dots. */
#include "app.h"
#include "assets/assets.h"
#include "pebble_net.h"
#include "port.h"
#include "voice.h"
#include "wake.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE_HOME     0
#define PAGE_SETTINGS 1
#define SWIPE_PX      40

#define COL_UI     0x15181A
#define COL_TEXT   0xF4F1EA
#define COL_SAND   0xC7BBA8
#define COL_MUTED  0x9AA0A6
#define COL_ROW    0x1F2326
#define COL_DANGER 0xE8A0A0

static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static lv_obj_t *s_settings;
static lv_obj_t *s_settings_list;
static lv_obj_t *s_face;
static lv_obj_t *s_clock;
static lv_obj_t *s_caption;
static lv_obj_t *s_dot0;
static lv_obj_t *s_dot1;
static int       s_page;
static int       s_timeout_s = 300; /* default 5 min */
static bool      s_keep_awake;
static bool      s_screen_off;
static int32_t   s_drag_x0 = -1;
static int32_t   s_drag_y0 = -1;
static uint32_t  s_press_ms;
static bool      s_ptt_down;
static bool      s_ptt_from_touch; /* face hold vs hardware/Talk button */
static bool      s_swiping;        /* horizontal drag — not a PTT hold */
static uint32_t  s_last_activity_ms;
static lv_timer_t *s_mood_timer;
static lv_timer_t *s_ask_timer;
static lv_timer_t *s_net_mood_timer;
static lv_timer_t *s_tap_timer;
static lv_timer_t *s_wake_listen_timer;
static unsigned   s_tap_line;
static bool       s_tap_busy;
static bool       s_breathe_on;
static bool       s_mood_idle; /* true when showing a calm base face */
static char       s_base_mood[16] = "content";
static char       s_base_caption[48];
static bool       s_ask_waiting;

static const int s_timeout_presets[] = { 0, 3 * 60, 5 * 60, 10 * 60, 30 * 60 };
#define PTT_HOLD_MS 220
#define PTT_MOVE_PX 18
#define PTT_THINK_MS 1400
#define TAP_MS       900
#define BREATHE_PX   7
#define BREATHE_MS   1700

static void face_breathe_start(void);
static void face_breathe_stop(void);

static const char *const s_tap_lines[] = {
    "Hey!",
    "Hi there!",
    "That tickles",
    "Thanks!",
    "*boop*",
    "Missed you",
    "I'm happy",
    "Again!",
};

static void paint_dots(void)
{
    lv_obj_set_style_bg_opa(s_dot0, s_page == 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(s_dot1, s_page == 1 ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void fill_settings_rows(lv_obj_t *list);

void shell_refresh_settings(void)
{
    if (s_settings_list) fill_settings_rows(s_settings_list);
}

static void show_page(int page)
{
    if (page < 0) page = 0;
    if (page > 1) page = 1;
    s_page = page;
    if (page == PAGE_HOME) {
        settings_close_panel();
        lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        if (s_mood_idle) face_breathe_start();
    } else {
        face_breathe_stop();
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        shell_refresh_settings();
    }
    paint_dots();
}

void shell_goto_page(int page) { show_page(page); }
int  shell_page(void) { return s_page; }
void shell_show_home(void) { show_page(PAGE_HOME); }
void shell_show_settings(void) { show_page(PAGE_SETTINGS); }

void shell_set_timeout(int seconds) { s_timeout_s = seconds < 0 ? 0 : seconds; }
int  shell_get_timeout(void) { return s_timeout_s; }

void shell_timeout_label(char *out, size_t n)
{
    if (!out || !n) return;
    if (s_timeout_s <= 0) {
        snprintf(out, n, "Off");
        return;
    }
    snprintf(out, n, "%d min", s_timeout_s / 60);
}

void shell_timeout_cycle(void)
{
    const int n = (int)(sizeof(s_timeout_presets) / sizeof(s_timeout_presets[0]));
    int i = 0;
    for (; i < n; i++) {
        if (s_timeout_presets[i] == s_timeout_s) break;
    }
    if (i >= n) i = 0; /* unknown → treat as Off */
    s_timeout_s = s_timeout_presets[(i + 1) % n];
    shell_note_activity();
}

void shell_note_activity(void)
{
    s_last_activity_ms = port_millis();
    if (s_screen_off) shell_screen_on();
}

void shell_keep_awake(bool on) { s_keep_awake = on; }

void shell_screen_off(void)
{
    if (s_screen_off) return;
    s_screen_off = true;
    face_breathe_stop();
    port_display_on(false);
}

void shell_screen_on(void)
{
    if (!s_screen_off) return;
    s_screen_off = false;
    port_display_on(true);
    s_last_activity_ms = port_millis();
    if (s_mood_idle && s_page == PAGE_HOME) face_breathe_start();
}

bool shell_screen_is_off(void) { return s_screen_off; }

void shell_set_mood(const char *mood); /* tap / ptt call this */

static void anim_ty_cb(void *obj, int32_t v);

static void face_reset_xform(void)
{
    if (!s_face) return;
    lv_obj_set_style_translate_y(s_face, 0, 0);
    lv_image_set_rotation(s_face, 0);
    lv_image_set_scale(s_face, LV_SCALE_NONE);
}

static void face_breathe_stop(void)
{
    if (!s_face) return;
    if (s_breathe_on) {
        lv_anim_delete(s_face, anim_ty_cb);
        s_breathe_on = false;
    }
    lv_obj_set_style_translate_y(s_face, 0, 0);
}

static void face_breathe_start(void)
{
    if (!s_face || !s_mood_idle || s_ptt_down || s_tap_busy) return;
    if (s_page != PAGE_HOME || s_screen_off) return;
    if (s_breathe_on) return;
    s_breathe_on = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, 0, -BREATHE_PX);
    lv_anim_set_duration(&a, BREATHE_MS);
    lv_anim_set_reverse_duration(&a, BREATHE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_ty_cb);
    lv_anim_start(&a);
}

static void restore_base_mood(void)
{
    char mood[16];
    char cap[48];
    snprintf(mood, sizeof(mood), "%s", s_base_mood[0] ? s_base_mood : "content");
    snprintf(cap, sizeof(cap), "%s", s_base_caption);
    shell_set_mood(mood);
    if (s_caption && cap[0] && !s_tap_busy)
        lv_label_set_text(s_caption, cap);
}

static void mood_voice_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (voice_is_busy()) {
        shell_note_activity();
        return;
    }
    if (s_mood_timer) {
        lv_timer_delete(s_mood_timer);
        s_mood_timer = NULL;
    }
    if (!s_ptt_down) restore_base_mood();
}

static void mood_speak_cb(lv_timer_t *t)
{
    (void)t;
    s_mood_timer = NULL;
    if (s_ptt_down) return;
    shell_set_mood("speaking");
    s_mood_timer = lv_timer_create(mood_voice_poll_cb, 120, NULL);
}

static void ask_poll_cb(lv_timer_t *t)
{
    (void)t;
    pebble_ask_result_t res;
    if (!pebble_net_ask_take(&res)) {
        if (!voice_ask_in_flight() && !pebble_net_ask_busy()) {
            /* Ask never started — fall back handled by loopback path. */
            if (s_ask_timer) {
                lv_timer_delete(s_ask_timer);
                s_ask_timer = NULL;
            }
            s_ask_waiting = false;
        }
        return;
    }
    if (s_ask_timer) {
        lv_timer_delete(s_ask_timer);
        s_ask_timer = NULL;
    }
    s_ask_waiting = false;
    shell_note_activity();

    if (!res.ok) {
        free(res.pcm);
        voice_ask_clear();
        /* owner_disabled / away_disabled / AI errors — pairing stays unless 401. */
        shell_set_mood("upset");
        if (s_caption)
            lv_label_set_text(s_caption, res.message[0] ? res.message : "Something went wrong");
        if (s_mood_timer) {
            lv_timer_delete(s_mood_timer);
            s_mood_timer = NULL;
        }
        s_mood_timer = lv_timer_create(mood_voice_poll_cb, 2200, NULL);
        lv_timer_set_repeat_count(s_mood_timer, 1);
        return;
    }

    if (res.reply[0] && s_caption) lv_label_set_text(s_caption, res.reply);
    else if (res.transcript[0] && s_caption) lv_label_set_text(s_caption, res.transcript);

    if (res.mood[0]) {
        snprintf(s_base_mood, sizeof(s_base_mood), "%s", res.mood);
        if (res.reply[0])
            snprintf(s_base_caption, sizeof(s_base_caption), "%s", res.reply);
    }

    shell_set_mood("speaking");
    /* voice_play_pcm22050 takes ownership of pcm (frees on failure paths). */
    if (!voice_play_pcm22050(res.pcm, res.nsamp)) {
        voice_ask_clear();
        restore_base_mood();
        return;
    }
    if (s_mood_timer) {
        lv_timer_delete(s_mood_timer);
        s_mood_timer = NULL;
    }
    s_mood_timer = lv_timer_create(mood_voice_poll_cb, 120, NULL);
}

static void mood_after_ptt(void)
{
    shell_set_mood("thinking");
    if (s_mood_timer) {
        lv_timer_delete(s_mood_timer);
        s_mood_timer = NULL;
    }
    if (s_ask_timer) {
        lv_timer_delete(s_ask_timer);
        s_ask_timer = NULL;
    }

    if (voice_ask_in_flight() || pebble_net_ask_busy()) {
        s_ask_waiting = true;
        s_ask_timer = lv_timer_create(ask_poll_cb, 100, NULL);
        return;
    }

    /* Unpaired loopback. */
    s_mood_timer = lv_timer_create(mood_speak_cb, PTT_THINK_MS, NULL);
    lv_timer_set_repeat_count(s_mood_timer, 1);
}

void shell_ptt_set(bool down)
{
    if (down && voice_is_busy()) return; /* ignore hold while playing back */
    if (down == s_ptt_down) return;
    if (!down && s_wake_listen_timer) {
        lv_timer_delete(s_wake_listen_timer);
        s_wake_listen_timer = NULL;
    }
    s_ptt_down = down;
    shell_note_activity();
    if (down) {
        if (s_mood_timer) {
            lv_timer_delete(s_mood_timer);
            s_mood_timer = NULL;
        }
        if (s_ask_timer) {
            lv_timer_delete(s_ask_timer);
            s_ask_timer = NULL;
        }
        s_ask_waiting = false;
        if (s_tap_timer) {
            lv_timer_delete(s_tap_timer);
            s_tap_timer = NULL;
        }
        s_tap_busy = false;
        face_breathe_stop();
        if (s_face) {
            lv_anim_delete(s_face, NULL);
            face_reset_xform();
        }
        if (s_page != PAGE_HOME) show_page(PAGE_HOME);
        shell_set_mood("listening");
        voice_ptt_down();
        port_log("ptt", port_talk_toggle() ? "toggle on - listen" : "hold - listen");
    } else {
        voice_ptt_up();
        port_log("ptt", port_pair_is_paired() ? "release - ask" : "release - loopback");
        mood_after_ptt();
    }
}

void shell_ptt_toggle(void)
{
    if (!port_talk_toggle()) return;
    if (s_ptt_down) {
        shell_ptt_set(false);
    } else if (!voice_is_busy()) {
        shell_ptt_set(true);
    }
}

bool shell_ptt_is_down(void) { return s_ptt_down; }

static void anim_ty_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void anim_rot_cb(void *obj, int32_t v)
{
    lv_image_set_rotation((lv_obj_t *)obj, (int32_t)v);
}

static void anim_scale_cb(void *obj, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)obj, (uint32_t)v);
}

static void tap_done_cb(lv_timer_t *t)
{
    (void)t;
    s_tap_timer = NULL;
    s_tap_busy = false;
    face_reset_xform();
    if (!s_ptt_down) restore_base_mood();
}

static void pebble_tap_react(void)
{
    if (!s_face || s_page != PAGE_HOME || s_ptt_down) return;

    /* Interrupt breathe / prior bounce cleanly. */
    face_breathe_stop();
    lv_anim_delete(s_face, NULL);
    if (s_tap_timer) {
        lv_timer_delete(s_tap_timer);
        s_tap_timer = NULL;
    }
    if (s_mood_timer) {
        lv_timer_delete(s_mood_timer);
        s_mood_timer = NULL;
    }

    s_tap_busy = true;
    face_reset_xform();
    shell_set_mood(s_base_mood[0] ? s_base_mood : "content"); /* calm art under bounce */
    if (s_caption) {
        lv_label_set_text(s_caption, s_tap_lines[s_tap_line % (sizeof(s_tap_lines) / sizeof(s_tap_lines[0]))]);
        s_tap_line++;
    }

    /* Happy hop (smaller buddy — gentler travel) */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, 0, -18);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_reverse_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, anim_ty_cb);
    lv_anim_start(&a);

    /* Wiggle (rotation is 0.1 deg units) */
    lv_anim_t r;
    lv_anim_init(&r);
    lv_anim_set_var(&r, s_face);
    lv_anim_set_values(&r, -45, 45);
    lv_anim_set_duration(&r, 120);
    lv_anim_set_reverse_duration(&r, 120);
    lv_anim_set_repeat_count(&r, 2);
    lv_anim_set_path_cb(&r, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&r, anim_rot_cb);
    lv_anim_start(&r);

    /* Tiny pulse — keep near 256 so Bayer stays sharp */
    lv_anim_t s;
    lv_anim_init(&s);
    lv_anim_set_var(&s, s_face);
    lv_anim_set_values(&s, LV_SCALE_NONE, 268);
    lv_anim_set_duration(&s, 160);
    lv_anim_set_reverse_duration(&s, 220);
    lv_anim_set_path_cb(&s, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&s, anim_scale_cb);
    lv_anim_start(&s);

    s_tap_timer = lv_timer_create(tap_done_cb, TAP_MS, NULL);
    lv_timer_set_repeat_count(s_tap_timer, 1);
    voice_play_nudge(s_tap_line); /* rotate chirp with caption */
    port_log("shell", "tap - pebble liked that");
}

void shell_set_mood(const char *mood)
{
    if (!s_face) return;
    if (s_tap_busy && mood &&
        (strcmp(mood, "idle") == 0 || strcmp(mood, "content") == 0) && s_tap_timer)
        return; /* keep tap caption until bounce ends */

    const char *m = mood && mood[0] ? mood : "content";
    /* Unknown base mood → content (PROTOCOL). */
    if (strcmp(m, "happy") && strcmp(m, "content") && strcmp(m, "sleepy") &&
        strcmp(m, "curious") && strcmp(m, "worried") && strcmp(m, "upset") &&
        strcmp(m, "proud") && strcmp(m, "idle") && strcmp(m, "listening") &&
        strcmp(m, "thinking") && strcmp(m, "speaking")) {
        m = "content";
    }

    const lv_image_dsc_t *src = &img_pebble_idle;
    const char *cap = "All quiet";
    bool calm = true;

    if (strcmp(m, "listening") == 0) {
        src = &img_pebble_listening;
        cap = "Listening";
        calm = false;
    } else if (strcmp(m, "thinking") == 0) {
        src = &img_pebble_thinking;
        cap = "Thinking";
        calm = false;
    } else if (strcmp(m, "speaking") == 0) {
        src = &img_pebble_idle;
        cap = "Speaking";
        calm = false;
    } else if (strcmp(m, "worried") == 0 || strcmp(m, "upset") == 0) {
        src = &img_pebble_worried;
        cap = (strcmp(m, "upset") == 0) ? "Something's wrong" : "Needs you";
        calm = false;
    } else if (strcmp(m, "happy") == 0 || strcmp(m, "proud") == 0) {
        src = &img_pebble_idle; /* placeholder until art lands */
        cap = (strcmp(m, "proud") == 0) ? "Glad I could help" : "All good";
        calm = true;
    } else if (strcmp(m, "sleepy") == 0) {
        src = &img_pebble_idle;
        cap = "Quiet day";
        calm = true;
    } else if (strcmp(m, "curious") == 0) {
        src = &img_pebble_idle;
        cap = "Something new";
        calm = true;
    } else {
        /* content / idle */
        src = &img_pebble_idle;
        cap = "All quiet";
        calm = true;
        m = "content";
    }

    /* Remember Pi base mood (not ask overlays). */
    if (strcmp(m, "listening") && strcmp(m, "thinking") && strcmp(m, "speaking")) {
        snprintf(s_base_mood, sizeof(s_base_mood), "%s", m);
    }

    s_mood_idle = calm;
    lv_image_set_src(s_face, src);
    if (s_caption && !s_tap_busy) lv_label_set_text(s_caption, cap);
    else if (s_caption && !calm)
        lv_label_set_text(s_caption, cap);

    if (calm && !s_tap_busy && !s_ptt_down)
        face_breathe_start();
    else
        face_breathe_stop();
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (s_clock) {
        lv_label_set_text(s_clock, buf);
        lv_obj_t *shadow = (lv_obj_t *)lv_obj_get_user_data(s_clock);
        if (shadow) lv_label_set_text(shadow, buf);
    }
}

static void idle_tick(lv_timer_t *t)
{
    (void)t;
    if (s_keep_awake || s_timeout_s <= 0 || s_screen_off) return;
    uint32_t idle = port_millis() - s_last_activity_ms;
    if (idle >= (uint32_t)s_timeout_s * 1000u) shell_screen_off();
}

static void wake_listen_end_cb(lv_timer_t *t)
{
    (void)t;
    s_wake_listen_timer = NULL;
    if (s_ptt_down) shell_ptt_set(false);
}

static void wake_tick(lv_timer_t *t)
{
    (void)t;
    if (!wake_consume_trigger()) return;
    if (s_ptt_down || s_ask_waiting || voice_is_busy()) return;

    if (s_screen_off) shell_screen_on();
    if (s_page != PAGE_HOME) show_page(PAGE_HOME);
    shell_note_activity();

    /* Open a timed listen window (no finger on PTT). */
    int win_s = 8;
    pebble_pair_t p;
    if (port_pair_load(&p) && p.max_ask_s > 0) win_s = (int)p.max_ask_s;
    if (win_s < 3) win_s = 3;
    if (win_s > 15) win_s = 15;

    if (s_wake_listen_timer) {
        lv_timer_delete(s_wake_listen_timer);
        s_wake_listen_timer = NULL;
    }
    shell_ptt_set(true);
    s_wake_listen_timer = lv_timer_create(wake_listen_end_cb, (uint32_t)win_s * 1000u, NULL);
    lv_timer_set_repeat_count(s_wake_listen_timer, 1);
    port_log("shell", "wake → listen %ds", win_s);
}

static void net_mood_tick(lv_timer_t *t)
{
    (void)t;
    if (!port_pair_is_paired()) return;
    if (s_ptt_down || s_ask_waiting || voice_is_busy()) return;
    if (s_page != PAGE_HOME) return;

    /* Soft 403 notices (owner_disabled / away_disabled) — pairing kept. */
    char notice[64];
    if (pebble_net_take_notice(notice, sizeof(notice)) && notice[0]) {
        shell_set_mood("upset");
        if (s_caption) lv_label_set_text(s_caption, notice);
        if (s_mood_timer) {
            lv_timer_delete(s_mood_timer);
            s_mood_timer = NULL;
        }
        s_mood_timer = lv_timer_create(mood_voice_poll_cb, 2800, NULL);
        lv_timer_set_repeat_count(s_mood_timer, 1);
    }

    /* Apply last mood result if any, then kick next poll. */
    char mood[16];
    char cap[48];
    pebble_net_base_mood(mood, sizeof(mood), cap, sizeof(cap));
    if (mood[0] && strcmp(mood, s_base_mood) != 0) {
        snprintf(s_base_mood, sizeof(s_base_mood), "%s", mood);
        snprintf(s_base_caption, sizeof(s_base_caption), "%s", cap);
        if (!s_tap_busy) {
            shell_set_mood(mood);
            if (s_caption && cap[0]) lv_label_set_text(s_caption, cap);
        }
    } else if (cap[0] && strcmp(cap, s_base_caption) != 0) {
        snprintf(s_base_caption, sizeof(s_base_caption), "%s", cap);
        if (s_caption && s_mood_idle && !s_tap_busy)
            lv_label_set_text(s_caption, cap);
    }

    if (!pebble_net_mood_busy()) pebble_net_mood_kick();

    /* Reschedule based on config (floor 10 s). */
    int sec = pebble_net_mood_poll_s();
    if (sec < 10) sec = 10;
    if (s_net_mood_timer) lv_timer_set_period(s_net_mood_timer, (uint32_t)sec * 1000u);
}

static lv_obj_t *make_dot(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(COL_SAND), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_outline_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_set_style_shadow_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(d, LV_SCROLLBAR_MODE_OFF);
    return d;
}

static void row_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *id = (const char *)lv_event_get_user_data(e);
    if (id) settings_open_panel(id);
}

static void add_row(lv_obj_t *list, const char *label, const char *meta, const char *id, bool danger)
{
    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_width(btn, 280);
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, label);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lab, danger ? lv_color_hex(COL_DANGER) : lv_color_hex(COL_TEXT), 0);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 12, 0);

    if (meta && meta[0]) {
        lv_obj_t *m = lv_label_create(btn);
        lv_label_set_text(m, meta);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(COL_MUTED), 0);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    lv_obj_add_event_cb(btn, row_click, LV_EVENT_CLICKED, (void *)id);
}

static void fill_settings_rows(lv_obj_t *list)
{
    if (!list) return;
    lv_obj_clean(list);

    char wifi_meta[33] = "-";
    char pair_meta[40] = "Not paired";
    char batt_meta[8] = "-";

    port_wifi_ssid(wifi_meta, sizeof(wifi_meta));
    if (!wifi_meta[0]) snprintf(wifi_meta, sizeof(wifi_meta), "-");

    if (port_pair_is_paired()) {
        pebble_pair_t p;
        if (port_pair_load(&p) && p.nickname[0])
            snprintf(pair_meta, sizeof(pair_meta), "%s", p.nickname);
        else
            snprintf(pair_meta, sizeof(pair_meta), "Paired");
    }

    int pct = port_battery_percent();
    if (pct >= 0) snprintf(batt_meta, sizeof(batt_meta), "%d%%", pct);

    add_row(list, "Wi-Fi", wifi_meta, "wifi", false);
    add_row(list, "Pairing", pair_meta, "pairing", false);
    char sound_meta[16];
    if (!settings_sounds_on())
        snprintf(sound_meta, sizeof(sound_meta), "Muted");
    else
        snprintf(sound_meta, sizeof(sound_meta), "%d%%", settings_get_volume());
    add_row(list, "Sound", sound_meta, "sound", false);
    char sleep_meta[16];
    shell_timeout_label(sleep_meta, sizeof(sleep_meta));
    add_row(list, "Sleep", sleep_meta, "sleep", false);
    add_row(list, "Battery", batt_meta, "battery", false);
    add_row(list, "Power off", "", "power", true);
}

static void build_settings_list(lv_obj_t *root)
{
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_settings_list = lv_obj_create(root);
    lv_obj_set_size(s_settings_list, 300, 340);
    lv_obj_align(s_settings_list, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_opa(s_settings_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_settings_list, 0, 0);
    lv_obj_set_style_pad_all(s_settings_list, 0, 0);
    lv_obj_set_style_pad_row(s_settings_list, 6, 0);
    lv_obj_set_flex_flow(s_settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_settings_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_settings_list, LV_OBJ_FLAG_EVENT_BUBBLE);

    fill_settings_rows(s_settings_list);
}

static void drag_cb(lv_event_t *e); /* wired from home + settings pages */

static void wire_swipe(lv_obj_t *obj)
{
    /* Handlers on each page — root alone never sees presses on the face/list. */
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, drag_cb, LV_EVENT_RELEASED, NULL);
}

void shell_wire_panel_gestures(lv_obj_t *panel)
{
    if (panel) wire_swipe(panel);
}

static void drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_drag_x0 = p.x;
        s_drag_y0 = p.y;
        s_press_ms = port_millis();
        s_swiping = false;
        if (s_screen_off) {
            shell_note_activity();
            s_drag_x0 = -1;
            return;
        }
        shell_note_activity();
    } else if (code == LV_EVENT_PRESSING && s_drag_x0 >= 0 && !s_ptt_down) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = (int)p.x - (int)s_drag_x0;
        int dy = (int)p.y - (int)s_drag_y0;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx > PTT_MOVE_PX && adx >= ady) {
            s_swiping = true; /* horizontal wins — page swipe, not talk */
            return;
        }
        if (s_swiping || s_page != PAGE_HOME) return;
        /* Hold-to-talk only when not in toggle mode. */
        if (port_talk_toggle()) return;
        if (adx * adx + ady * ady > PTT_MOVE_PX * PTT_MOVE_PX) return;
        if (port_millis() - s_press_ms >= PTT_HOLD_MS) {
            s_ptt_from_touch = true;
            shell_ptt_set(true);
        }
    } else if (code == LV_EVENT_RELEASED) {
        int dx = 0;
        uint32_t held = port_millis() - s_press_ms;
        if (s_drag_x0 >= 0) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            dx = (int)p.x - (int)s_drag_x0;
        }
        bool was_swipe = s_swiping;
        s_drag_x0 = -1;
        s_swiping = false;
        /* Hold mode: end face-hold PTT on lift. */
        if (s_ptt_from_touch) {
            s_ptt_from_touch = false;
            if (s_ptt_down && !port_talk_toggle()) shell_ptt_set(false);
            return;
        }
        if (was_swipe || dx <= -SWIPE_PX || dx >= SWIPE_PX) {
            if (dx <= -SWIPE_PX) show_page(PAGE_SETTINGS);
            else if (dx >= SWIPE_PX) show_page(PAGE_HOME);
            return;
        }
        if (s_page != PAGE_HOME) return;
        if (held < PTT_HOLD_MS) {
            if (port_talk_toggle()) {
                /* Press to start / press to stop. */
                shell_ptt_toggle();
            } else if (!s_ptt_down) {
                pebble_tap_react();
            }
        }
    }
}

static void dot_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    show_page(page);
}

static void back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    void (*fn)(void) = (void (*)(void))lv_event_get_user_data(e);
    if (fn) fn();
}

void shell_start(void)
{
    /* Replay-safe: splash BOOT just reloads the existing shell. */
    if (s_scr) {
        settings_close_panel();
        shell_screen_on();
        shell_ptt_set(false);
        restore_base_mood();
        lv_screen_load(s_scr);
        show_page(PAGE_HOME);
        shell_note_activity();
        clock_tick(NULL);
        return;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_UI), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_screen_load(s_scr);

    /* Home — full-bleed character like the HTML mockup */
    s_home = lv_obj_create(s_scr);
    lv_obj_set_size(s_home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home, 0, 0);
    lv_obj_set_style_pad_all(s_home, 0, 0);
    /* No clip_corner circle — masking a 466px photo every frame stalls the PC sim. */
    wire_swipe(s_home);

    s_face = lv_image_create(s_home);
    lv_image_set_src(s_face, &img_pebble_idle);
    lv_obj_center(s_face);
    lv_obj_add_flag(s_face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_CLICKABLE);

    /* Shadow under clock so it stays readable on the face. */
    lv_obj_t *clock_shadow = lv_label_create(s_home);
    lv_label_set_text(clock_shadow, "--:--");
    lv_obj_set_style_text_font(clock_shadow, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(clock_shadow, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_opa(clock_shadow, LV_OPA_50, 0);
    lv_obj_align(clock_shadow, LV_ALIGN_TOP_MID, 1, 29);
    s_clock = lv_label_create(s_home);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_clock, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_user_data(s_clock, clock_shadow);

    /* Status under the clock — keeps feet clear of chrome. */
    s_caption = lv_label_create(s_home);
    lv_label_set_text(s_caption, "All quiet");
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_color(s_caption, lv_color_hex(0x0B0D0E), 0);
    lv_obj_set_style_bg_opa(s_caption, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_caption, 0, 0);
    lv_obj_set_style_pad_hor(s_caption, 10, 0);
    lv_obj_set_style_pad_ver(s_caption, 3, 0);
    lv_obj_set_style_radius(s_caption, 12, 0);
    lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_flag(s_caption, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_caption, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);

    /* Settings hub */
    s_settings = lv_obj_create(s_scr);
    lv_obj_set_size(s_settings, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(COL_UI), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 0, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
    wire_swipe(s_settings);
    build_settings_list(s_settings);

    lv_obj_t *dots = lv_obj_create(s_scr);
    lv_obj_set_size(dots, 40, 16);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_outline_width(dots, 0, 0);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(dots, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 10, 0);

    s_dot0 = make_dot(dots);
    s_dot1 = make_dot(dots);
    lv_obj_add_flag(s_dot0, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dot0, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_add_event_cb(s_dot1, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    show_page(PAGE_HOME);
    shell_set_mood("content");
    shell_note_activity();
    clock_tick(NULL);
    lv_timer_create(clock_tick, 15000, NULL);
    lv_timer_create(idle_tick, 1000, NULL);
    lv_timer_create(wake_tick, 100, NULL);
    s_net_mood_timer = lv_timer_create(net_mood_tick, 30000, NULL);
    buttons_start();
    wake_start();

    port_log("shell", "home ready");
}

lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void))
{
    /* Soft chevron at 9 o'clock — no heavy chip. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ROW), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_opa(lab, LV_OPA_70, 0);
    lv_obj_center(lab);

    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, (void *)action);
    return btn;
}
