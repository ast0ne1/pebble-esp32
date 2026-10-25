/* PC LVGL simulator — same UI as firmware.
 *   (default)     screenshots → shots/
 *   --serve       stdin commands / stdout frames (for server.py)
 *
 * Adapted from curisama/The-Badge sim (MIT). */
#include "app.h"
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#define PEBBLE_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define PEBBLE_MKDIR(p) mkdir((p), 0755)
#endif

#define W 466
#define H 466

static uint16_t s_fb[W * H];

extern uint32_t g_sim_us;
static uint32_t tick_cb(void) { return g_sim_us / 1000; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; y++)
        for (int x = area->x1; x <= area->x2; x++)
            s_fb[y * W + x] = *src++;
    lv_display_flush_ready(disp);
}

static int32_t s_touch_x, s_touch_y;
static bool    s_touch_down;
static bool    s_pending_press;

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    bool pressed = s_touch_down || s_pending_press;
    if (s_pending_press && !s_touch_down) s_pending_press = false;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void advance(uint32_t us)
{
    const uint32_t BASE = 5000;
    for (uint32_t done = 0; done < us; ) {
        uint32_t chunk = BASE;
        if (done + chunk > us) chunk = us - done;
        g_sim_us += chunk;
        done += chunk;
        lv_timer_handler();
    }
}

/* ── minimal uncompressed PNG (The-Badge approach) ──────────── */

static uint32_t s_crc_tab[256];
static int s_crc_ready;

static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n)
{
    if (!s_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            s_crc_tab[i] = v;
        }
        s_crc_ready = 1;
    }
    for (size_t i = 0; i < n; i++) c = s_crc_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t buf[4];
    put_be32(buf, (uint32_t)n);
    fwrite(buf, 1, 4, f);
    fwrite(tag, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc_upd(0xFFFFFFFFu, (const uint8_t *)tag, 4);
    if (n) crc = crc_upd(crc, data, n);
    put_be32(buf, crc ^ 0xFFFFFFFFu);
    fwrite(buf, 1, 4, f);
}

static void write_png(const char *name, int mask_corners)
{
    char png[256];
    PEBBLE_MKDIR("shots");
    snprintf(png, sizeof(png), "shots/%s.png", name);

    const size_t stride = 1 + (size_t)W * 3;
    const size_t raw_n = stride * (size_t)H;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) return;

    const int cx = W / 2, cy = H / 2, r2 = (W / 2) * (W / 2);
    for (int y = 0; y < H; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        *row++ = 0;
        for (int x = 0; x < W; x++) {
            uint16_t c = s_fb[y * W + x];
            uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
            int dx = x - cx, dy = y - cy;
            if (mask_corners && dx * dx + dy * dy > r2) r = g = b = 40;
            *row++ = r; *row++ = g; *row++ = b;
        }
    }

    const size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = (uint8_t *)malloc(2 + blocks * 5 + raw_n + 4);
    if (!z) { free(raw); return; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = (raw_n - off < 65535) ? raw_n - off : 65535;
        z[zi++] = (off + n >= raw_n) ? 1 : 0;
        z[zi++] = (uint8_t)(n & 0xFF);
        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);
        z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
    }
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_n; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    put_be32(z + zi, (s2 << 16) | s1);
    zi += 4;

    FILE *f = fopen(png, "wb");
    if (!f) { free(raw); free(z); return; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)W);
    put_be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(f, "IDAT", z, zi);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    fprintf(stderr, "shot: %s\n", png);
}

/* Commands:
 *   T x y 0|1   touch
 *   H           home (PWR short)
 *   Z           soft sleep / screen off (PWR hold)
 *   W           unused (BOOT is PTT via S)
 *   S 0|1       PTT (Talk / BOOT hold) — 1 down, 0 up
 *   P ms        advance time
 *   R           RGB565 frame
 *   Q           quit */
static void serve_loop(void)
{
    char line[128];
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    while (fgets(line, sizeof(line), stdin)) {
        switch (line[0]) {
        case 'T': {
            int x, y, st;
            if (sscanf(line + 1, "%d %d %d", &x, &y, &st) == 3) {
                s_touch_x = x;
                s_touch_y = y;
                if (st) {
                    s_touch_down = true;
                    s_pending_press = true;
                } else {
                    s_touch_down = false;
                }
            }
            break;
        }
        case 'H':
            shell_note_activity();
            shell_show_home();
            break;
        case 'Z':
            shell_ptt_set(false);
            shell_screen_off();
            break;
        case 'W':
            /* Legacy: no longer opens splash — BOOT is PTT via S. */
            break;
        case 'S':
            shell_ptt_set(atoi(line + 1) != 0);
            break;
        case 'P': {
            int ms = atoi(line + 1);
            if (ms > 0 && ms < 5000) advance((uint32_t)ms * 1000);
            break;
        }
        case 'R': {
            if (!port_display_is_on()) memset(s_fb, 0, sizeof(s_fb));
            printf("FRAME %d\n", W * H * 2);
            fwrite(s_fb, 1, sizeof(s_fb), stdout);
            fflush(stdout);
            break;
        }
        case 'Q':
            return;
        default:
            break;
        }
    }
}

int main(int argc, char **argv)
{
    bool serve = (argc > 1 && strcmp(argv[1], "--serve") == 0);

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(W, H);
#define BAND_H 24
    static uint8_t draw_buf[W * H * 2];
    lv_display_set_buffers(disp, draw_buf, NULL, W * BAND_H * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);
    lv_timer_set_period(lv_indev_get_read_timer(indev), 12);

    /* Real boot path — splash, then shell. Serve mode advances via P ticks
     * from server.py, so the 2.8s splash plays once clients are connected. */
    splash_show();
    if (serve) {
        shell_set_timeout(0);
        serve_loop();
        return 0;
    }

    advance(80000); /* let LVGL paint splash before its timer fires */
    write_png("00_splash", 1);
    advance(3200000); /* past splash timer into shell */
    shell_set_timeout(0);
    advance(50000);
    write_png("01_home", 1);
    shell_show_settings();
    advance(80000);
    write_png("02_settings", 1);
    settings_open_panel("pairing");
    advance(80000);
    write_png("03_pairing", 1);
    settings_close_panel();
    shell_show_home();
    fprintf(stderr, "done - shots in sim/shots/\n");
    return 0;
}
