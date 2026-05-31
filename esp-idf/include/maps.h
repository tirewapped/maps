/**
 * maps — on-device offline map viewer (T-Deck Plus LCD).
 *
 * A launcher program that draws pre-baked RGB565 slippy-map tiles read from the
 * SD card (default /sdcard/maps/<z>/<x>/<y>.bin), centred on the live GPS fix
 * (gps.* ephemeral vars) with a position marker. Tiles are generated on a
 * computer with scripts/maketiles.py — nothing is rendered or fetched on the
 * device. Gated on CONFIG_SPANGAP_LCD; mapsInit() is a no-op without it.
 *
 * Config:    s.maps.zoom (slippy zoom), s.maps.tiledir (SD path)
 * Ephemeral: maps.state
 */
#pragma once
/* mapsInit() is the straddle's `init:` hook (see straddle.yaml) — the generated
 * spangapInitStraddles() dispatcher calls it automatically. It self-registers
 * the "Maps" launcher program + Settings pane (gated on CONFIG_SPANGAP_LCD), so
 * there is no separate registration entry point and no consumer wiring. */
void mapsInit(void);          /* worker task + CLI + storage defaults + LCD launcher */
