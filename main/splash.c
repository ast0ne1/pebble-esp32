/* Friendly boot — full-bleed plate (Muse-style native RGB565) + LVGL type. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"

#define SPLASH_MS 3200

static lv_obj_t *s_splash;
static lv_timer_t *s_timer;

static void done_cb(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;
    lv_obj_t *old = s_splash;
    s_splash = NULL;
    shell_start();
    if (old) lv_obj_delete(old);
}

void splash_show(void)
{
    shell_screen_on();

    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_splash) {
        lv_obj_delete(s_splash);
        s_splash = NULL;
    }

    s_splash = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(0x0B0D0E), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_screen_load(s_splash);

    /* One native 466×466 plate: radial + AA cairn + soft buddy (Bayer RGB565). */
    lv_obj_t *plate = lv_image_create(s_splash);
    lv_image_set_src(plate, &img_pebble_splash);
    lv_obj_center(plate);

    /* Type sits in the band between mark and buddy on the plate. */
    lv_obj_t *hello = lv_label_create(s_splash);
    lv_label_set_text(hello, "Hi - I'm Pebble");
    lv_obj_set_style_text_font(hello, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hello, lv_color_hex(0xF4F1EA), 0);
    lv_obj_set_style_text_align(hello, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hello, LV_ALIGN_TOP_MID, 0, 168);

    lv_obj_t *sub = lv_label_create(s_splash);
    lv_label_set_text(sub, "StonePi's sidekick");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xC7BBA8), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 200);

    s_timer = lv_timer_create(done_cb, SPLASH_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}
