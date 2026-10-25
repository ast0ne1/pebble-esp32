#pragma once
#include <stdbool.h>

/* Optional wake word (ESP-SR). Config from GET /config talk.wake / wake_phrase.
 *
 * When enabled: AFE + WakeNet ("Hi ESP") and MultiNet for the custom phrase.
 * Detection sets a latch consumed on the LVGL thread (shell). Pause while the
 * voice path holds the mic/speaker. */

void wake_start(void);
void wake_stop(void);
bool wake_enabled(void);
void wake_on_config_changed(void); /* after GET /config applies talk.* */

/* Voice path: release mic before PTT/play; reclaim when idle. */
void wake_pause(void);
void wake_resume(void);

/* Shell: true once per detection (clears latch). */
bool wake_consume_trigger(void);
