/**
 * maps — on-device offline map viewer.
 *
 * Draws pre-baked RGB565 slippy-map tiles read off the SD card, with a GPS
 * position marker. Nothing is rendered or fetched on the device: tiles are
 * produced on a computer (scripts/maketiles.py) and copied to the SD card as
 * /sdcard/maps/<z>/<x>/<y>.jpg (decoded on the worker via esp_jpeg) or .bin
 * (raw 256x256 little-endian RGB565 — LVGL's native canvas order, byte-swapped
 * for the ST7789 on flush). .jpg is tried first, .bin is the fallback.
 *
 * Features:
 *   - Follows the GPS fix, or free-pan (drag / trackball-drag) with a
 *     "centre me" button to re-lock to GPS.
 *   - Overzoom fallback: a missing tile at the display zoom is filled by
 *     upscaling the nearest existing lower-zoom ancestor, so a coarse base map
 *     "shows through" where local detail is absent instead of going grey.
 *
 * Split of work:
 *   - worker task (this file): owns the tile cache, the slippy-map math, the
 *     view centre / follow state. Subscribes to gps.* and s.maps.*; the lcd
 *     task feeds it pan deltas + a recentre flag. On any change it fetches the
 *     visible tiles (or ancestors) from SD into a PSRAM LRU cache, then hands a
 *     composite request to the lcd task.
 *   - lcd task: owns the LVGL canvas + the centre-me button + the drag handler.
 *     composite() blits cached tiles (scaled for overzoom) into the canvas,
 *     draws the marker, toggles a status label.
 * The cache is shared (s_mux); pan/recentre flags are shared (s_ctrlMux).
 *
 * Config:    s.maps.zoom, s.maps.tiledir
 * Ephemeral: maps.state   ("ready" | "no tiles" | "no fix" | "no sd")
 */
#include "maps.h"
#include "spangap.h"

#if CONFIG_SPANGAP_LCD

#include "lcd.h"          /* pulls in lvgl.h */
#include "fs.h"
#include "storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"

#include <sys/stat.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char* TAG = "maps";

#define MAPS_VERSION 1
#define TILE         256
#define TILE_BYTES   (TILE * TILE * 2)
#define MAPS_CACHE   16         /* display tiles + overzoom ancestors + pan margin */
#define MAPS_MAXK    6          /* max ancestor levels for overzoom fallback */
#define ZOOM_MIN     1
#define ZOOM_MAX     19

/* ─────────────── worker/config state (worker task owns) ─────────────── */

static TaskHandle_t  s_worker = nullptr;
static volatile bool s_dirty  = true;
static volatile bool s_open   = false;   /* set on the lcd task in mapsApp */

static double s_lat = 0, s_lon = 0;      /* last GPS fix */
static bool   s_haveCenter = false;
static double s_viewLat = 0, s_viewLon = 0;   /* map centre (== GPS when following) */
static bool   s_haveView = false;
static bool   s_follow = true;

/* control flags: lcd task -> worker */
static portMUX_TYPE s_ctrlMux = portMUX_INITIALIZER_UNLOCKED;
static long         s_panDx = 0, s_panDy = 0;   /* accumulated drag, screen px */
static bool         s_recenter = false;

/* ─────────────── lcd-task-owned widgets (set in mapsApp) ─────────────── */

static lv_obj_t* s_canvas    = nullptr;
static lv_obj_t* s_label     = nullptr;
static uint16_t* s_canvasBuf = nullptr;
static int       s_W = 0, s_H = 0;       /* canvas size, px */
static int       s_stridePx  = 0;        /* row pitch, uint16 units */

/* ─────────────── tile cache (shared, guarded by s_mux) ─────────────── */

struct TileBuf { int z, x, y; bool valid; uint16_t* px; uint32_t lru; };
static TileBuf          s_cache[MAPS_CACHE] = {};
static SemaphoreHandle_t s_mux = nullptr;
static uint32_t          s_lruClock = 0;

/* composite request, heap-passed to the lcd task */
enum { MAPS_OK = 0, MAPS_NOFIX, MAPS_NOTILES, MAPS_NOSD };
struct Composite { long tlx, tly; int z; int state; long markerX, markerY; bool haveFix; };

/* ─────────────── slippy-map math ─────────────── */

/* lon/lat (deg) -> global pixel coordinates at zoom z (256 px/tile). */
static void lonlatToGlobalPx(double lon, double lat, int z, double* gx, double* gy) {
    double n  = (double)(1u << z);
    double xf = (lon + 180.0) / 360.0 * n;
    double lr = lat * M_PI / 180.0;
    double yf = (1.0 - log(tan(lr) + 1.0 / cos(lr)) / M_PI) / 2.0 * n;
    *gx = xf * TILE;
    *gy = yf * TILE;
}

/* inverse: global pixel -> lon/lat (deg). */
static void globalPxToLonLat(double gx, double gy, int z, double* lon, double* lat) {
    double n = (double)(1u << z);
    *lon = gx / TILE / n * 360.0 - 180.0;
    double m = M_PI * (1.0 - 2.0 * (gy / TILE / n));
    *lat = atan(sinh(m)) * 180.0 / M_PI;
}

/* ─────────────── tile cache (worker task) ─────────────── */

static bool tileInCache(int z, int x, int y) {
    bool hit = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (auto& t : s_cache)
        if (t.valid && t.z == z && t.x == x && t.y == y) { t.lru = ++s_lruClock; hit = true; break; }
    xSemaphoreGive(s_mux);
    return hit;
}

/* Decode a JPEG tile file into px (256x256 RGB565). false if missing/bad. */
static bool decodeJpg(const char* path, uint16_t* px) {
    struct stat st;
    if (fs_stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 256 * 1024) return false;
    size_t jlen = (size_t)st.st_size;
    uint8_t* jbuf = (uint8_t*)heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM);
    if (!jbuf) return false;
    int f = fs_open(path, "rb");
    bool rok = (f >= 0) && (fs_read(jbuf, 1, jlen, f) == jlen);
    if (f >= 0) fs_close(f);
    if (!rok) { free(jbuf); return false; }

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata      = jbuf;
    cfg.indata_size = jlen;
    cfg.outbuf      = (uint8_t*)px;
    cfg.outbuf_size = TILE_BYTES;
    cfg.out_format  = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale   = JPEG_IMAGE_SCALE_0;
    cfg.flags.swap_color_bytes = 0;   /* little-endian to match the LVGL canvas */
    esp_jpeg_image_output_t out = {};
    esp_err_t e = esp_jpeg_decode(&cfg, &out);
    free(jbuf);
    if (e != ESP_OK) {
        /* The file exists and was read fine, but the decoder rejected it — without
         * this, that looks identical to "no tiles for this area". esp_jpeg
         * (TJpgDec) decodes baseline JPEG only: progressive / arithmetic-coded /
         * CMYK / 12-bit tiles fail here. Throttled, because a screenful of bad
         * tiles is retried every GPS update (decode failures aren't cached). */
        static bool     warned = false;
        static uint32_t lastMs = 0;
        uint32_t now = millis();
        if (!warned || now - lastMs > 30000) {
            warn("%s: JPEG decode failed (%s) — esp_jpeg/TJpgDec is baseline-only "
                 "(progressive/arithmetic/CMYK/12-bit rejected); re-encode tiles as "
                 "baseline JPEG\n", path, esp_err_to_name(e));
            warned = true; lastMs = now;
        }
        return false;
    }
    return true;
}

/* Load (z,x,y) into the cache: tries <z>/<x>/<y>.jpg (decoded), then .bin (raw).
 * Returns true if present (cached or just loaded), false if missing / read /
 * alloc / decode failed. File I/O + decode happen outside the lock; only the
 * slot swap is locked. */
static bool ensureTile(const char* dir, int z, int x, int y) {
    if (tileInCache(z, x, y)) return true;

    uint16_t* px = (uint16_t*)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    if (!px) return false;

    char path[192];
    snprintf(path, sizeof(path), "%s/%d/%d/%d.jpg", dir, z, x, y);
    bool ok = decodeJpg(path, px);
    if (!ok) {
        snprintf(path, sizeof(path), "%s/%d/%d/%d.bin", dir, z, x, y);
        int f = fs_open(path, "rb");
        if (f >= 0) { ok = (fs_read(px, 1, TILE_BYTES, f) == TILE_BYTES); fs_close(f); }
    }
    if (!ok) { free(px); return false; }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    int slot = 0; uint32_t best = UINT32_MAX;
    for (int i = 0; i < MAPS_CACHE; i++) {
        if (!s_cache[i].valid) { slot = i; break; }
        if (s_cache[i].lru < best) { best = s_cache[i].lru; slot = i; }
    }
    if (s_cache[slot].valid && s_cache[slot].px) free(s_cache[slot].px);
    s_cache[slot] = { z, x, y, true, px, ++s_lruClock };
    xSemaphoreGive(s_mux);
    return true;
}

/* Cover one display cell: the exact tile if present, else the nearest existing
 * lower-zoom ancestor (overzoom fallback). */
static bool ensureCell(const char* dir, int z, int x, int y) {
    for (int k = 0; k <= MAPS_MAXK && z - k >= 0; k++)
        if (ensureTile(dir, z - k, x >> k, y >> k)) return true;
    return false;
}

/* ─────────────── compositing (lcd task) ─────────────── */

static inline void putPx(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= s_W || y >= s_H) return;
    s_canvasBuf[(size_t)y * s_stridePx + x] = c;
}

/* Blit a sub-region (sx,sy,sw,sh) of a 256-wide tile into the canvas at
 * (destX,destY), nearest-neighbour upscaled by `scale`, clipped to the canvas.
 * scale==1 is the normal 1:1 path (memcpy per row). */
static void blitScaled(const uint16_t* px, int sx, int sy, int sw, int sh,
                       long destX, long destY, int scale) {
    long dw = (long)sw * scale, dh = (long)sh * scale;
    long dx0 = destX < 0 ? 0 : destX;
    long dy0 = destY < 0 ? 0 : destY;
    long dx1 = (destX + dw < s_W) ? destX + dw : s_W;
    long dy1 = (destY + dh < s_H) ? destY + dh : s_H;
    if (dx1 <= dx0 || dy1 <= dy0) return;
    for (long dy = dy0; dy < dy1; dy++) {
        int srow = sy + (int)((dy - destY) / scale);
        const uint16_t* s = px + (size_t)srow * TILE;
        uint16_t* d = s_canvasBuf + (size_t)dy * s_stridePx;
        if (scale == 1) {
            memcpy(d + dx0, s + sx + (dx0 - destX), (size_t)(dx1 - dx0) * 2);
        } else {
            for (long dx = dx0; dx < dx1; dx++)
                d[dx] = s[sx + (int)((dx - destX) / scale)];
        }
    }
}

static void drawMarker(int cx, int cy) {
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= 25) putPx(cx + dx, cy + dy, 0xFFFF);
            if (d2 <= 9)  putPx(cx + dx, cy + dy, 0xF800);
        }
}

static void composite(void* arg) {
    Composite* c = (Composite*)arg;
    if (!s_canvas || !s_canvasBuf) { free(c); return; }

    const uint16_t BG = 0x8410;   /* mid-grey */
    for (int y = 0; y < s_H; y++) {
        uint16_t* row = s_canvasBuf + (size_t)y * s_stridePx;
        for (int x = 0; x < s_W; x++) row[x] = BG;
    }

    int present = 0;
    if (c->state == MAPS_OK) {
        int z = c->z, n = 1 << z;
        int tx0 = (int)floor(c->tlx / (double)TILE), tx1 = (int)floor((c->tlx + s_W - 1) / (double)TILE);
        int ty0 = (int)floor(c->tly / (double)TILE), ty1 = (int)floor((c->tly + s_H - 1) / (double)TILE);
        xSemaphoreTake(s_mux, portMAX_DELAY);
        for (int tx = tx0; tx <= tx1; tx++)
            for (int ty = ty0; ty <= ty1; ty++) {
                if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;
                long destX = (long)tx * TILE - c->tlx;
                long destY = (long)ty * TILE - c->tly;
                /* exact tile, else nearest cached ancestor upscaled */
                for (int k = 0; k <= MAPS_MAXK && z - k >= 0; k++) {
                    int az = z - k, ax = tx >> k, ay = ty >> k;
                    const uint16_t* px = nullptr;
                    for (auto& t : s_cache)
                        if (t.valid && t.px && t.z == az && t.x == ax && t.y == ay) { px = t.px; break; }
                    if (!px) continue;
                    int sub = TILE >> k;
                    int sx = (tx & ((1 << k) - 1)) * sub;
                    int sy = (ty & ((1 << k) - 1)) * sub;
                    blitScaled(px, sx, sy, sub, sub, destX, destY, 1 << k);
                    present++;
                    break;
                }
            }
        xSemaphoreGive(s_mux);

        if (c->haveFix && c->markerX >= 0 && c->markerX < s_W && c->markerY >= 0 && c->markerY < s_H)
            drawMarker((int)c->markerX, (int)c->markerY);
    }

    const char* msg = nullptr;
    char nofix[112];
    if (c->state == MAPS_NOSD) {
        msg = "No SD card";
    } else if (c->state == MAPS_NOFIX) {
        /* Acquisition screen: nudge the user to improve the sky view, with the live
         * satellite count + best signal so progress toward a fix is visible. */
        snprintf(nofix, sizeof nofix,
                 "Make the device see more of the sky\n\n"
                 "Satellites seen: %d\nSignal to noise: %d",
                 storageGetInt("gps.sats_view", 0), storageGetInt("gps.snr", 0));
        msg = nofix;
    } else if (c->state == MAPS_NOTILES || (c->state == MAPS_OK && present == 0)) {
        msg = "No tiles here\n(copy to /sdcard/maps)";
    }
    if (s_label) {
        if (msg) { lv_label_set_text(s_label, msg); lv_obj_clear_flag(s_label, LV_OBJ_FLAG_HIDDEN); }
        else     { lv_obj_add_flag(s_label, LV_OBJ_FLAG_HIDDEN); }
    }

    lv_obj_invalidate(s_canvas);
    free(c);
}

/* ─────────────── rebuild (worker task) ─────────────── */

static void rebuild(void) {
    if (!s_open) return;   /* program not on screen yet — nothing to draw */

    int zoom = storageGetInt("s.maps.zoom", 15);
    if (zoom < ZOOM_MIN) zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    std::string dir = storageGetStr("s.maps.tiledir", "/sdcard/maps");

    std::string la = storageGetStr("gps.lat", "");
    std::string lo = storageGetStr("gps.lon", "");
    bool fix = !la.empty() && !lo.empty();
    if (fix) {
        s_lat = strtod(la.c_str(), nullptr);
        s_lon = strtod(lo.c_str(), nullptr);
        s_haveCenter = true;
    }

    /* consume pan / recentre from the lcd task */
    long dx, dy; bool recenter;
    portENTER_CRITICAL(&s_ctrlMux);
    dx = s_panDx; dy = s_panDy; s_panDx = 0; s_panDy = 0;
    recenter = s_recenter; s_recenter = false;
    portEXIT_CRITICAL(&s_ctrlMux);
    if (recenter)            s_follow = true;
    if ((dx || dy) && s_haveView) s_follow = false;

    Composite* c = (Composite*)malloc(sizeof(Composite));
    if (!c) return;
    *c = {};
    c->z = zoom; c->markerX = -1; c->markerY = -1; c->haveFix = fix;

    if (!sdAvailable()) { c->state = MAPS_NOSD;  storageSet("maps.state", "no sd");  lcdRun(composite, c); return; }
    if (!s_haveCenter)  { c->state = MAPS_NOFIX; storageSet("maps.state", "no fix"); lcdRun(composite, c); return; }

    /* view centre: GPS when following, else free-panned */
    if (s_follow) { s_viewLat = s_lat; s_viewLon = s_lon; s_haveView = true; }
    else if (dx || dy) {
        double gx, gy;
        lonlatToGlobalPx(s_viewLon, s_viewLat, zoom, &gx, &gy);
        gx -= dx; gy -= dy;     /* drag the map under the finger */
        globalPxToLonLat(gx, gy, zoom, &s_viewLon, &s_viewLat);
    }
    if (!s_haveView) { s_viewLat = s_lat; s_viewLon = s_lon; s_haveView = true; }

    double vgx, vgy;
    lonlatToGlobalPx(s_viewLon, s_viewLat, zoom, &vgx, &vgy);
    long tlx = (long)llround(vgx) - s_W / 2;
    long tly = (long)llround(vgy) - s_H / 2;
    c->tlx = tlx; c->tly = tly; c->state = MAPS_OK;

    /* marker = GPS pixel relative to top-left (may fall off-screen when panned) */
    double ggx, ggy;
    lonlatToGlobalPx(s_lon, s_lat, zoom, &ggx, &ggy);
    c->markerX = (long)llround(ggx) - tlx;
    c->markerY = (long)llround(ggy) - tly;

    int n   = 1 << zoom;
    int tx0 = (int)floor(tlx / (double)TILE), tx1 = (int)floor((tlx + s_W - 1) / (double)TILE);
    int ty0 = (int)floor(tly / (double)TILE), ty1 = (int)floor((tly + s_H - 1) / (double)TILE);
    int present = 0;
    for (int tx = tx0; tx <= tx1; tx++)
        for (int ty = ty0; ty <= ty1; ty++) {
            if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;
            if (ensureCell(dir.c_str(), zoom, tx, ty)) present++;
        }

    storageSet("maps.state", present ? "ready" : "no tiles");
    lcdRun(composite, c);
}

static void wakeWorker(void) {
    s_dirty = true;
    if (s_worker) xTaskNotifyGive(s_worker);
}

static void onChange(const char* /*k*/, const char* /*v*/) { wakeWorker(); }

static void mapsWorker(void*) {
    info("task up");
    itsClientInit(2);
    /* Only the fix position re-centres the map. Subscribing to the whole "gps"
     * scope pulled in the ~1 Hz telemetry churn (sats/snr/dop/utc/fix_age) too,
     * which overflowed this worker's inbox while it was busy reading SD tiles —
     * a "notify drop" warn storm. lat/lon are all rebuild() actually reads. */
    storageSubscribeChanges("gps.lat", onChange);  /* centre */
    storageSubscribeChanges("gps.lon", onChange);
    storageSubscribeChanges("s.maps",  onChange);  /* zoom / tiledir */
    for (;;) {
        if (s_dirty) { s_dirty = false; rebuild(); }
        itsPoll(portMAX_DELAY);
    }
}

/* ─────────────── input (lcd task) ─────────────── */

/* Drag to pan (touch or trackball-press-drag). Accumulate the indev movement
 * vector; the worker applies it and re-locks follow off. */
static void mapPressCb(lv_event_t* e) {
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    portENTER_CRITICAL(&s_ctrlMux);
    s_panDx += v.x; s_panDy += v.y;
    portEXIT_CRITICAL(&s_ctrlMux);
    wakeWorker();
}

static void mapCenterCb(lv_event_t* /*e*/) {
    portENTER_CRITICAL(&s_ctrlMux);
    s_recenter = true;
    portEXIT_CRITICAL(&s_ctrlMux);
    wakeWorker();
}

/* Pinch-to-zoom (lcd task, via the lcd gesture callback). Two fingers: live-
 * scale the canvas for feedback; on release step s.maps.zoom by the nearest
 * power-of-two of the pinch ratio. The worker then refetches at the new integer
 * zoom; overzoom fallback keeps the view non-grey across the change. */
static bool   s_pinchActive   = false;
static double s_pinchInitDist = 1;
static double s_pinchRatio    = 1;
static int    s_pinchZoom     = 15;

static void mapsGesture(const lcd_touch_pt_t* pts, int count) {
    if (count >= 2) {
        double dx = pts[0].x - pts[1].x, dy = pts[0].y - pts[1].y;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist < 1) dist = 1;
        if (!s_pinchActive) {
            s_pinchActive   = true;
            s_pinchInitDist = dist;
            s_pinchZoom     = storageGetInt("s.maps.zoom", 15);
            if (s_canvas) {
                lv_obj_set_style_transform_pivot_x(s_canvas, (pts[0].x + pts[1].x) / 2, 0);
                lv_obj_set_style_transform_pivot_y(s_canvas, (pts[0].y + pts[1].y) / 2, 0);
            }
        }
        s_pinchRatio = dist / s_pinchInitDist;
        if (s_canvas) {
            int sc = (int)(256.0 * s_pinchRatio);    /* LVGL scale: 256 = 1.0x */
            if (sc < 64) sc = 64; else if (sc > 1024) sc = 1024;
            lv_obj_set_style_transform_scale(s_canvas, sc, 0);
        }
    } else if (s_pinchActive) {
        s_pinchActive = false;
        if (s_canvas) lv_obj_set_style_transform_scale(s_canvas, 256, 0);   /* back to 1.0x */
        int steps = (int)lround(log2(s_pinchRatio));
        s_pinchRatio = 1;
        if (steps != 0) {
            int nz = s_pinchZoom + steps;
            if (nz < ZOOM_MIN) nz = ZOOM_MIN; else if (nz > ZOOM_MAX) nz = ZOOM_MAX;
            if (nz != s_pinchZoom) storageSet("s.maps.zoom", nz);   /* -> worker rebuild */
        }
    }
}

/* ─────────────── launcher program (lcd task) ─────────────── */

static void mapsApp(void* arg) {
    lv_obj_t* layer = (lv_obj_t*)arg;
    storageSet("tdeck.multi_touch", 1);   /* we want pinch gestures while open */
    if (s_canvas) {                 /* re-open: layer was kept, just refresh */
        s_open = true; wakeWorker();
        return;
    }

    int W = lv_obj_get_content_width(layer);
    int H = lv_obj_get_content_height(layer);
    if (W <= 0) W = 320;
    if (H <= 0) H = 240;

    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)W, LV_COLOR_FORMAT_RGB565);
    s_canvasBuf = (uint16_t*)heap_caps_malloc((size_t)stride * H, MALLOC_CAP_SPIRAM);
    if (!s_canvasBuf) { err("canvas alloc failed (%dx%d)", W, H); return; }
    s_W = W; s_H = H; s_stridePx = (int)(stride / 2);

    s_canvas = lv_canvas_create(layer);
    lv_canvas_set_buffer(s_canvas, s_canvasBuf, W, H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x808080), LV_OPA_COVER);
    lv_obj_center(s_canvas);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);    /* so it gets press/drag */
    lv_obj_add_event_cb(s_canvas, mapPressCb, LV_EVENT_PRESSING, nullptr);

    s_label = lv_label_create(layer);
    lv_label_set_text(s_label, "Waiting for GPS...");
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);   /* multi-line status reads centred */
    lv_obj_center(s_label);

    /* "centre me" button (re-lock to GPS) */
    lv_obj_t* btn = lv_button_create(layer);
    lv_obj_set_size(btn, 26, 26);   /* ~60% of the old 44px */
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_add_event_cb(btn, mapCenterCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_GPS);
    lv_obj_center(bl);

    lcdTouchAddGestureHandler(mapsGesture);   /* pinch-to-zoom */

    s_open = true; wakeWorker();
}

/* ─────────────── CLI + settings ─────────────── */

static void cliMaps(const char* args) {
    if (args && strcmp(args, "help") == 0) { cliPrintf("%-*s map status; center to recentre on GPS\n", CLI_HELP_COL, "maps [center]"); return; }
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s map status\n",        CLI_HELP_COL, "maps");
        cliPrintf("%-*s recentre on GPS\n",   CLI_HELP_COL, "maps center");
        return;
    }
    if (args && strcmp(args, "center") == 0) { mapCenterCb(nullptr); cliPrintf("recentred\n"); return; }

    cliPrintf("state:   %s\n", storageGetStr("maps.state", "-").c_str());
    cliPrintf("sd:      %s\n", sdAvailable() ? "mounted" : "none");
    cliPrintf("zoom:    %d\n", storageGetInt("s.maps.zoom", 15));
    cliPrintf("follow:  %s\n", s_follow ? "yes" : "no (panned)");
    cliPrintf("tiledir: %s\n", storageGetStr("s.maps.tiledir", "/sdcard/maps").c_str());
    if (s_haveCenter) cliPrintf("gps:     %.6f, %.6f\n", s_lat, s_lon);
    else              cliPrintf("gps:     (no fix yet)\n");
    if (s_haveView)   cliPrintf("view:    %.6f, %.6f\n", s_viewLat, s_viewLon);
}

static void mapsSettingsPane(void* arg) {
    lv_obj_t* p = (lv_obj_t*)arg;
    lcdSettingSection(p, "Map");
    lcdSettingSlider (p, "Zoom",     "s.maps.zoom", ZOOM_MIN, ZOOM_MAX);
    lcdSettingValue  (p, "Status",   "maps.state");
    lcdSettingText   (p, "Tile dir", "s.maps.tiledir");
}

/* ─────────────── init ─────────────── */

void mapsInit(void) {
    if (storageGetInt("s.maps.version", 0) < MAPS_VERSION) {
        storageDefault("s.maps.zoom", 15);
        storageDefault("s.maps.tiledir", "/sdcard/maps");
        storageSet("s.maps.version", MAPS_VERSION);
    }
    s_mux = xSemaphoreCreateMutex();
    cliRegisterCmd("maps", cliMaps);
    s_worker = spawnTask(mapsWorker, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);

    /* Self-register the launcher program + Settings pane. Both push into
     * registries the lcd task reads lazily (lcdRegisterSettings is documented
     * safe from any init task; the launcher entry is read when the grid is
     * built, on the lcd task after lcdInit), so registering here — from
     * spangapInitStraddles(), before lcdInit — is fine. Folding this into
     * mapsInit() is what lets the consumer's main.cpp reference maps nowhere. */
    lcdRegister("Maps", "maps", mapsApp);
    lcdRegisterSettings("Maps", "Maps", mapsSettingsPane);
}

#else  /* !CONFIG_SPANGAP_LCD — no display, no map */

void mapsInit(void) {}

#endif
