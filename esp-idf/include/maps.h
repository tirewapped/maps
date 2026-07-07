/**
 * maps — on-device offline map viewer (T-Deck Plus LCD).
 *
 * A launcher program that draws pre-baked slippy-map tiles read from the SD
 * card (default /sdcard/maps/<z>/<x>/<y>.jpg, raw .bin fallback), centred on
 * the live GPS fix (gps.* ephemeral vars) with a position marker. Tiles are
 * baked on a computer with the tilebake toolchain — nothing is rendered or
 * fetched on the device. The viewer lives in the LCD slice
 * (conditional/spangap-lcd/) and is brought up as the MapsApp `services:` entry
 * (straddle.yaml), when:-gated on spangap-lcd.
 *
 * Config:    s.maps.zoom (slippy zoom), s.maps.tiledir (SD path)
 * Ephemeral: maps.state
 */
#pragma once
/* No public firmware API: the maps program is the MapsApp Service (see
 * conditional/spangap-lcd/include/maps_app.h), constructed by the generated
 * services: trampoline. Nothing outside the straddle calls into maps. */
