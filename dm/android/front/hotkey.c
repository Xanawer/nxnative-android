// Controller hotkeys of the Android front end. Select (−) + right stick press toggles the operation mode, handheld ↔ docked (hle_oe.c dm_set_opmode: the game gets the
// notification a dock change sends, and a game that renders by mode switches resolution: Paper Mario: The Thousand-Year Door renders docked at 1600x900 into a 1920x1080
// composite, its handheld mode fewer pixels, which the RG 476H's GPU needs). The start mode comes from DM_OPMODE (env.txt: 0 handheld, 1 docked, else the profile's).
// Off unless DM_HOTKEY_OPMODE=1 (env.txt): another game may use the combination. Docs: docs/android/switches.md
#include "dm.h"
#include "hle.h"
#include "dm_input.h"
#include <stdlib.h>

// Called by the key handler with the buttons player 1's controller holds now
void dm_hotkeys(uint64_t held) {
    static int on = -1, armed = 1;
    if (on < 0) { const char *e = getenv("DM_HOTKEY_OPMODE"); on = e && *e == '1'; }
    if (!on) return;
    const uint64_t combo = DM_BTN_MINUS | DM_BTN_RS;
    if ((held & combo) != combo) { armed = 1; return; }
    if (!armed) return;   // once per press of the combination
    armed = 0;
    int m = dm_opmode() ? 0 : 1;
    LOG("hotkey Select + R3: operation mode → %s", m ? "docked" : "handheld");
    dm_set_opmode(m);
}
