#pragma once
#include "lvgl.h"

void splash_show(void);
void shell_start(void);           /* after splash — home ↔ settings */
void buttons_start(void);         /* BOOT / PWR (device); no-op on sim */
void shell_show_home(void);
void shell_show_settings(void);
void shell_goto_page(int page);   /* 0 home, 1 settings */
int  shell_page(void);
/* Base (Pi): happy|content|sleepy|curious|worried|upset|proud.
 * Overlays (local, during an ask): listening|thinking|speaking.
 * Art today: idle (content), listening, thinking, worried. */
void shell_set_mood(const char *mood);

/* Push-to-talk: hold (default) or toggle when config.talk.toggle is on. */
void shell_ptt_set(bool down);
void shell_ptt_toggle(void); /* press-to-start / press-to-stop */
bool shell_ptt_is_down(void);

void settings_load(void);
void settings_open_panel(const char *id);  /* wifi | pairing | sound | sleep | timezone | battery | power */
void settings_close_panel(void);
void shell_refresh_settings(void); /* rebuild hub row meta (Sound %, Sleep, …) */
/* Swipe gestures on a full-screen settings panel (layer_top). */
void shell_wire_panel_gestures(lv_obj_t *panel);
int  settings_get_volume(void);   /* 0..100 */
bool settings_sounds_on(void);    /* chirps / UI sounds */
void settings_tz_label(char *out, size_t n); /* short city name for Settings row */

lv_obj_t *ui_back_btn(lv_obj_t *parent, void (*action)(void));

/* Auto screen-off seconds; 0 = off (never). Presets: 3/5/10/30 min. */
void shell_set_timeout(int seconds);
int  shell_get_timeout(void);
void shell_timeout_label(char *out, size_t n); /* "Off" / "3 min" / … */
void shell_timeout_cycle(void);                 /* Off → 3 → 5 → 10 → 30 → Off */
void shell_note_activity(void);                 /* reset idle timer; wake if dark */
void shell_keep_awake(bool on);
void shell_screen_off(void);
void shell_screen_on(void);
bool shell_screen_is_off(void);
