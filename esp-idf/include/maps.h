/**
 * maps — on-device offline map viewer (T-Deck Plus LCD).
 *
 * A launcher program that draws pre-baked RGB565 slippy-map tiles read from the
 * SD card (default /sdcard/maps/<z>/<x>/<y>.bin), centred on the live GPS fix
 * (gps.* ephemeral vars) with a position marker. Tiles are generated on a
 * computer with scripts/maketiles.py — nothing is rendered or fetched on the
 * device. The viewer lives in the LCD slice (conditional/spangap-lcd/) and
 * registers via the when:-gated mapsLcdRegister hook; mapsInit() is a no-op.
 *
 * Config:    s.maps.zoom (slippy zoom), s.maps.tiledir (SD path)
 * Ephemeral: maps.state
 */
#pragma once
/* mapsInit() is the straddle's `init:` hook (see straddle.yaml) — the generated
 * spangapInitStraddles() dispatcher calls it automatically. It is a no-op: the
 * map viewer (worker task + CLI + storage defaults + LCD launcher) registers via
 * the when:-gated mapsLcdRegister hook in the conditional/spangap-lcd/ slice. */
void mapsInit(void);          /* no-op; viewer registered by mapsLcdRegister */
