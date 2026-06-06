/**
 * maps — on-device offline map viewer.
 *
 * The whole maps feature is the on-device LCD viewer (render worker, LVGL
 * canvas, CLI verb, Settings pane). That code lives in the LCD slice
 * (conditional/spangap-lcd/src/maps_lcd.cpp) and registers via the when:-gated
 * mapsLcdRegister hook — compiled in only when the lcd straddle is staged.
 *
 * This translation unit holds the straddle's normal init hook, mapsInit(),
 * which has no work of its own: in an LCD build mapsLcdRegister() does the
 * setup; in a non-LCD build there is nothing to do (no display, no map). It is
 * kept so the generated spangapInitStraddles() dispatcher always has a hook to
 * call regardless of staging.
 *
 * Config:    s.maps.zoom, s.maps.tiledir
 * Ephemeral: maps.state   ("ready" | "no tiles" | "no fix" | "no sd")
 */
#include "maps.h"

/* No-op: all maps work is the LCD viewer, registered by mapsLcdRegister (the
 * spangap/spangap-lcd when:-gated hook) in conditional/spangap-lcd/. */
void mapsInit(void) {}
