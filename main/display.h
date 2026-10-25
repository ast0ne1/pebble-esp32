#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Our own display bring-up instead of the BSP's. Holding the panel handle is
 * what makes it possible to really turn the screen off (the BSP does not hand
 * the handle over). */
bool        pebble_display_start(void);
lv_indev_t *pebble_display_indev(void);
void        pebble_display_rotate180(void);
void        pebble_display_brightness(int percent);   /* 0 = pixels off */
int         pebble_display_brightness_get(void);      /* 0 is not remembered */
void        pebble_display_on(bool on);
/* The x margin when flipped 180 degrees. Wrong, and a coloured band is left at
 * the edge. There is no documentation for the controller's column count, so it
 * is chosen in settings. */
void        pebble_display_set_xgap(int gap);
int         pebble_display_get_xgap(void);               /* turns the panel itself on and off (0x28/0x29) */
