/**
 * maps_app.h — the Maps launcher program as a boot-registered Service.
 *
 * MapsApp is an LcdApp (hence a Service): the straddle's `services:` entry
 * points the generated boot code at this header, which constructs a MapsApp and
 * registers it. LcdApp::onInit installs its launcher tile; appInit() does the
 * boot-task wiring (cache mutex, `maps` CLI verb, render worker).
 *
 * The class is declared here (global, no namespace — the codebase disambiguates
 * by the maps* symbol prefix) so the trampoline TU can `new MapsApp()`; the
 * methods are defined out-of-line in maps_lcd.cpp, where the file-static viewer
 * state (tile cache, worker, canvas) lives. Compiled only under
 * conditional/spangap-lcd/, so it exists only when the lcd straddle is staged —
 * matching the entry's `when: spangap/spangap-lcd` gate.
 */
#pragma once

#include "lcd_app.h"   /* LcdApp (a Service) */
#include "lvgl.h"      /* lv_obj_t */

/** The Maps viewer. onCreate builds the LVGL canvas once; onShow (re)arms
 *  multi-touch + the render worker on every open; onClose tears the canvas down
 *  so a later reopen rebuilds. appInit() (boot task) wires the cache mutex, the
 *  `maps` CLI verb, and spawns the render worker. */
class MapsApp : public LcdApp {
public:
    MapsApp();
    void onCreate(lv_obj_t* root) override;
    void onShow() override;
    void onClose() override;

protected:
    void appInit() override;
};
