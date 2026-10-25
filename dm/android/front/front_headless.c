// Headless Android front end (front.h; docs/android/PLAN.md 1.6): front_mac.m with DM_WINDOW unset. No window, no input, no text box; the guest runs on the calling thread.
// The app shell (window, touch overlay, JNI) replaces it in a later phase; front_save_failed, dm_thermal_now, dm_low_power_now and dm_brightness_now keep their weak host defaults
#include "../../host/dm.h"
#include "../../host/front.h"

void front_run(void (*guest)(void)) { guest(); }   // as front_mac.m without DM_WINDOW: main.c's thread runs the guest, and exits when it returns
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }        // no screen known
void front_screen_match(int fps) { (void)fps; }
// No window: nothing is shown and -1 is returned (front.h); the software keyboard takes it as a cancel
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    return -1;
}
