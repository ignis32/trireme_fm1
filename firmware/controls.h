/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: the physical controls, as Vial sees them, and the default configuration.
 *
 * Positions (pos): 0..13 the buttons (felucca-bsp ids, also their LED ids), 14..40 the note keys F3..G5 (LED id
 * 14 + n). Vial matrix (vial.json must agree; tests/vialgui_test.py checks it):
 *   row 0, cols 0..13   the buttons by id: 0 OCT-, 1 OCT+, 2 FX, 3 SEL, 4 ENV, 5 LFO, 6 EDIT, 7 GLO, 8 HOME,
 *                       9 SAVE, 10 ARP, 11 SEQ, 12 PLAY, 13 REC
 *   row 1, cols 0..10   the black keys, low to high (F#3 .. F#5)
 *   row 2, cols 0..15   the white keys, low to high (F3 .. G5)
 * Encoders: the panel has 7 (felucca-bsp ids: 0 SELECT, 1 ALGORITHM, 2..5 KNOB1..4, 6 PRESETS); only KNOB1..4 are
 * assignable, as Vial encoders 0..3 ("knobs"), direction 0 CCW, 1 CW. MASTER (the pot) is not in Vial.
 *
 * Fixed in firmware, never assignable: SELECT, ALGORITHM, PRESETS (layer -/+), OCT- / OCT+ (OCT+: the editor;
 * together, held 10 s: UBOOT). OCT- / OCT+ stay in Vial's matrix because they are the unlock keys; what Vial stores
 * there is ignored (the defaults name their fixed job).
 *
 * Layers: KB_LAYERS of them, all alike (no hidden layers): PRESETS switches between them (cfg.layer, "the base"),
 * layer keys switch to one temporarily. The FM-1's own functions are keycodes (FK_*, QK_KB + i = vial.json
 * customKeycodes[i]). */
#pragma once

#define KB_LAYERS 5u                        /* five: the editor picks a hold layer with five buttons */
#define KB_ROWS 3u
#define KB_COLS 16u
#define KB_NENC 7u                          /* the panel's encoders */
#define KB_NKNOB 4u                         /* the assignable ones: KNOB1..4 */
#define KB_NBTN 14u
#define KB_NNOTE 27u
#define KB_NPOS (KB_NBTN + KB_NNOTE)
#define KB_MACROS 16u
#define KB_MACRO_BYTES 2048u
#define KB_TDS 16u                          /* tap dances (Vial dynamic entries) */
#define KB_COMBOS 16u                       /* combos: up to 4 keys -> a keycode */

enum { POS_OCTDN = 0, POS_OCTUP = 1 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB4 = 5, ENC_PRESETS = 6 };

/* the FM-1's functions: QK_KB + index; FK_NAMES is the vial.json customKeycodes shortName list, in order.
 * (Up to format 3 the list was PRESET- PRESET+ SYSTEM NAV- NAV+ VAL- VAL+ KB OFF: cfg_migrate renumbers.) */
enum { FK_LAYER_PREV = QK_KB, FK_LAYER_NEXT, FK_KB_OFF, FK_END };
#define FK_COUNT (FK_END - QK_KB)
static const char *const FK_NAMES[] = {"LAYER-", "LAYER+", "KB OFF"};
typedef char fk_names_ok[sizeof FK_NAMES / sizeof FK_NAMES[0] == FK_COUNT ? 1 : -1];

static const char *const BTN_NAMES[KB_NBTN] = {"OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME",
                                               "SAVE", "ARP", "SEQ", "PLAY", "REC"};
static const char *const KNOB_NAMES[KB_NKNOB] = {"KNOB1", "KNOB2", "KNOB3", "KNOB4"};

/* note n (0 = F3): black? (the octave from F: F F# G G# A A# B C C# D D# E) */
static int note_is_black(uint32_t n)
{
    return (0x52Au >> (n % 12u)) & 1u;      /* bits 1 3 5 8 10 of the 12 from F */
}

/* pos -> (row, col) */
static void pos_rowcol(uint32_t pos, uint8_t *row, uint8_t *col)
{
    uint32_t n, i, k = 0;
    if (pos < KB_NBTN) {
        *row = 0;
        *col = (uint8_t)pos;
        return;
    }
    n = pos - KB_NBTN;
    for (i = 0; i < n; i++)                 /* keys of the same colour below this one */
        if (note_is_black(i) == note_is_black(n))
            k++;
    *row = note_is_black(n) ? 1u : 2u;
    *col = (uint8_t)k;
}

/* (row, col) -> pos, or -1: no control there */
static int32_t rowcol_pos(uint32_t row, uint32_t col)
{
    uint32_t p;
    for (p = 0; p < KB_NPOS; p++) {
        uint8_t r, c;
        pos_rowcol(p, &r, &c);
        if (r == row && c == col)
            return (int32_t)p;
    }
    return -1;
}

/* the default keymap: layer 1 (0) the MVP's layout (PLAY = play / pause), the other layers SAME (KC_TRNS) */
static uint16_t default_key(uint32_t layer, uint32_t pos)
{
    if (pos == POS_OCTDN)
        return FK_LAYER_PREV;               /* (fixed in firmware: what Vial shows says what they do) */
    if (pos == POS_OCTUP)
        return FK_LAYER_NEXT;
    if (layer != 0)
        return KC_TRNS;
    if (pos == 12u)
        return KC_MPLY;                     /* PLAY: play / pause */
    if (pos < KB_NBTN)
        return (uint16_t)(KC_F13 + (pos - 2u));   /* the other buttons: F13 .. F24 (F23 unused: PLAY's) */
    pos -= KB_NBTN;
    return pos < 26u ? (uint16_t)(KC_A + pos) : (uint16_t)KC_SPC;   /* the keys: A .. Z, Space */
}

/* knob k (0..3 = KNOB1..4), direction cw */
static uint16_t default_knob(uint32_t layer, uint32_t k, uint32_t cw)
{
    /* KNOB1 volume, KNOB2 the scroll wheel (clockwise: down, as a wheel rolled towards you), KNOB3 / 4 arrows */
    static const uint16_t KNOBS[KB_NKNOB][2] = {{KC_VOLD, KC_VOLU}, {KC_WH_U, KC_WH_D}, {KC_LEFT, KC_RGHT},
                                                {KC_DOWN, KC_UP}};
    return layer == 0 && k < KB_NKNOB ? KNOBS[k][cw != 0] : (uint16_t)KC_TRNS;
}
