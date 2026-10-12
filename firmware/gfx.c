/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: drawing on the 240 x 240 screen, portable (the firmware draws to the LCD, tests/screen_test.c to a
 * picture). Text in two fonts (font.h: S 5 x 8, L 6 x 12, cells with their spacing) at any scale, rectangles.
 * The includer defines:
 *   gfx_fill(x, y, w, h, c)        a filled rectangle, RGB565
 *   gfx_blit(x, y, w, h, px)       w x h pixels, RGB565 in the panel's byte order if GFX_PANEL_SWAP is 1
 *   gfx_sync()                     the last blit has left its buffer (the next one reuses it)
 * Characters are drawn one at a time from a small buffer, as the old text.c did. */
#include "font.h"

static void gfx_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c);
static void gfx_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);
static void gfx_sync(void);

#define GFX_W 240u
#define GFX_H 240u
enum { F_S, F_L };                          /* the fonts */
#define GFX_MAXSCALE 3u
static uint16_t gfx_buf[FONT_L_W * GFX_MAXSCALE * FONT_L_H * GFX_MAXSCALE];

static uint32_t gfx_cw(uint32_t font, uint32_t scale) { return (font == F_L ? FONT_L_W : FONT_S_W) * scale; }
static uint32_t gfx_ch(uint32_t font, uint32_t scale) { return (font == F_L ? FONT_L_H : FONT_S_H) * scale; }

static uint32_t gfx_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static uint32_t gfx_text_w(const char *s, uint32_t font, uint32_t scale) { return gfx_len(s) * gfx_cw(font, scale); }

static const uint8_t *gfx_glyph(uint8_t c, uint32_t font)
{
    uint32_t i = c >= FONT_FIRST && c <= FONT_LAST ? c - FONT_FIRST :
                 c >= 0x80u && c < 0x80u + FONT_SYMS ? (FONT_LAST - FONT_FIRST + 1u) + (c - 0x80u) : 0u;
    return font == F_L ? FONT_L[i] : FONT_S[i];
}

static uint16_t gfx_px(uint16_t c)
{
#if GFX_PANEL_SWAP
    return (uint16_t)(c >> 8 | c << 8);
#else
    return c;
#endif
}

/* text from (x, y), left-aligned; characters that would cross x_end are not drawn. Returns the x after it. */
static uint32_t gfx_text_clip(uint32_t x, uint32_t y, uint32_t x_end, const char *s, uint32_t font, uint32_t scale,
                              uint16_t fg, uint16_t bg)
{
    uint32_t w, h;
    uint16_t f = gfx_px(fg), b = gfx_px(bg);
    if (scale > GFX_MAXSCALE)
        scale = GFX_MAXSCALE;
    w = gfx_cw(font, scale);
    h = gfx_ch(font, scale);
    for (; *s; s++, x += w) {
        const uint8_t *g = gfx_glyph((uint8_t)*s, font);
        uint32_t r, c, cw = w;
        if (x + w > x_end) {                /* the last character may lose its spacing column, no more */
            if (s[1] || x + w - scale > x_end)
                break;
            cw = w - scale;
        }
        gfx_sync();
        for (r = 0; r < h; r++)
            for (c = 0; c < cw; c++)
                gfx_buf[r * cw + c] = (g[r / scale] << (c / scale)) & 0x80u ? f : b;
        gfx_blit(x, y, cw, h, gfx_buf);
    }
    gfx_sync();
    return x;
}
static uint32_t gfx_text(uint32_t x, uint32_t y, const char *s, uint32_t font, uint32_t scale, uint16_t fg,
                         uint16_t bg)
{
    return gfx_text_clip(x, y, GFX_W, s, font, scale, fg, bg);
}

/* text centred in [x, x + w) */
static void gfx_text_c(uint32_t x, uint32_t w, uint32_t y, const char *s, uint32_t font, uint32_t scale, uint16_t fg,
                       uint16_t bg)
{
    uint32_t tw = gfx_text_w(s, font, scale);
    if (tw > w && tw - scale <= w)
        tw -= scale;                        /* (fits without the last spacing column) */
    gfx_text_clip(tw < w ? x + (w - tw) / 2u : x, y, x + w, s, font, scale, fg, bg);
}

/* a 1 px frame */
static void gfx_frame(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    gfx_fill(x, y, w, 1, c);
    gfx_fill(x, y + h - 1u, w, 1, c);
    gfx_fill(x, y, 1, h, c);
    gfx_fill(x + w - 1u, y, 1, h, c);
}
