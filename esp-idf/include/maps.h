/**
 * maps — on-device offline map viewer (T-Deck Plus LCD).
 *
 * A launcher program that draws pre-baked RGB565 slippy-map tiles read from the
 * SD card (default /sdcard/maps/<z>/<x>/<y>.bin), centred on the live GPS fix
 * (gps.* ephemeral vars) with a position marker. Tiles are generated on a
 * computer with scripts/maketiles.py — nothing is rendered or fetched on the
 * device. Gated on CONFIG_SPANGAP_LCD; both calls below are no-ops without it.
 *
 * Config:    s.maps.zoom (slippy zoom), s.maps.tiledir (SD path)
 * Ephemeral: maps.state
 */
#pragma once
void mapsInit(void);          /* worker task + CLI + storage defaults */
void mapsLcdRegister(void);   /* register the "Maps" launcher program */
