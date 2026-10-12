/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: QMK keycode numbers, the subset the engine knows. The values are QMK's "v6" numbering (Vial protocol
 * 6, vial-gui keycodes_v6.py), taken from vial-qmk quantum/keycodes.h (GPL-2.0-or-later): the numbers are the
 * protocol, they must match exactly.
 * A keycode is 16 bits:
 *   0x0000..0x00FF   basic: keyboard page usages (0x04..0xA4, 0xE0..0xE7), 0 KC_NO, 1 KC_TRNS;
 *                    0xA5..0xBE system / consumer and 0xCD..0xDF mouse (QMK's own codes, phase 3)
 *   0x0100..0x1FFF   QK_MODS: a basic key with modifiers, bits 8..12 = Ctrl Shift Alt GUI, Right
 *   0x5200.. TO, 0x5220.. MO, 0x5240.. DF, 0x5260.. TG (layer in the low 5 bits)
 *   0x7700..0x777F   QK_MACRO: macro n
 *   0x7C00           QK_BOOT
 *   0x7E00..0x7E3F   QK_KB: the keyboard's own codes (vial.json customKeycodes[i] = 0x7E00 + i) */
#pragma once

enum {
    KC_NO = 0x0000,
    KC_TRNS = 0x0001,
    KC_A = 0x0004, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G, KC_H, KC_I, KC_J, KC_K, KC_L, KC_M,
    KC_N, KC_O, KC_P, KC_Q, KC_R, KC_S, KC_T, KC_U, KC_V, KC_W, KC_X, KC_Y, KC_Z,          /* 0x1D */
    KC_1 = 0x001E, KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9, KC_0,                   /* 0x27 */
    KC_ENT = 0x0028, KC_ESC, KC_BSPC, KC_TAB, KC_SPC, KC_MINS, KC_EQL,                     /* 0x2E */
    KC_F1 = 0x003A,                                                                        /* .. F12 0x45 */
    KC_PGUP = 0x004B, KC_DEL = 0x004C, KC_END = 0x004D, KC_PGDN = 0x004E,
    KC_RGHT = 0x004F, KC_LEFT = 0x0050, KC_DOWN = 0x0051, KC_UP = 0x0052,
    KC_F13 = 0x0068, KC_F14, KC_F15, KC_F16, KC_F17, KC_F18, KC_F19, KC_F20, KC_F21, KC_F22, KC_F23, KC_F24,
    KC_KB_LAST = 0x00A4,                                                                   /* the last keyboard-page code */
    KC_PWR = 0x00A5, KC_SLEP, KC_WAKE,                                                     /* system control */
    KC_MUTE = 0x00A8, KC_VOLU, KC_VOLD, KC_MNXT, KC_MPRV, KC_MSTP, KC_MPLY,                /* .. consumer: 0xC2 */
    KC_CONSUMER_LAST = 0x00C2,
    KC_MS_U = 0x00CD, KC_MS_D, KC_MS_L, KC_MS_R,                                           /* mouse: cursor */
    KC_BTN1 = 0x00D1,                                                                      /* .. KC_BTN8 0xD8 */
    KC_WH_U = 0x00D9, KC_WH_D, KC_WH_L, KC_WH_R,                                           /* wheel */
    KC_MOUSE_LAST = 0x00DF,                                                                /* (0xDD..0xDF: acceleration) */
    KC_LCTL = 0x00E0, KC_LSFT, KC_LALT, KC_LGUI, KC_RCTL, KC_RSFT, KC_RALT, KC_RGUI,       /* 0xE7 */
};

#define QK_MODS 0x0100u
#define QK_MODS_MAX 0x1FFFu
#define QK_TO 0x5200u
#define QK_MOMENTARY 0x5220u
#define QK_DEF_LAYER 0x5240u
#define QK_TOGGLE_LAYER 0x5260u
#define QK_LAYER_MAX_OFS 0x1Fu        /* each of the four: 32 layers */
#define QK_MOD_TAP 0x2000u                  /* MT(mods, kc): mods at 8..12, kc 0..7 */
#define QK_MOD_TAP_MAX 0x3FFFu
#define QK_LAYER_TAP 0x4000u                /* LT(layer, kc): layer at 8..11, kc 0..7 */
#define QK_LAYER_TAP_MAX 0x4FFFu
#define QK_LAYER_MOD 0x5000u                /* LM(layer, mods): layer at 5..8, mods 0..4 */
#define QK_LAYER_MOD_MAX 0x51FFu
#define QK_ONE_SHOT_LAYER 0x5280u
#define QK_ONE_SHOT_MOD 0x52A0u             /* OSM(mods): 5 bits */
#define QK_LAYER_TAP_TOGGLE 0x52C0u         /* TT(layer) */
#define QK_TAP_DANCE 0x5700u
#define QK_TAP_DANCE_MAX 0x57FFu
#define QK_MACRO 0x7700u
#define QK_MACRO_MAX 0x777Fu
#define QK_BOOT 0x7C00u
#define QK_KB 0x7E00u
#define QK_KB_MAX 0x7E3Fu

/* QK_MODS bits (5 bits at 8..12): Ctrl, Shift, Alt, GUI, and "right-hand" for all of them */
#define MOD_BIT_CTL 0x01u
#define MOD_BIT_SFT 0x02u
#define MOD_BIT_ALT 0x04u
#define MOD_BIT_GUI 0x08u
#define MOD_BIT_RIGHT 0x10u
/* QK_MODS is the range's start, not a flag: LCTL(kc) = 0x0100 | kc, LSFT(kc) = 0x0200 | kc (QMK QK_LCTL, QK_LSFT) */
#define LCTL(kc) ((uint16_t)(MOD_BIT_CTL << 8 | (kc)))
#define LSFT(kc) ((uint16_t)(MOD_BIT_SFT << 8 | (kc)))
#define TO(n) ((uint16_t)(QK_TO + (n)))
#define MO(n) ((uint16_t)(QK_MOMENTARY + (n)))
#define DF(n) ((uint16_t)(QK_DEF_LAYER + (n)))
#define TG(n) ((uint16_t)(QK_TOGGLE_LAYER + (n)))
#define MT(m, kc) ((uint16_t)(QK_MOD_TAP | ((m) & 0x1Fu) << 8 | ((kc) & 0xFFu)))
#define LT(l, kc) ((uint16_t)(QK_LAYER_TAP | ((l) & 0xFu) << 8 | ((kc) & 0xFFu)))
#define LM(l, m) ((uint16_t)(QK_LAYER_MOD | ((l) & 0xFu) << 5 | ((m) & 0x1Fu)))
#define OSM(m) ((uint16_t)(QK_ONE_SHOT_MOD + ((m) & 0x1Fu)))
#define OSL(n) ((uint16_t)(QK_ONE_SHOT_LAYER + (n)))
#define TT(n) ((uint16_t)(QK_LAYER_TAP_TOGGLE + (n)))
#define TD(n) ((uint16_t)(QK_TAP_DANCE + (n)))
#define KC_IS_MOD(k) ((k) >= KC_LCTL && (k) <= KC_RGUI)
