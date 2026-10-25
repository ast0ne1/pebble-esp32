/*
 * Phone-style keypad adapted from Muse Gadget (Meta Platforms, Inc.)
 * Apache-2.0 — see NOTICE.
 *
 * Twelve big multi-tap keys for small round screens. Tap again within a
 * second to step characters; hold a letter key for its digit.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* Types into `ta`. Done sends LV_EVENT_READY on the keypad. On a round screen
 * the bottom row keeps in from the sides where the circle cuts off. */
lv_obj_t *keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round);

/* Back to lower-case letters. password=true shows a Show/Hide key. */
void keypad_reset(lv_obj_t *kp, bool password);

/* Switch to digit pad (e.g. host last octet after a filled http://a.b.c.). */
void keypad_use_digits(lv_obj_t *kp);
