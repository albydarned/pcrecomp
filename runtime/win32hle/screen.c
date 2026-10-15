/*
 * screen.c - the display and input for a DirectDraw game on win32hle: an SDL2
 * window showing the primary surface (ddraw.c), scaled to the window with its
 * aspect kept, and SDL's mouse and keyboard turned into the window messages
 * a game's windows get (user32.c's hle_input_*).
 *
 * It runs as user32's pump hook, on the thread that runs the game (SDL wants
 * its video calls on one thread, and the game's thread is the one that pumps
 * messages). A frame is shown when the primary has changed, at most 60 times
 * a second: from the pump, and as the game draws into the primary (ddraw.c's
 * hle_dd_primary_hook), since on Windows the primary is the screen. A game
 * that draws without pumping (a loading screen, a menu's transition) is seen
 * as it draws, as on Windows.
 *
 * Headless (hle_screen_open(..., headless)), there is no window: the pump
 * only paces, and input comes from a host's script.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "win32hle.h"

#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u
#define WM_CLOSE       0x0010u

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_tex_w, g_tex_h, g_tex_fmt;
static unsigned g_shown_seq = ~0u;
static uint32_t g_last_present;
static uint32_t g_last_pump;                             /* when the game last pumped: in its loop, or not */
static unsigned g_stat_pump, g_stat_primary, g_stat_heard;
static double g_stat_ms, g_stat_compose, g_stat_upload;   /* ms: the present whole, its compose, its upload */
static int g_stat_box[2];                                /* the last upload's region, 1x pixels */   /* HLE_SCREEN_STATS: presents from the pump, from the primary hook, primary changes heard */
static int g_headless, g_fullscreen;
static SDL_Rect g_dst;                                   /* where the picture is in the window */
static pthread_t g_video_thread;                         /* SDL's video calls stay on the thread that opened it */
static SDL_Texture *g_pre, *g_small;                     /* sharp's integer prescale; the bars' blur */
static int g_pre_w, g_pre_h;

/* How the picture is scaled, as the Windows presenter offers (F12 cycles):
 * sharp-bilinear (an integer prescale, nearest, then the rest bilinear),
 * smooth (bilinear), crt (sharp with scanlines), nearest, integer (whole
 * multiples only). The bars beside a letterboxed picture are a dark blur of
 * it, or black. */
static const char *const k_modes[] = { "sharp", "smooth", "crt", "nearest", "integer" };
static int g_mode, g_bars = 1;
void (*hle_screen_settings_hook)(const char *scale, int bars, int fullscreen);   /* to remember them */
int hle_screen_scale(const char *name) {
    for (int i = 0; i < 5; i++) if (name && !strcmp(name, k_modes[i])) return g_mode = i, 1;
    return 0;
}
void hle_screen_bars(int blur) { g_bars = blur != 0; }
static void settings_changed(void) {
    g_shown_seq = ~0u;
    if (hle_screen_settings_hook) hle_screen_settings_hook(k_modes[g_mode], g_bars, g_fullscreen);
}
static uint32_t *g_conv;                                 /* 8-bit frames converted */
static size_t g_conv_n;
void (*hle_screen_frame_hook)(const uint8_t *px, int w, int h, int pitch, int bpp);   /* a recorder, a checksum */
/* A host's own picture at twice the size (an HD layer): fills out with 2w x
 * 2h XRGB from the 16-bit frame and returns 1, or returns 0 for the 1x. */
int (*hle_screen_compose_hook)(const uint8_t *px16, int pitch, int w, int h, uint32_t *out);
static uint32_t *g_hd;
static size_t g_hd_n;
/* A host's zoom: the frame's rect src (game pixels) shown scaled into its rect
 * dst, over the rest of the picture, and the mouse in dst mapped back into
 * src. Rects are x, y, w, h; returns 0 when nothing is zoomed. The wheel goes
 * to the host with the frame point under the cursor (before that mapping). */
int (*hle_screen_view_hook)(int src[4], int dst[4]);
void (*hle_screen_wheel_hook)(float dy, int fx, int fy);
void (*hle_screen_motion_hook)(int fx, int fy);
int (*hle_screen_crop_hook)(const uint8_t *px16, int pitch, int w, int h, const int src[4], uint32_t *out);
int hle_screen_dirty[4];
int hle_screen_crop_wanted;                              /* this picture will show the crop (set before the compose) */
static uint32_t *g_crop;                                 /* the crop at 4x, and its texture */
static size_t g_crop_n;
static SDL_Texture *g_ctex;
static int g_ctex_w, g_ctex_h;

static int vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}
/* the PC/AT set-1 scan code, which lParam carries */
static uint32_t scan_of(SDL_Scancode s) {
    static const uint8_t t[SDL_NUM_SCANCODES] = {
        [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 2, [SDL_SCANCODE_2] = 3, [SDL_SCANCODE_3] = 4, [SDL_SCANCODE_4] = 5,
        [SDL_SCANCODE_5] = 6, [SDL_SCANCODE_6] = 7, [SDL_SCANCODE_7] = 8, [SDL_SCANCODE_8] = 9, [SDL_SCANCODE_9] = 10,
        [SDL_SCANCODE_0] = 11, [SDL_SCANCODE_MINUS] = 12, [SDL_SCANCODE_EQUALS] = 13, [SDL_SCANCODE_BACKSPACE] = 14,
        [SDL_SCANCODE_TAB] = 15, [SDL_SCANCODE_Q] = 16, [SDL_SCANCODE_W] = 17, [SDL_SCANCODE_E] = 18, [SDL_SCANCODE_R] = 19,
        [SDL_SCANCODE_T] = 20, [SDL_SCANCODE_Y] = 21, [SDL_SCANCODE_U] = 22, [SDL_SCANCODE_I] = 23, [SDL_SCANCODE_O] = 24,
        [SDL_SCANCODE_P] = 25, [SDL_SCANCODE_LEFTBRACKET] = 26, [SDL_SCANCODE_RIGHTBRACKET] = 27, [SDL_SCANCODE_RETURN] = 28,
        [SDL_SCANCODE_LCTRL] = 29, [SDL_SCANCODE_A] = 30, [SDL_SCANCODE_S] = 31, [SDL_SCANCODE_D] = 32, [SDL_SCANCODE_F] = 33,
        [SDL_SCANCODE_G] = 34, [SDL_SCANCODE_H] = 35, [SDL_SCANCODE_J] = 36, [SDL_SCANCODE_K] = 37, [SDL_SCANCODE_L] = 38,
        [SDL_SCANCODE_SEMICOLON] = 39, [SDL_SCANCODE_APOSTROPHE] = 40, [SDL_SCANCODE_GRAVE] = 41, [SDL_SCANCODE_LSHIFT] = 42,
        [SDL_SCANCODE_BACKSLASH] = 43, [SDL_SCANCODE_Z] = 44, [SDL_SCANCODE_X] = 45, [SDL_SCANCODE_C] = 46, [SDL_SCANCODE_V] = 47,
        [SDL_SCANCODE_B] = 48, [SDL_SCANCODE_N] = 49, [SDL_SCANCODE_M] = 50, [SDL_SCANCODE_COMMA] = 51, [SDL_SCANCODE_PERIOD] = 52,
        [SDL_SCANCODE_SLASH] = 53, [SDL_SCANCODE_RSHIFT] = 54, [SDL_SCANCODE_LALT] = 56, [SDL_SCANCODE_SPACE] = 57,
        [SDL_SCANCODE_CAPSLOCK] = 58, [SDL_SCANCODE_F1] = 59, [SDL_SCANCODE_F2] = 60, [SDL_SCANCODE_F3] = 61, [SDL_SCANCODE_F4] = 62,
        [SDL_SCANCODE_F5] = 63, [SDL_SCANCODE_F6] = 64, [SDL_SCANCODE_F7] = 65, [SDL_SCANCODE_F8] = 66, [SDL_SCANCODE_F9] = 67,
        [SDL_SCANCODE_F10] = 68, [SDL_SCANCODE_F11] = 87, [SDL_SCANCODE_F12] = 88, [SDL_SCANCODE_HOME] = 71, [SDL_SCANCODE_UP] = 72,
        [SDL_SCANCODE_PAGEUP] = 73, [SDL_SCANCODE_LEFT] = 75, [SDL_SCANCODE_RIGHT] = 77, [SDL_SCANCODE_END] = 79,
        [SDL_SCANCODE_DOWN] = 80, [SDL_SCANCODE_PAGEDOWN] = 81, [SDL_SCANCODE_INSERT] = 82, [SDL_SCANCODE_DELETE] = 83 };
    return (unsigned)s < SDL_NUM_SCANCODES ? t[s] : 0;
}

/* window points -> game pixels, clamped to the picture; view: through the
 * host's zoom (a point in its dst lands in its src) */
static void to_frame(int x, int y, int *gx, int *gy, int view) {
    int w, h, bpp, pw, ph, ww, wh;
    hle_dd_mode(&w, &h, &bpp);
    if (g_ren && g_win) {                                /* HiDPI: the renderer counts pixels, events points */
        SDL_GetRendererOutputSize(g_ren, &pw, &ph);
        SDL_GetWindowSize(g_win, &ww, &wh);
        if (ww > 0 && wh > 0) x = x * pw / ww, y = y * ph / wh;
    }
    if (g_dst.w <= 0 || g_dst.h <= 0) { *gx = x, *gy = y; return; }
    double fx = (double)(x - g_dst.x) * w / g_dst.w, fy = (double)(y - g_dst.y) * h / g_dst.h;
    int src[4], dst[4];
    if (view && hle_screen_view_hook && hle_screen_view_hook(src, dst) && dst[2] > 0 && dst[3] > 0
        && fx >= dst[0] && fx < dst[0] + dst[2] && fy >= dst[1] && fy < dst[1] + dst[3]) {
        fx = src[0] + (fx - dst[0]) * src[2] / dst[2];
        fy = src[1] + (fy - dst[1]) * src[3] / dst[3];
    }
    *gx = (int)fx, *gy = (int)fy;
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= w) *gx = w - 1;
    if (*gy >= h) *gy = h - 1;
}
static void to_game(int x, int y, int *gx, int *gy) { to_frame(x, y, gx, gy, 1); }

static void present(void) {
    const uint8_t *px;
    const uint32_t *pal;
    int w, h, pitch, bpp;
    unsigned seq;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq)) return;
    uint32_t now = hle_ticks_ms();
    if (seq == g_shown_seq && now - g_last_present < 250) return;
    if (now - g_last_present < 15) return;
    g_shown_seq = seq, g_last_present = now;
    {                                                    /* HLE_SCREEN_STATS=1: every 2 s, who presented how often, and what a present took */
        static int on = -1;
        static uint32_t since;
        static unsigned n;
        if (on < 0) on = getenv("HLE_SCREEN_STATS") != NULL, since = now;
        if (on) {
            if (now - since >= 2000) {
                fprintf(stderr, "[screen] %.1f presents/s (%u pumps, %u presents as the game drew into the primary, %u changes heard); "
                        "%.1f ms each: compose %.1f, upload %.1f (last %dx%d), the rest %.1f\n",
                        n * 1000.0 / (now - since), g_stat_pump, g_stat_primary, g_stat_heard, n ? g_stat_ms / n : 0.0,
                        n ? g_stat_compose / n : 0.0, n ? g_stat_upload / n : 0.0, g_stat_box[0], g_stat_box[1],
                        n ? (g_stat_ms - g_stat_compose - g_stat_upload) / n : 0.0);
                since = now, n = 0, g_stat_ms = g_stat_compose = g_stat_upload = 0, g_stat_pump = g_stat_primary = g_stat_heard = 0;
            }
            n++;
        }
    }
    if (hle_screen_frame_hook) hle_screen_frame_hook(px, w, h, pitch, bpp);
    if (!g_ren) return;
    int hd = 0;
    {                                                    /* will the crop be drawn? the compose may skip work under it */
        int s4[4], d4[4];
        hle_screen_crop_wanted = bpp == 16 && hle_screen_crop_hook && hle_screen_view_hook && hle_screen_view_hook(s4, d4)
                                 && s4[2] > 0 && s4[2] * 2 <= d4[2];
    }
    if (bpp == 16 && hle_screen_compose_hook) {
        if (g_hd_n < (size_t)w * h * 4) { free(g_hd); g_hd_n = (size_t)w * h * 4; g_hd = (uint32_t *)malloc(g_hd_n * 4); }
        hle_screen_dirty[2] = -1;                        /* a hook that does not say: all of it */
        uint32_t t0 = hle_ticks_ms();
        hd = g_hd && hle_screen_compose_hook(px, pitch, w, h, g_hd);
        g_stat_compose += (double)(hle_ticks_ms() - t0);
    }
    int fmt = bpp == 16 && !hd ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888;
    int tw = hd ? 2 * w : w, th = hd ? 2 * h : h;
    int fresh = 0;
    if (!g_tex || g_tex_w != tw || g_tex_h != th || g_tex_fmt != fmt) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, (Uint32)fmt, SDL_TEXTUREACCESS_STREAMING, tw, th);
        g_tex_w = tw, g_tex_h = th, g_tex_fmt = fmt;
        fresh = 1;
    }
    uint32_t t_up = hle_ticks_ms();
    g_stat_box[0] = g_stat_box[1] = 0;
    if (hd) {
        /* The texture holds the last 2x picture whole, and the hook's
         * buffer persists: only what the compose changed goes up (a
         * cursor's worth, not the 2560x1440 picture's 59 MB). */
        int dx = hle_screen_dirty[0], dy = hle_screen_dirty[1], dw = hle_screen_dirty[2], dh = hle_screen_dirty[3];
        if (fresh || dw < 0 || dh < 0) {
            SDL_UpdateTexture(g_tex, NULL, g_hd, tw * 4);
            g_stat_box[0] = w, g_stat_box[1] = h;
        } else if (dw > 0 && dh > 0) {
            if (dx < 0) dw += dx, dx = 0;
            if (dy < 0) dh += dy, dy = 0;
            if (dx + dw > w) dw = w - dx;
            if (dy + dh > h) dh = h - dy;
            if (dw > 0 && dh > 0) {
                SDL_Rect r = { 2 * dx, 2 * dy, 2 * dw, 2 * dh };
                SDL_UpdateTexture(g_tex, &r, g_hd + (size_t)r.y * tw + r.x, tw * 4);
                g_stat_box[0] = dw, g_stat_box[1] = dh;
            }
        }
    } else if (bpp == 8) {
        if (g_conv_n < (size_t)w * h) { free(g_conv); g_conv_n = (size_t)w * h; g_conv = (uint32_t *)malloc(g_conv_n * 4); }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t e = pal ? pal[px[(size_t)y * pitch + x]] : 0;   /* PALETTEENTRY r, g, b */
                g_conv[(size_t)y * w + x] = 0xFF000000u | (e & 0xFF) << 16 | (e & 0xFF00) | (e >> 16 & 0xFF);
            }
        SDL_UpdateTexture(g_tex, NULL, g_conv, w * 4);
    } else {
        SDL_UpdateTexture(g_tex, NULL, px, pitch);
    }
    g_stat_upload += (double)(hle_ticks_ms() - t_up);
    int ww, wh;
    SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    if ((long long)ww * h > (long long)wh * w) g_dst.h = wh, g_dst.w = (int)((long long)wh * w / h);
    else g_dst.w = ww, g_dst.h = (int)((long long)ww * h / w);
    if (g_mode == 4) {                                   /* integer: whole multiples of the game's size */
        int k = ww / w < wh / h ? ww / w : wh / h;
        if (k < 1) k = 1;
        g_dst.w = w * k, g_dst.h = h * k;
    }
    g_dst.x = (ww - g_dst.w) / 2, g_dst.y = (wh - g_dst.h) / 2;
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    if (g_bars && (g_dst.w < ww || g_dst.h < wh)) {     /* the bars: the picture, tiny then stretched, dark */
        if (!g_small) g_small = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, 24, 15);
        if (g_small) {
            SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeLinear);
            SDL_SetRenderTarget(g_ren, g_small);
            SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
            SDL_SetRenderTarget(g_ren, NULL);
            SDL_SetTextureScaleMode(g_small, SDL_ScaleModeLinear);
            SDL_SetTextureColorMod(g_small, 90, 90, 90);
            SDL_RenderCopy(g_ren, g_small, NULL, NULL);
        }
    }
    int sharp = g_mode == 0 || g_mode == 2;
    int k = g_dst.w / tw < g_dst.h / th ? g_dst.w / tw : g_dst.h / th;
    if (sharp && k >= 1 && (tw * k != g_dst.w || th * k != g_dst.h)) {
        if (!g_pre || g_pre_w != tw * k || g_pre_h != th * k) {
            if (g_pre) SDL_DestroyTexture(g_pre);
            g_pre = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, tw * k, th * k);
            g_pre_w = tw * k, g_pre_h = th * k;
        }
    }
    SDL_Texture *shown = g_tex;                          /* the picture's texture, frame-sized times sw x sh */
    int sw = tw, sh = th;
    if (sharp && k >= 1 && g_pre && (tw * k != g_dst.w || th * k != g_dst.h)) {
        SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeNearest);
        SDL_SetRenderTarget(g_ren, g_pre);
        SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
        SDL_SetRenderTarget(g_ren, NULL);
        SDL_SetTextureScaleMode(g_pre, SDL_ScaleModeLinear);
        SDL_RenderCopy(g_ren, g_pre, NULL, &g_dst);
        shown = g_pre, sw = g_pre_w, sh = g_pre_h;
    } else {
        SDL_SetTextureScaleMode(g_tex, g_mode == 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        SDL_RenderCopy(g_ren, g_tex, NULL, &g_dst);
    }
    int vsrc[4], vdst[4];
    if (hle_screen_view_hook && hle_screen_view_hook(vsrc, vdst)) {   /* the host's zoom, over its own rect */
        SDL_Rect from = { vsrc[0] * sw / w, vsrc[1] * sh / h, vsrc[2] * sw / w, vsrc[3] * sh / h };
        SDL_Rect to = { g_dst.x + (int)((long long)vdst[0] * g_dst.w / w), g_dst.y + (int)((long long)vdst[1] * g_dst.h / h), 0, 0 };
        to.w = g_dst.x + (int)((long long)(vdst[0] + vdst[2]) * g_dst.w / w) - to.x;
        to.h = g_dst.y + (int)((long long)(vdst[1] + vdst[3]) * g_dst.h / h) - to.y;
        /* magnified 2x or more with HD on: the crop composed at 4x (a 4x
         * pack's own pixels), the cost of the 2x picture's quarter */
        int cropped = 0;
        if (hd && bpp == 16 && hle_screen_crop_hook && vsrc[2] * 2 <= vdst[2] && vsrc[2] > 0 && vsrc[3] > 0) {
            size_t need = (size_t)vsrc[2] * 4 * vsrc[3] * 4;
            if (g_crop_n < need) { free(g_crop); g_crop = (uint32_t *)malloc(need * 4); g_crop_n = g_crop ? need : 0; }
            if (g_crop && hle_screen_crop_hook(px, pitch, w, h, vsrc, g_crop)) {
                int cw = vsrc[2] * 4, chh = vsrc[3] * 4;
                if (!g_ctex || g_ctex_w != cw || g_ctex_h != chh) {
                    if (g_ctex) SDL_DestroyTexture(g_ctex);
                    g_ctex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, cw, chh);
                    g_ctex_w = cw, g_ctex_h = chh;
                }
                if (g_ctex) {
                    SDL_UpdateTexture(g_ctex, NULL, g_crop, cw * 4);
                    SDL_SetTextureScaleMode(g_ctex, g_mode == 3 || g_mode == 4 ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
                    SDL_RenderCopy(g_ren, g_ctex, NULL, &to);
                    cropped = 1;
                }
            }
        }
        if (!cropped) {
            if (g_mode != 3 && g_mode != 4) SDL_SetTextureScaleMode(shown, SDL_ScaleModeLinear);
            SDL_RenderCopy(g_ren, shown, &from, &to);
        }
    }
    if (g_mode == 2 && g_dst.h >= 2 * h) {                /* crt: the lower third of every game row darker */
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 96);
        for (int y = 0; y < h; y++) {
            int y0 = g_dst.y + (int)((long long)(3 * y + 2) * g_dst.h / (3 * h)), y1 = g_dst.y + (int)((long long)(y + 1) * g_dst.h / h);
            SDL_Rect r = { g_dst.x, y0, g_dst.w, y1 > y0 ? y1 - y0 : 1 };
            SDL_RenderFillRect(g_ren, &r);
        }
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
    }
    static const char *shots = (const char *)-1;         /* HLE_SCREEN_SHOT=DIR: every 100th picture shown, as the window has it */
    static unsigned shot_n;
    if (shots == (const char *)-1) shots = getenv("HLE_SCREEN_SHOT");
    if (shots && shot_n++ % 100 == 0) {
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ww, wh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (sf && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s/screen_%05u.bmp", shots, shot_n / 100);
            SDL_SaveBMP(sf, path);
        }
        if (sf) SDL_FreeSurface(sf);
    }
    SDL_RenderPresent(g_ren);
    g_stat_ms += (double)(hle_ticks_ms() - now);
}

/* Who shows the cursor. A DirectDraw game draws its own into the picture, so
 * by default the window has none. A game whose menus are Win32 dialogs (Red
 * Alert 2) uses the Windows cursor there: its host's hook says when, and the
 * window shows the cursor the game set, built from its resources. */
int (*hle_screen_cursor_hook)(void);
int (*hle_screen_key_hook)(int sdl_scancode, int down);

/* The window's title, for a host that shows what is playing (a mod). */
void hle_screen_title(const char *title) { if (g_win) SDL_SetWindowTitle(g_win, title); }

static SDL_Cursor *sdl_cursor_from(const hle_cursor_image_t *img) {
    /* Windows' AND/XOR masks to SDL's mask/data: AND 0 is opaque (XOR 0 black,
     * 1 white); AND 1 is transparent (XOR 1 inverts). */
    uint8_t data[128], mask[128];
    for (int i = 0; i < 128; i++) {
        mask[i] = (uint8_t)~img->and_mask[i];
        data[i] = (uint8_t)((~img->xor_mask[i] & mask[i]) | (img->xor_mask[i] & img->and_mask[i]));
    }
    return SDL_CreateCursor(data, mask, 32, 32, img->hot_x, img->hot_y);
}

static void update_cursor(void) {
    static int was_shown = 0;
    static uint32_t was_handle;
    static SDL_Cursor *made;
    int show = hle_screen_cursor_hook && hle_screen_cursor_hook();
    uint32_t handle = hle_current_cursor();
    if (show == was_shown && (!show || handle == was_handle)) return;
    if (show && handle != was_handle) {
        const hle_cursor_image_t *img = hle_cursor_image(handle);
        SDL_Cursor *c = img ? sdl_cursor_from(img) : NULL;
        SDL_SetCursor(c ? c : SDL_GetDefaultCursor());
        if (made) SDL_FreeCursor(made);
        made = c;
        was_handle = handle;
    }
    if (show != was_shown) SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
    fprintf(stderr, "[screen] cursor: %s\n", !show ? "drawn by the game" : made ? "the game's, from its resources" : "the system arrow");
    was_shown = show;
}

/* The window's close button (or Cmd-Q): WM_CLOSE to the game, which may save
 * its settings and quit on its own, and after a grace period the process
 * leaves anyway, as the Windows presenter does: a game that ignores WM_CLOSE,
 * or takes long over its fade-out, is not what a player closing the window
 * wants to wait for.
 *
 * The grace period is kept by a thread of the host's, not by a check in the
 * pump: a game that stops pumping once it is told to close (it waits for a
 * worker that never ends, or sits in its fade-out) would never run that
 * check, and the player would be left with a window that answers nothing but
 * a force-quit. The thread tells the host first (hle_screen_abandon_hook),
 * so the host can say where the game was. */
static int g_close_asked;
void (*hle_screen_abandon_hook)(void);
static void *close_timer(void *arg) {
    (void)arg;
    usleep(3000 * 1000);
    fprintf(stderr, "[screen] 3 s after the close and the game is still running: leaving\n");
    if (hle_screen_abandon_hook) hle_screen_abandon_hook();
    hle_exit(0);
    return NULL;
}
/* Called with the machine held (from the pump, or a host thread inside mach_enter). */
void hle_screen_request_close(void) {
    uint32_t h = hle_first_hwnd();
    if (!h || g_close_asked) { fprintf(stderr, "[screen] close asked again: leaving now\n"); hle_exit(0); }
    fprintf(stderr, "[screen] close asked: WM_CLOSE to window 0x%08X; leaving in 3 s at the latest\n", h);
    hle_post_message(h, WM_CLOSE, 0, 0);
    g_close_asked = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, close_timer, NULL) == 0) pthread_detach(t);
    else hle_exit(0);
}
/* "Are you sure?": a quit by Cmd-Q or the close button asks first, in the
 * system's own box over the window (the game has no window message for a
 * close it would answer with a question of its own). The box is modal on
 * the game's thread, so the game stands still while it is up; a no leaves
 * the game as it was. Runs with the machine held, from the pump. */
static void ask_quit(const char *how) {
    if (g_close_asked) return;                           /* already on the way out */
    if (!g_win) { hle_screen_request_close(); return; }
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
    };
    char text[160];
    snprintf(text, sizeof text, "Quit %s? A game in progress is not saved.", SDL_GetWindowTitle(g_win));
    SDL_MessageBoxData box = { SDL_MESSAGEBOX_WARNING, g_win, SDL_GetWindowTitle(g_win), text,
                               (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL };
    int was_shown = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE, answer = 1;
    SDL_ShowCursor(SDL_ENABLE);                          /* the game hides the system cursor; the box needs it */
    if (SDL_ShowMessageBox(&box, &answer) != 0) { fprintf(stderr, "[screen] no message box (%s): leaving\n", SDL_GetError()); answer = 1; }
    SDL_ShowCursor(was_shown ? SDL_ENABLE : SDL_DISABLE);
    fprintf(stderr, "[screen] %s: %s\n", how, answer ? "quit" : "cancelled");
    if (answer) hle_screen_request_close();
}
void hle_screen_pump(void) {
    SDL_Event e;
    if (g_win) update_cursor();
    while (g_win && SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            ask_quit("close");
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            SDL_Scancode sc = e.key.keysym.scancode;
            /* a key the host keeps for itself (a mod switch): the game never sees it */
            if (hle_screen_key_hook && hle_screen_key_hook((int)sc, e.type == SDL_KEYDOWN)) break;
#ifdef __APPLE__
            /* Cmd-Q is the Mac's quit. SDL's own menu turns it into SDL_QUIT
             * when the menu gets it; when the key reaches the window instead
             * (a fullscreen window, a process started by another app), it is
             * the same request. */
            if (sc == SDL_SCANCODE_Q && (e.key.keysym.mod & KMOD_GUI)) {
                if (e.type == SDL_KEYDOWN) ask_quit("Cmd-Q");
                break;
            }
#endif
            /* the presenter's keys, the Windows build's: they do not reach the game */
            if ((sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) || sc == SDL_SCANCODE_F11) {
                if (e.type == SDL_KEYDOWN) {
                    g_fullscreen = !g_fullscreen;
                    SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    settings_changed();
                }
                break;
            }
            if (sc == SDL_SCANCODE_F12) {
                if (e.type == SDL_KEYDOWN) {
                    g_mode = (g_mode + 1) % 5;
                    fprintf(stderr, "[screen] scaling: %s\n", k_modes[g_mode]);
                    settings_changed();
                }
                break;
            }
            int vk = vk_of(e.key.keysym.scancode);
            if (vk) hle_input_key(e.type == SDL_KEYDOWN, (uint32_t)vk, scan_of(e.key.keysym.scancode));
            break;
        }
        case SDL_MOUSEMOTION: {
            int gx, gy;
            if (hle_screen_motion_hook) {
                int fx, fy;
                to_frame(e.motion.x, e.motion.y, &fx, &fy, 0);
                hle_screen_motion_hook(fx, fy);
            }
            to_game(e.motion.x, e.motion.y, &gx, &gy);
            hle_input_mouse(WM_MOUSEMOVE, gx, gy);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int gx, gy, down = e.type == SDL_MOUSEBUTTONDOWN;
            to_game(e.button.x, e.button.y, &gx, &gy);
            uint32_t m = e.button.button == SDL_BUTTON_RIGHT ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                       : e.button.button == SDL_BUTTON_MIDDLE ? (down ? WM_MBUTTONDOWN : WM_MBUTTONUP)
                       : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
            hle_input_mouse(m, gx, gy);
            break;
        }
        case SDL_MOUSEWHEEL: {
            if (!hle_screen_wheel_hook) break;
            int mx, my, fx, fy;
            SDL_GetMouseState(&mx, &my);
            to_frame(mx, my, &fx, &fy, 0);
#if SDL_VERSION_ATLEAST(2, 0, 18)
            float dy = e.wheel.preciseY;
#else
            float dy = (float)e.wheel.y;
#endif
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
            if (dy != 0) hle_screen_wheel_hook(dy, fx, fy);
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_EXPOSED) g_shown_seq = ~0u;
            break;
        default: break;
        }
    }
    g_last_pump = hle_ticks_ms();
    g_stat_pump++;
    present();
}

/* The guest moved the cursor (SetCursorPos): move the real one with it. */
static void warp(int x, int y) {
    int w, h, bpp;
    hle_dd_mode(&w, &h, &bpp);
    if (!g_win || g_dst.w <= 0 || w <= 0 || h <= 0) return;
    int src[4], dst[4];
    if (hle_screen_view_hook && hle_screen_view_hook(src, dst) && src[2] > 0 && src[3] > 0
        && x >= src[0] && x < src[0] + src[2] && y >= src[1] && y < src[1] + src[3]) {
        x = dst[0] + (x - src[0]) * dst[2] / src[2];
        y = dst[1] + (y - src[1]) * dst[3] / src[3];
    }
    SDL_WarpMouseInWindow(g_win, g_dst.x + x * g_dst.w / w, g_dst.y + y * g_dst.h / h);
}

/* The game drew into the primary. On the window's thread, show it (present
 * keeps to 60 a second), and keep the window's own events moving: a load
 * that does not pump for seconds would otherwise look hung to the desktop
 * (macOS's spinning cursor). The events wait in SDL's queue for the pump. */
static void primary_changed(void) {
    static uint32_t last_events;
    if (!g_ren || !pthread_equal(pthread_self(), g_video_thread)) return;
    uint32_t now = hle_ticks_ms();
    g_stat_heard++;
    /* A game in its loop pumps every frame, and the pump shows the frame. A
     * present from here too would show every change to the primary between
     * two pumps (RA2 draws its cursor there, animated while it hovers): up to
     * sixty a second, each a whole compose and upload on the game's thread.
     * So only a game that has stopped pumping (a slide, a load, a movie) is
     * shown from here. */
    if (now - g_last_pump < 50) return;
    if (now - g_last_present < 15) return;
    if (now - last_events >= 100) SDL_PumpEvents(), last_events = now;
    g_stat_primary++;
    present();
}

int hle_screen_open(const char *title, int fullscreen, int headless) {
    g_headless = headless, g_fullscreen = fullscreen;
    hle_set_pump_hook(hle_screen_pump);
    if (headless) return 0;
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      /* SIGINT and SIGTERM end the process: SDL would make them a close the game can refuse */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[screen] SDL: %s\n", SDL_GetError()); return -1; }
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_win) { fprintf(stderr, "[screen] window: %s\n", SDL_GetError()); return -1; }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) { fprintf(stderr, "[screen] renderer: %s\n", SDL_GetError()); return -1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_ShowCursor(SDL_DISABLE);                         /* the game draws its own */
    hle_cursor_hook = warp;
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(g_ren, &ri);
    g_video_thread = pthread_self();
    hle_dd_primary_hook = primary_changed;
    fprintf(stderr, "[screen] %s, renderer %s, scaling %s (F12), fullscreen F11\n", SDL_GetCurrentVideoDriver(), ri.name, k_modes[g_mode]);
    return 0;
}
