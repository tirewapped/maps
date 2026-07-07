/**
 * maps — on-device offline map viewer.
 *
 * The whole maps feature is the on-device LCD viewer (render worker, LVGL
 * canvas, CLI verb, Settings pane), which lives in the LCD slice
 * (conditional/spangap-lcd/src/maps_lcd.cpp) and is brought up as a
 * boot-registered Service: the MapsApp `services:` entry (straddle.yaml),
 * when:-gated on spangap-lcd, so it compiles in only when the lcd straddle is
 * staged. There is nothing to do in a non-LCD build (no display, no map), and
 * no browser UI — so this base translation unit carries no code of its own. It
 * exists only to keep the maps component's SRCS non-empty in a headless build.
 *
 * Config:    s.maps.zoom, s.maps.tiledir
 * Ephemeral: maps.state   ("ready" | "no tiles" | "no fix" | "no sd")
 */
#include "maps.h"
