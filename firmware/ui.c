/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: the screen, portable: kbcore.c's state in, gfx.c out. Names of keycodes (long,
 * for the detail line; short, for the legend's cells) and of controls; the NORMAL screen: a status line, a legend
 * of what the controls do now (two pages: PANEL = the buttons and the knobs, KEYS = the note keys), and the detail
 * of the control touched last. Every region is redrawn only when its text or colours change. */
#include "gfx.c"

#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_GREY 0x8410u
#define C_DARK 0x2945u
#define C_GREEN 0x07E0u
#define C_YELLOW 0xFFE0u
#define C_BLUE 0x001Fu
#define C_CYAN 0x07FFu
#define C_RED 0xF800u

/* ---------------------------------------------------------------- names */
static char *ui_put(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
static char *ui_num(char *b, uint32_t v)
{
    char d[10];
    uint32_t n = 0;
    do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        *b++ = d[--n];
    *b = 0;
    return b;
}
static char *ui_hex(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 4; i--;)
        *b++ = "0123456789ABCDEF"[(v >> (4u * i)) & 15u];
    *b = 0;
    return b;
}

/* basic keycodes (keyboard page): a long and a short name; NULL: not in the table */
typedef struct { uint8_t kc; const char *l, *s; } kname_t;
static const kname_t KNAMES[] = {
    {0x28, "ENTER", "ENT"}, {0x29, "ESC", "ESC"}, {0x2A, "BACKSPACE", "BSPC"}, {0x2B, "TAB", "TAB"},
    {0x2C, "SPACE", "SPC"}, {0x2D, "-", "-"}, {0x2E, "=", "="}, {0x2F, "[", "["}, {0x30, "]", "]"},
    {0x31, "\\", "\\"}, {0x32, "#", "#"}, {0x33, ";", ";"}, {0x34, "'", "'"}, {0x35, "`", "`"}, {0x36, ",", ","},
    {0x37, ".", "."}, {0x38, "/", "/"}, {0x39, "CAPS LOCK", "CAPS"}, {0x46, "PRINT SCREEN", "PRSC"},
    {0x47, "SCROLL LOCK", "SCRL"}, {0x48, "PAUSE", "PAUS"}, {0x49, "INSERT", "INS"}, {0x4A, "HOME", "HOME"},
    {0x4B, "PAGE UP", "PGUP"}, {0x4C, "DELETE", "DEL"}, {0x4D, "END", "END"}, {0x4E, "PAGE DOWN", "PGDN"},
    {0x4F, "RIGHT", FONT_SYM_RIGHT}, {0x50, "LEFT", FONT_SYM_LEFT}, {0x51, "DOWN", FONT_SYM_ARROW_DOWN},
    {0x52, "UP", FONT_SYM_ARROW_UP}, {0x53, "NUM LOCK", "NUML"}, {0x54, "KP /", "K/"}, {0x55, "KP *", "K*"},
    {0x56, "KP -", "K-"}, {0x57, "KP +", "K+"}, {0x58, "KP ENTER", "KENT"}, {0x63, "KP .", "K."},
    {0x64, "\\ (ISO)", "\\"}, {0x65, "MENU", "MENU"}, {0x67, "KP =", "K="},
    {0xE0, "LEFT CTRL", "CTRL"}, {0xE1, "LEFT SHIFT", "SHFT"}, {0xE2, "LEFT ALT", "ALT"}, {0xE3, "LEFT GUI", "GUI"},
    {0xE4, "RIGHT CTRL", "RCTL"}, {0xE5, "RIGHT SHIFT", "RSFT"}, {0xE6, "RIGHT ALT", "RALT"},
    {0xE7, "RIGHT GUI", "RGUI"},
};
static const char *const MEDIA_L[] = {"POWER", "SLEEP", "WAKE", "MUTE", "VOLUME +", "VOLUME -", "NEXT TRACK",
                                      "PREVIOUS TRACK", "STOP", "PLAY / PAUSE", "MEDIA", "EJECT", "MAIL",
                                      "CALCULATOR", "MY COMPUTER", "SEARCH", "BROWSER HOME", "BACK", "FORWARD",
                                      "BROWSER STOP", "REFRESH", "FAVORITES", "FAST FORWARD", "REWIND",
                                      "BRIGHTNESS +", "BRIGHTNESS -", "CONTROL PANEL", "ASSISTANT",
                                      "MISSION CONTROL", "LAUNCHPAD"};
static const char *const MEDIA_S[] = {"PWR", "SLP", "WAKE", "MUTE", "VOL+", "VOL-", "NEXT", "PREV", "STOP", "PLAY",
                                      "MED", "EJCT", "MAIL", "CALC", "PC", "SRCH", "WWW", "BACK", "FWD", "WSTP",
                                      "RFSH", "FAV", "FFWD", "RWND", "BRI+", "BRI-", "CPNL", "ASST", "MCTL", "LPAD"};
static const char *const MOUSE_L[] = {"MOUSE UP", "MOUSE DOWN", "MOUSE LEFT", "MOUSE RIGHT", "MOUSE BUTTON 1",
                                      "MOUSE BUTTON 2", "MOUSE BUTTON 3", "MOUSE BUTTON 4", "MOUSE BUTTON 5",
                                      "MOUSE BUTTON 6", "MOUSE BUTTON 7", "MOUSE BUTTON 8", "WHEEL UP",
                                      "WHEEL DOWN", "WHEEL LEFT", "WHEEL RIGHT", "MOUSE SPEED 0",
                                      "MOUSE SPEED 1", "MOUSE SPEED 2"};
static const char *const MOUSE_S[] = {"M" FONT_SYM_ARROW_UP, "M" FONT_SYM_ARROW_DOWN, "M" FONT_SYM_LEFT,
                                      "M" FONT_SYM_RIGHT, "MB1", "MB2", "MB3", "MB4", "MB5", "MB6", "MB7", "MB8",
                                      "W" FONT_SYM_ARROW_UP, "W" FONT_SYM_ARROW_DOWN, "W" FONT_SYM_LEFT,
                                      "W" FONT_SYM_RIGHT, "MS0", "MS1", "MS2"};

/* a basic keycode (0x00..0xFF) */
static char *kc_basic(uint32_t kc, char *b, int shrt)
{
    uint32_t i;
    if (kc >= KC_A && kc <= KC_Z) {
        *b++ = (char)('A' + (kc - KC_A));
        *b = 0;
        return b;
    }
    if (kc >= KC_1 && kc <= KC_0) {
        *b++ = kc == KC_0 ? '0' : (char)('1' + (kc - KC_1));
        *b = 0;
        return b;
    }
    if (kc >= KC_F1 && kc < KC_F1 + 12u)
        return ui_num(ui_put(b, "F"), 1u + (kc - KC_F1));
    if (kc >= KC_F13 && kc <= KC_F24)
        return ui_num(ui_put(b, "F"), 13u + (kc - KC_F13));
    if (kc >= 0x59u && kc <= 0x62u) {       /* keypad 1..9, 0 */
        b = ui_put(b, shrt ? "K" : "KP ");
        *b++ = kc == 0x62u ? '0' : (char)('1' + (kc - 0x59u));
        *b = 0;
        return b;
    }
    for (i = 0; i < sizeof KNAMES / sizeof KNAMES[0]; i++)
        if (KNAMES[i].kc == kc)
            return ui_put(b, shrt ? KNAMES[i].s : KNAMES[i].l);
    if (kc >= KC_PWR && kc <= KC_CONSUMER_LAST)
        return ui_put(b, (shrt ? MEDIA_S : MEDIA_L)[kc - KC_PWR]);
    if (kc >= KC_MS_U && kc <= KC_MOUSE_LAST)
        return ui_put(b, (shrt ? MOUSE_S : MOUSE_L)[kc - KC_MS_U]);
    return ui_hex(ui_put(b, shrt ? "" : "KEY "), kc);
}

/* modifiers (QMK's 5 bits): long "CTRL+SHIFT+", short "CS" */
static char *kc_mods(uint32_t m, char *b, int shrt)
{
    static const char *const L[4] = {"CTRL+", "SHIFT+", "ALT+", "GUI+"};
    static const char S[4] = {'C', 'S', 'A', 'G'};
    uint32_t i;
    if (!shrt && (m & MOD_BIT_RIGHT))
        b = ui_put(b, "R");
    for (i = 0; i < 4u; i++)
        if (m >> i & 1u) {
            if (shrt)
                *b++ = S[i];
            else
                b = ui_put(b, L[i]);
        }
    *b = 0;
    return b;
}

/* a keycode's name: long (up to 20 characters, the detail line) or short (up to 4, a legend cell) */
static void kc_label(uint16_t kc, char *b, int shrt)
{
    uint32_t n = kc & QK_LAYER_MAX_OFS;
    if (kc == KC_NO) { ui_put(b, shrt ? "" : "NOTHING"); return; }
    if (kc == KC_TRNS) { ui_put(b, shrt ? FONT_SYM_SAME : "SAME"); return; }
    if (kc <= 0xFFu) { kc_basic(kc, b, shrt); return; }
    if (kc >= QK_MODS && kc <= QK_MODS_MAX) {
        if (shrt) {
            b = kc_mods(kc >> 8 & 0x1Fu, b, 1);
            *b++ = '+';
            kc_basic(kc & 0xFFu, b, 1);
        } else
            kc_basic(kc & 0xFFu, kc_mods(kc >> 8 & 0x1Fu, b, 0), 0);
        return;
    }
    if (kc >= QK_MOD_TAP && kc <= QK_MOD_TAP_MAX) {   /* TAP x / HOLD mods */
        if (shrt) {
            kc_basic(kc & 0xFFu, b, 1);
            return;
        }
        b = kc_basic(kc & 0xFFu, ui_put(b, "TAP "), 0);
        b = kc_mods(kc >> 8 & 0x1Fu, ui_put(b, " HOLD "), 0);
        b[-1] = 0;                          /* (the last '+') */
        return;
    }
    if (kc >= QK_LAYER_TAP && kc <= QK_LAYER_TAP_MAX) {
        if (shrt) {
            kc_basic(kc & 0xFFu, b, 1);
            return;
        }
        b = kc_basic(kc & 0xFFu, ui_put(b, "TAP "), 0);
        ui_num(ui_put(b, " HOLD L"), kc >> 8 & 0xFu);
        return;
    }
    if (kc >= QK_LAYER_MOD && kc <= QK_LAYER_MOD_MAX) {
        b = ui_num(ui_put(b, shrt ? "H" : "HOLD L"), kc >> 5 & 0xFu);
        if (!shrt) {
            b = kc_mods(kc & 0x1Fu, ui_put(b, " "), 0);
            b[-1] = 0;
        } else
            ui_put(b, "+");
        return;
    }
    if (kc >= QK_TO && kc <= QK_TO + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "GO" : "GO TO L"), n); return; }
    if (kc >= QK_DEF_LAYER && kc <= QK_DEF_LAYER + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "GO" : "GO TO L"), n); return; }
    if (kc >= QK_MOMENTARY && kc <= QK_MOMENTARY + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "HLD" : "HOLD L"), n); return; }
    if (kc >= QK_TOGGLE_LAYER && kc <= QK_TOGGLE_LAYER + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "TG" : "TOGGLE L"), n); return; }
    if (kc >= QK_ONE_SHOT_LAYER && kc <= QK_ONE_SHOT_LAYER + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "1S" : "ONE-SHOT L"), n); return; }
    if (kc >= QK_LAYER_TAP_TOGGLE && kc <= QK_LAYER_TAP_TOGGLE + QK_LAYER_MAX_OFS) { ui_num(ui_put(b, shrt ? "TT" : "TAP-TOGGLE L"), n); return; }
    if (kc >= QK_ONE_SHOT_MOD && kc <= QK_ONE_SHOT_MOD + 0x1Fu) {
        if (shrt) {
            b = ui_put(b, "1S");
            kc_mods(kc & 0x1Fu, b, 1);
        } else {
            b = kc_mods(kc & 0x1Fu, ui_put(b, "ONE-SHOT "), 0);
            b[-1] = 0;
        }
        return;
    }
    if (kc >= QK_MACRO && kc <= QK_MACRO_MAX) { ui_num(ui_put(b, shrt ? "M" : "MACRO "), kc - QK_MACRO); return; }
    if (kc >= QK_TAP_DANCE && kc <= QK_TAP_DANCE_MAX) { ui_num(ui_put(b, shrt ? "TD" : "TAP DANCE "), kc - QK_TAP_DANCE); return; }
    if (kc == FK_LAYER_PREV) { ui_put(b, shrt ? "L-" : "LAYER -"); return; }
    if (kc == FK_LAYER_NEXT) { ui_put(b, shrt ? "L+" : "LAYER +"); return; }
    if (kc == FK_KB_OFF) { ui_put(b, shrt ? "OFF" : "KEYBOARD OFF"); return; }
    if (kc == QK_BOOT) { ui_put(b, shrt ? "BOOT" : "BOOTLOADER"); return; }
    ui_hex(ui_put(b, shrt ? "" : "CODE "), kc);
}

/* a control's name: a button's printed label, a key's note (F3 .. G5), a knob and its direction (pos 100 + 2k + cw) */
static void pos_label(int32_t pos, char *b)
{
    static const char *const NOTE[12] = {"F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"};
    uint32_t n;
    if (pos >= 200) { ui_put(b, "COMBO"); return; }
    if (pos >= 100) {
        b = ui_put(b, KNOB_NAMES[(pos - 100) / 2]);
        ui_put(b, (pos - 100) & 1 ? " " FONT_SYM_RIGHT : " " FONT_SYM_LEFT);
        return;
    }
    if (pos < (int32_t)KB_NBTN) { ui_put(b, BTN_NAMES[pos]); return; }
    n = (uint32_t)pos - KB_NBTN;            /* F3 = 0; the octave number changes at C */
    b = ui_put(b, NOTE[n % 12u]);
    ui_num(b, 3u + (n + 5u) / 12u);
}

/* ---------------------------------------------------------------- what the legend shows */
/* ui_view < 0: what the controls do now (the layers on, resolved); else that layer's stored bindings (the editor) */
static int32_t ui_view = -1;
static uint16_t ui_kc_at(uint32_t pos)
{
    uint8_t r, c;
    if (ui_view < 0)
        return resolve_key(pos);
    pos_rowcol(pos, &r, &c);
    return km_get((uint32_t)ui_view, r, c);
}
static uint16_t ui_knob_at(uint32_t k, uint32_t cw)
{
    return ui_view < 0 ? resolve_enc(k, cw) : enc_get((uint32_t)ui_view, k, cw);
}

/* ---------------------------------------------------------------- lights: a level per LED */
enum { LV_OFF, LV_DIM, LV_MID, LV_ON, LV_BREATHE, LV_BLINK, LV_BLINK_SLOW };
/* AUTO: the style from the binding */
static uint32_t light_auto(uint16_t kc)
{
    if (kc == KC_NO || kc == KC_TRNS)
        return LIGHT_OFF;
    if (kc >= QK_MACRO && kc <= QK_MACRO_MAX)
        return LIGHT_ON;
    if (kc >= QK_KB && kc <= QK_KB_MAX)
        return LIGHT_BREATHE;
    if ((kc >= KC_LCTL && kc <= KC_RGUI) || kc >= QK_TO)
        return LIGHT_MID;                   /* modifiers alone, layer keys, one-shots, tap dances */
    if ((kc & 0xFF00u) && !(kc & 0xFFu) && kc <= QK_MODS_MAX)
        return LIGHT_MID;                   /* modifiers without a key */
    return LIGHT_DIM;
}
/* a light style (LIGHT_*) as an LED level */
static uint32_t light_level(uint32_t style)
{
    static const uint8_t LV[LIGHT_STYLES] = {LV_OFF, LV_OFF, LV_DIM, LV_MID, LV_ON, LV_BREATHE, LV_BLINK};
    return style < LIGHT_STYLES ? LV[style] : LV_OFF;
}
/* control pos's style on a layer, AUTO resolved with kc (what it is bound to there) */
static uint32_t light_style(uint32_t layer, uint32_t pos, uint16_t kc)
{
    uint32_t st = layer < KB_LAYERS && pos < KB_NPOS ? cfg.light[layer][pos] : LIGHT_AUTO;
    return st == LIGHT_AUTO ? light_auto(kc) : st;
}
/* NORMAL: each control as styled on the layer it resolves from; a control held: lit */
static uint32_t ui_led_normal(uint32_t pos, uint32_t held)
{
    uint8_t r, c;
    uint32_t i = kb.ls_n;
    if (held)
        return LV_ON;
    if (pos == POS_OCTDN || pos == POS_OCTUP)
        return pos == POS_OCTUP ? LV_ON : LV_OFF;   /* OCT+ opens the editor; OCT- does nothing here */
    pos_rowcol(pos, &r, &c);
    while (i--)                             /* the layer its binding comes from (SAME falls through) */
        if (km_get(kb.ls_layer[i], r, c) != KC_TRNS)
            return light_level(light_style(kb.ls_layer[i], pos, km_get(kb.ls_layer[i], r, c)));
    return light_level(light_style(cfg.layer, pos, km_get(cfg.layer, r, c)));
}

/* ---------------------------------------------------------------- regions: redrawn when their content changes */
enum {
    R_STATUS_L, R_STATUS_R, R_PAGE,
    R_CELL0,                                /* the legend's cells: buttons 2..13, then knobs (2 each), then keys */
    R_DETAIL_POS = R_CELL0 + 12u + 8u + KB_NNOTE, R_DETAIL_KC, R_BOTTOM,
    R_N                                     /* (editor.c's regions follow, up to UI_REGIONS) */
};
#define UI_REGIONS (R_N + 64u)
typedef struct {
    char s[24];
    uint16_t fg, bg;
    uint8_t valid;
} ui_region_t;
static ui_region_t ui_reg[UI_REGIONS];

static void ui_invalidate(void)
{
    uint32_t i;
    for (i = 0; i < UI_REGIONS; i++)
        ui_reg[i].valid = 0;
}

/* region id: text s in (x, y, w, h), centred (c) or left-aligned, a frame if frame != 0 (its colour) */
static void ui_box(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *s, uint32_t font,
                   uint32_t scale, uint16_t fg, uint16_t bg, uint16_t frame, int centred)
{
    ui_region_t *r = &ui_reg[id];
    uint32_t i, th = gfx_ch(font, scale);
    for (i = 0; s[i] && i + 1u < sizeof r->s; i++)
        if (r->s[i] != s[i])
            break;
    if (r->valid && !s[i] && !r->s[i] && r->fg == fg && r->bg == bg)
        return;
    for (i = 0; s[i] && i + 1u < sizeof r->s; i++)
        r->s[i] = s[i];
    r->s[i] = 0;
    r->fg = fg;
    r->bg = bg;
    r->valid = 1;
    gfx_fill(x, y, w, h, bg);
    if (frame)
        gfx_frame(x, y, w, h, frame);
    if (centred)
        gfx_text_c(frame ? x + 1u : x, frame ? w - 2u : w, y + (h > th ? (h - th) / 2u : 0u), r->s, font, scale, fg, bg);
    else
        gfx_text_clip(x, y + (h > th ? (h - th) / 2u : 0u), x + w, r->s, font, scale, fg, bg);
}

/* ---------------------------------------------------------------- the NORMAL screen */
typedef struct {                            /* from the platform (main.c) */
    uint8_t usb;                            /* 0 keyboard off, 1 no USB, 2 no host, 3 suspended, 4 ready */
    uint8_t vial;                           /* 0 locked, 1 unlocked, 2 unlock waiting for the keys */
    uint16_t unlock_permille;
    uint8_t saved, save_error;              /* SAVED shows a second; the flash refuses */
    uint32_t now;
} ui_status_t;

enum { PG_PANEL, PG_KEYS, PG_N };
static struct {
    uint8_t page, drawn_page;
    int16_t last_pos;                       /* the control touched last (-1 none), for the detail and the page */
    uint32_t last_t;
    int32_t shown_last;
    uint8_t cleared;
} un = {PG_PANEL, 0xFF, -1, 0, -2, 0};

#define Y_STATUS 0u
#define Y_PAGE 20u
#define Y_KNOBS 40u                         /* PANEL, as on the panel: KNOB1..4 (clockwise, anticlockwise) */
#define Y_ROW_A 100u                        /* the buttons FX..GLO */
#define Y_ROW_B 126u                        /* HOME..REC */
#define Y_BLACK 40u                         /* KEYS: the black keys, then the white */
#define Y_WHITE 80u
#define Y_DETAIL 160u
#define CELL_H 26u
#define HILITE_MS 2000u

static const char *const USB_S[] = {"KB OFF", "NO USB", "NO HOST", "SLEEP", "USB"};

static void ui_status(const ui_status_t *st)
{
    char b[24], *p = ui_put(b, "LAYER ");
    uint16_t c;
    p = ui_num(p, cfg.layer);
    if (kb.ls_n) {
        p = ui_put(p, kb.tg_layer != POS_NONE && layer_top() == kb.tg_layer ? " TG " : " + ");
        ui_num(p, layer_top());
    }
    ui_box(R_STATUS_L, 0, Y_STATUS, 130, 18, b, F_S, 2, C_WHITE, C_BLACK, 0, 0);
    if (kb.off) {
        ui_put(b, "KB OFF");
        c = C_RED;
    } else if (st->save_error) {
        ui_put(b, "SAVE ERR");
        c = C_RED;
    } else if (st->saved) {
        ui_put(b, "SAVED");
        c = C_GREEN;
    } else if (st->usb != 4u) {
        ui_put(b, USB_S[st->usb]);
        c = C_YELLOW;
    } else {
        ui_put(b, st->vial == 1u ? "VIAL OPEN" : "READY");
        c = st->vial == 1u ? C_YELLOW : C_GREEN;
    }
    ui_box(R_STATUS_R, 130, Y_STATUS, 110, 18, b, F_S, 2, c, C_BLACK, 0, 0);
}

/* the cell of a control, framed like a key: its short label, highlighted while it is the one touched last; a black
 * key's cell dark */
static void ui_cell(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t kc, int hi, int black)
{
    char b[24];
    uint16_t bg = hi ? C_WHITE : black ? C_DARK : C_BLACK, fg = hi ? C_BLACK : C_WHITE;
    kc_label(kc, b, 1);
    ui_box(id, x, y, w, h, b, F_S, 2, fg, bg, hi ? C_WHITE : C_GREY, 1);
}

static void ui_page_panel(int32_t hi)
{
    uint32_t i, k;
    for (i = 0; i < 12u; i++) {             /* FX..GLO, HOME..REC: 6 + 6, 40 px each */
        uint32_t pos = 2u + i;
        ui_cell(R_CELL0 + i, (i % 6u) * 40u, i < 6u ? Y_ROW_A : Y_ROW_B, 40, CELL_H, ui_kc_at(pos),
                hi == (int32_t)pos, 0);
    }
    for (k = 0; k < KB_NKNOB; k++) {        /* KNOB1..4: clockwise above, anticlockwise below */
        ui_cell(R_CELL0 + 12u + 2u * k, k * 60u, Y_KNOBS, 60, CELL_H, ui_knob_at(k, 1),
                hi == 100 + 2 * (int32_t)k + 1, 0);
        ui_cell(R_CELL0 + 13u + 2u * k, k * 60u, Y_KNOBS + CELL_H, 60, CELL_H, ui_knob_at(k, 0),
                hi == 100 + 2 * (int32_t)k, 0);
    }
}

static void ui_page_keys(int32_t hi)
{
    uint32_t n, w = 0;
    for (n = 0; n < KB_NNOTE; n++) {        /* white keys 15 px wide in a row; a black key over the gap before */
        uint32_t pos = KB_NBTN + n;
        if (note_is_black(n))
            ui_cell(R_CELL0 + 20u + n, w * 15u - 7u, Y_BLACK, 14, 34, ui_kc_at(pos), hi == (int32_t)pos, 1);
        else {
            ui_cell(R_CELL0 + 20u + n, w * 15u, Y_WHITE, 15, 50, ui_kc_at(pos), hi == (int32_t)pos, 0);
            w++;
        }
    }
}

static void ui_normal(const ui_status_t *st)
{
    int32_t hi = -1;
    char b[24];
    if (kb.last_pos != un.shown_last) {     /* a control touched: its page, highlighted, for a while */
        un.shown_last = kb.last_pos;
        if (kb.last_pos >= 0 && kb.last_pos < 200 && kb.last_pos != POS_OCTDN && kb.last_pos != POS_OCTUP) {
            un.last_pos = kb.last_pos;
            un.last_t = st->now;
            un.page = kb.last_pos >= (int16_t)KB_NBTN && kb.last_pos < (int16_t)KB_NPOS ? PG_KEYS : PG_PANEL;
        }
    }
    if (kb.sel_steps) {                     /* SELECT pages by hand */
        un.page = (uint8_t)((un.page + PG_N + (kb.sel_steps > 0 ? 1u : PG_N - 1u)) % PG_N);
        kb.sel_steps = 0;
    }
    if (un.last_pos >= 0 && st->now - un.last_t < HILITE_MS)
        hi = un.last_pos;
    if (un.page != un.drawn_page) {
        uint32_t i;
        un.drawn_page = un.page;
        gfx_fill(0, Y_PAGE, GFX_W, Y_DETAIL - Y_PAGE, C_BLACK);
        for (i = R_CELL0; i < R_DETAIL_POS; i++)
            ui_reg[i].valid = 0;
    }
    ui_status(st);
    ui_box(R_PAGE, 0, Y_PAGE, GFX_W, 16, un.page == PG_PANEL ? "PANEL     SELECT: KEYS" : "KEYS     SELECT: PANEL",
           F_S, 2, C_GREY, C_BLACK, 0, 0);
    if (un.page == PG_PANEL)
        ui_page_panel(hi);
    else
        ui_page_keys(hi);
    if (un.last_pos < 0) {                  /* the detail: the control touched last, what it does now */
        ui_box(R_DETAIL_POS, 0, Y_DETAIL, GFX_W, 18, "", F_S, 2, C_GREY, C_BLACK, 0, 0);
        ui_box(R_DETAIL_KC, 0, Y_DETAIL + 20u, GFX_W, 26, "PRESS A KEY", F_L, 2, C_GREY, C_BLACK, 0, 0);
    } else {
        uint16_t kc = un.last_pos >= 100 ? resolve_enc((uint32_t)(un.last_pos - 100) / 2u,
                                                        (uint32_t)(un.last_pos - 100) & 1u) :
                                           resolve_key((uint32_t)un.last_pos);
        pos_label(un.last_pos, b);
        ui_box(R_DETAIL_POS, 0, Y_DETAIL, GFX_W, 18, b, F_S, 2, C_GREY, C_BLACK, 0, 0);
        kc_label(kc, b, 0);
        ui_box(R_DETAIL_KC, 0, Y_DETAIL + 20u, GFX_W, 26, b, F_L, 2, C_WHITE, C_BLACK, 0, 0);
    }
    if (st->vial == 2u) {                   /* the unlock's progress: the bottom line */
        uint32_t w = st->unlock_permille * 200u / 1000u;
        if (ui_reg[R_BOTTOM].s[0] != 'H')   /* (the hint was there) */
            gfx_fill(0, 206, GFX_W, 34, C_BLACK);
        ui_box(R_BOTTOM, 0, 206, GFX_W, 16, "HOLD OCT- + OCT+", F_S, 2, C_YELLOW, C_BLACK, 0, 1);
        gfx_frame(18, 224, 204, 14, C_GREY);
        gfx_fill(20, 226, w, 10, C_GREEN);
        gfx_fill(20 + w, 226, 200 - w, 10, C_BLACK);
    } else {                                /* the hint: how to edit */
        if (!un.cleared || ui_reg[R_BOTTOM].s[0] == 'H') {   /* (from the unlock's lines, or at first: clear) */
            un.cleared = 1;
            gfx_fill(0, 206, GFX_W, 34, C_BLACK);
            ui_reg[R_BOTTOM].valid = 0;
        }
        ui_box(R_BOTTOM, 0, 222, GFX_W, 18, "OCT+ EDITOR", F_S, 2, C_GREY, C_BLACK, 0, 1);
    }
}
