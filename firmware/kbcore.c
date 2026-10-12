/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard core: the configuration, the key engine and the Vial (VIA protocol 9 + Vial protocol 6)
 * command handler. Portable C: no register, no USB; the firmware (main.c) and the host tests (tests/) include it
 * after defining the platform hooks below, and feed it the panel (kb_input) and the raw HID requests (vial_cmd).
 *
 * The protocol follows vial-qmk quantum/via.c and quantum/vial.c (GPL-2.0-or-later), command by command; the reply
 * to a request is the request buffer, modified in place (unknown: byte 0 = 0xFF). */
#include "keycodes.h"
#include "controls.h"

/* ---- platform hooks (the includer defines them) ----
 *   kb_now()        milliseconds, free-running
 *   kb_out_ready()  the host takes keyboard reports (configured, awake)
 *   kb_out_room()   reports the queue can still take
 *   kb_out_push(r)  queue an 8-byte boot report; 0 = full
 *   kb_uboot()      enter the ROM update mode (does not return)
 *   kb_diag(out)    fm1_keyboard's status for a host tool (read-only): the platform's own counters
 *   kb_media_ok() / kb_media_room() / kb_media_push(r, n)   the mouse (ID 1), system (ID 2), consumer (ID 3) reports */
static uint32_t kb_now(void);
static int kb_out_ready(void);
static uint32_t kb_out_room(void);
static int kb_out_push(const uint8_t r[8]);
static void kb_uboot(void);
static void kb_diag(uint8_t *out);          /* the vendor status query: up to 26 bytes (see vial_cmd 0x02 0x80) */
static int kb_media_ok(void);               /* the mouse / system / consumer interface exists and the host takes it */
static uint32_t kb_media_room(void);
static int kb_media_push(const uint8_t *r, uint32_t n);   /* a report, its ID first; 0 = full */

#define VIA_PROTOCOL 0x0009u                /* via.h VIA_PROTOCOL_VERSION in vial-qmk; vial-gui accepts only 9 */
#define VIAL_PROTOCOL 6u                    /* vial.h VIAL_PROTOCOL_VERSION; keycodes "v6" */
static const uint8_t VIAL_UID[8] = {0x3D, 0x77, 0x09, 0x5E, 0x9E, 0x40, 0x53, 0x76};   /* this board design */
#include "vial_def.h"                       /* VIAL_DEF: firmware/vial.json, minified + xz (tools/vialdef.py) */

/* ================================================================ configuration
 * Laid out as vial-qmk's EEPROM so VIA's buffer offsets map directly: keymap [layer][row][col], knobs
 * [layer][knob][direction 0 CCW, 1 CW], big-endian 16-bit keycodes; the macro buffer (NUL-separated). The
 * fields up to `dirty` are what store.c keeps in flash (CFG_PERSIST_BYTES): a change there needs ST_VERSION bumped
 * (store.c reads the older layouts it knows field by field). cfg.dirty / changed_ms: the includer saves once nothing
 * has changed for a while. */
#define CFG_KEYMAP_BYTES (KB_LAYERS * KB_ROWS * KB_COLS * 2u)
#define CFG_ENC_BYTES (KB_LAYERS * KB_NKNOB * 2u * 2u)
enum { LIGHT_AUTO, LIGHT_OFF, LIGHT_DIM, LIGHT_MID, LIGHT_ON, LIGHT_BREATHE, LIGHT_BLINK, LIGHT_STYLES };
typedef struct {
    uint8_t keymap[CFG_KEYMAP_BYTES];
    uint8_t enc[CFG_ENC_BYTES];
    uint8_t macros[KB_MACRO_BYTES];
    uint8_t layer;                          /* the base layer (PRESETS), 0..KB_LAYERS - 1 */
    uint8_t reserved[3];
    struct {                                /* QMK settings (Vial: qsid in vial_sub 0x0A / 0x0B) */
        uint16_t tapping_term;              /* qsid 7: tap-hold, tap dance (unless its own) */
        uint16_t combo_term;                /* 2 */
        uint16_t osk_timeout;               /* 6: one-shot mods / layers, ms; 0 = no timeout */
        uint16_t tap_code_delay;            /* 18: between a tap's down and up (0: one report each) */
        uint16_t quick_tap_term;            /* 25: tapped, then held again within this: the tap repeats */
        uint8_t tapping_v2;                 /* 22 permissive hold bit 0, 23 hold on other key press 1, 24 retro 2 */
        uint8_t tapping_toggle;             /* 20: TT taps to toggle */
    } qs;
    uint8_t td[KB_TDS][10];                 /* Vial's vial_tap_dance_entry_t: on_tap, on_hold, on_double_tap,
                                             * on_tap_hold, tapping_term, LE u16 (as the protocol carries them) */
    uint8_t combo[KB_COMBOS][10];           /* vial_combo_entry_t: input[4], output, LE u16 */
    uint8_t light[KB_LAYERS][KB_NPOS];      /* each control's light style on each layer (LIGHT_*; the knobs: none) */
    uint8_t dirty;                          /* (not stored from here on) */
    uint32_t changed_ms;
} cfg_t;
static cfg_t cfg;
#define CFG_PERSIST_BYTES ((uint32_t)__builtin_offsetof(cfg_t, dirty))
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint8_t *cfg_persist(void) { return (uint8_t *)&cfg; }

static uint32_t km_ofs(uint32_t layer, uint32_t row, uint32_t col)
{
    return ((layer * KB_ROWS + row) * KB_COLS + col) * 2u;
}
static uint32_t enc_ofs(uint32_t layer, uint32_t knob, uint32_t cw)
{
    return ((layer * KB_NKNOB + knob) * 2u + cw) * 2u;
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t km_get(uint32_t layer, uint32_t row, uint32_t col)
{
    if (layer >= KB_LAYERS || row >= KB_ROWS || col >= KB_COLS)
        return KC_NO;
    return be16(&cfg.keymap[km_ofs(layer, row, col)]);
}
static uint16_t enc_get(uint32_t layer, uint32_t knob, uint32_t cw)
{
    if (layer >= KB_LAYERS || knob >= KB_NKNOB)
        return KC_NO;
    return be16(&cfg.enc[enc_ofs(layer, knob, cw != 0)]);
}
static uint32_t cfg_touches;                /* every change counts (vial_cmd: did this request change cfg?) */
static uint32_t cfg_vial_gen;               /* changes made by Vial requests (the editor reloads its view) */
static void cfg_touch(void)
{
    cfg.dirty = 1;
    cfg.changed_ms = kb_now();
    cfg_touches++;
}

static void cfg_keymap_defaults(void)
{
    uint32_t l, p, e, i;
    for (i = 0; i < CFG_KEYMAP_BYTES; i++)
        cfg.keymap[i] = 0;                  /* KC_NO where no control is */
    for (l = 0; l < KB_LAYERS; l++) {
        for (p = 0; p < KB_NPOS; p++) {
            uint8_t r, c;
            pos_rowcol(p, &r, &c);
            put_be16(&cfg.keymap[km_ofs(l, r, c)], default_key(l, p));
        }
        for (e = 0; e < KB_NKNOB; e++) {
            put_be16(&cfg.enc[enc_ofs(l, e, 0)], default_knob(l, e, 0));
            put_be16(&cfg.enc[enc_ofs(l, e, 1)], default_knob(l, e, 1));
        }
    }
}

static void cfg_qs_defaults(void)
{
    cfg.qs.tapping_term = 200;              /* QMK's defaults */
    cfg.qs.combo_term = 50;
    cfg.qs.osk_timeout = 0;
    cfg.qs.tap_code_delay = 0;
    cfg.qs.quick_tap_term = 200;
    cfg.qs.tapping_v2 = 0;
    cfg.qs.tapping_toggle = 5;
}

static void cfg_lights_defaults(void)
{
    uint32_t i;
    for (i = 0; i < sizeof cfg.light; i++)
        ((uint8_t *)cfg.light)[i] = LIGHT_AUTO;
}

static void cfg_v3_defaults(void)           /* what version 3 added: QMK settings, tap dances, combos */
{
    uint32_t i;
    cfg_qs_defaults();
    for (i = 0; i < sizeof cfg.td; i++)
        ((uint8_t *)cfg.td)[i] = 0;
    for (i = 0; i < sizeof cfg.combo; i++)
        ((uint8_t *)cfg.combo)[i] = 0;
}

static void cfg_defaults(void)
{
    uint32_t i;
    cfg_v3_defaults();
    cfg_keymap_defaults();
    cfg_lights_defaults();
    for (i = 0; i < KB_MACRO_BYTES; i++)
        cfg.macros[i] = 0;
    cfg.layer = 0;
    for (i = 0; i < sizeof cfg.reserved; i++)
        cfg.reserved[i] = 0;
    cfg.dirty = 0;
}

/* format 3's FM-1 keycodes (PRESET- PRESET+ SYSTEM NAV- NAV+ VAL- VAL+ KB OFF) -> today's; the rest unchanged */
static uint16_t kc_from_v3(uint16_t kc)
{
    if (kc < QK_KB || kc > QK_KB + 7u)
        return kc;
    if (kc <= QK_KB + 1u)
        return kc;                          /* PRESET-/+ = LAYER-/+ */
    return kc == QK_KB + 7u ? (uint16_t)FK_KB_OFF : (uint16_t)KC_NO;   /* SYSTEM, NAV, VAL: gone */
}

/* store.c read an older copy into cfg field by field (version 2: no settings, tap dances, combos; up to 3: 8 layers,
 * 7 encoders, other FM-1 keycodes, no lights). What it lacks gets its defaults; the keycodes are renumbered. */
static void cfg_migrate(uint32_t version)
{
    uint32_t i, j;
    if (version < 3u)
        cfg_v3_defaults();
    if (version < 4u) {
        cfg_lights_defaults();
        for (i = 0; i < CFG_KEYMAP_BYTES; i += 2u)
            put_be16(&cfg.keymap[i], kc_from_v3(be16(&cfg.keymap[i])));
        for (i = 0; i < CFG_ENC_BYTES; i += 2u)
            put_be16(&cfg.enc[i], kc_from_v3(be16(&cfg.enc[i])));
        for (i = 0; i < KB_TDS; i++)
            for (j = 0; j < 4u; j++) {      /* (field 4: the term, not a keycode) */
                uint16_t kc = kc_from_v3(le16(&cfg.td[i][2u * j]));
                cfg.td[i][2u * j] = (uint8_t)kc;
                cfg.td[i][2u * j + 1u] = (uint8_t)(kc >> 8);
            }
        for (i = 0; i < KB_COMBOS; i++)
            for (j = 0; j < 5u; j++) {
                uint16_t kc = kc_from_v3(le16(&cfg.combo[i][2u * j]));
                cfg.combo[i][2u * j] = (uint8_t)kc;
                cfg.combo[i][2u * j + 1u] = (uint8_t)(kc >> 8);
            }
    }
}

/* after store.c loaded a copy: keep what the engine relies on in range */
static void cfg_loaded(void)
{
    uint32_t i;
    if (cfg.qs.tapping_toggle == 0)
        cfg.qs.tapping_toggle = 1;
    if (cfg.layer >= KB_LAYERS)
        cfg.layer = 0;
    for (i = 0; i < sizeof cfg.light; i++)
        if (((uint8_t *)cfg.light)[i] >= LIGHT_STYLES)
            ((uint8_t *)cfg.light)[i] = LIGHT_AUTO;
    cfg.dirty = 0;
}

/* ================================================================ engine */
#define KQ_N 16u                            /* keys held at once we track (the report shows 6) */
#define AQ_N 64u                            /* actions waiting (down / up / tap), executed in order */
#define EVQ_N 32u                           /* events waiting in a pipeline stage */
#define POS_COMBO 0x80u                     /* an event's pos for a combo's output: POS_COMBO + combo index */
#define CHORD_MS 100u                       /* OCT- and OCT+ this close: the chord, not two keys */
#define CHORD_UBOOT_MS 10000u               /* the chord held this long: UBOOT (well past the 2 s Vial unlock) */
enum { CH_IDLE, CH_PEND, CH_FWD, CH_CHORD };
#define LS_N 8u                             /* temporary layers on at once */
#define POS_NONE 0xFFu                      /* an action from no control (a knob, a macro, a combo, a tap dance) */
enum { LK_HOLD = 1, LK_OSL, LK_TG };        /* how a temporary layer is on: held (MO, LM, LT, TT), one-shot, toggle */

static struct {
    /* temporary layers: on top of the base (cfg.layer), the most recent first. A control's keycode
     * comes from the first of them, then the base, that does not have KC_TRNS ("SAME") there. One toggle at most:
     * while it is on, the control that started it toggles back (whatever it is on the toggled layer), other toggles
     * are ignored, and a base change ends it. */
    uint8_t ls_layer[LS_N], ls_kind[LS_N], ls_n;
    uint8_t tg_layer, tg_pos;               /* the toggle (tg_layer POS_NONE: none), and its control */
    uint8_t cur_pos;                        /* the control of the action being executed, or POS_NONE */
    uint8_t down[KB_NPOS];                  /* the engine treats the control as held */
    uint16_t held[KB_NPOS];                 /* the keycode it resolved to when pressed */
    uint32_t lvl;                           /* buttons, bits 0..13, as last seen (unlock, key tester) */
    uint32_t notes;                         /* note keys, bits 0..26 */
    uint8_t mod_cnt[8];                     /* Left Ctrl .. Right GUI: holders */
    uint8_t kq[KQ_N], kq_cnt[KQ_N], kq_n;   /* keyboard-page keys held, in press order, with holders */
    uint8_t sent[8], synced;                /* the last report queued; the host has it */
    uint8_t off;                            /* FK_KB_OFF: nothing goes to the host */
    uint16_t aq_kc[AQ_N];                   /* the action queue: keycode, op (1 tap, 2 down, 3 up), control */
    uint8_t aq_op[AQ_N], aq_pos[AQ_N];
    uint32_t aq_r, aq_w, taps_dropped;
    uint8_t aq_phase;                       /* a tap with tap_code_delay: its down is out, the up waits */
    uint32_t aq_wait;
    uint8_t phys[KB_NPOS];                  /* the controls as the pipeline got them (edges -> events) */
    /* tap-hold (MT, LT, TT) per control: how it was decided, what it was */
    uint8_t th_mode[KB_NPOS];               /* 0 not tap-hold, 1 tapped, 2 held */
    uint16_t th_orig[KB_NPOS];
    int16_t last_tap_pos, retro_pos;        /* quick tap: the last tap-hold tapped; retro tap: held alone */
    uint32_t last_tap_t;
    uint8_t retro_ok;
    uint8_t tt_layer, tt_count;             /* TT: taps counted towards a toggle */
    uint32_t tt_last;
    /* one-shot mods / layer */
    uint8_t osm_mods, osm_holding, osm_used, osm_armed;
    uint32_t osm_t;
    uint8_t osl_layer, osl_holding, osl_used, osl_armed;
    uint32_t osl_t;
    /* tap dance: the one in progress (vial.c's state machine) */
    struct {
        uint8_t active, idx, pos, count, pressed, interrupted, finished;
        uint16_t down_kc;                   /* what on_dance_finished pressed: released at the reset */
        uint32_t t;                         /* the last press */
    } td;
    /* combos */
    uint32_t cmb_n, cmb_t0;                 /* the events at the head of the combo stage held back, since */
    uint8_t cmb_active[KB_COMBOS], cmb_released[KB_COMBOS];
    uint64_t cmb_mask[KB_COMBOS];           /* its controls still held */
    uint8_t ch, ch_pos;                     /* the OCT chord state; the OCT key pending (CH_PEND) */
    uint32_t ch_t0;
    uint8_t ch_prev;                        /* OCT- / OCT+ (bits 0, 1) as last seen (CH_FWD: new presses) */
    uint32_t editor_req;                    /* OCT+ taps (the editor: main.c) */
    int32_t sel_steps, alg_steps;           /* SELECT / ALGORITHM detents not taken yet (the UI: main.c) */
    uint16_t last_kc;                       /* the screen: the last key pressed (0xFFFF: none yet) */
    uint16_t consumer, system;              /* the consumer / system usage held (the last pressed wins, as QMK) */
    uint16_t m_consumer, m_system;          /* ... as the host has them */
    uint8_t mbtn_cnt[8], m_buttons;         /* mouse buttons: holders; as sent */
    uint8_t mv_keys, wh_keys;               /* mouse keys held: cursor up, down, left, right; wheel up, down, left, right */
    uint32_t mv_t0, mv_next, wh_next;
    int32_t p_dx, p_dy, p_wheel, p_pan;     /* movement not sent yet */
    uint8_t m_synced;
    int32_t mac_ofs;                        /* the macro playing: its next byte, -1 = none */
    uint32_t mac_wait;                      /* a delay step: not before this time */
    uint8_t mac_id;
    uint32_t macros_run;
    int16_t last_pos;                       /* its control: pos, 100 + 2 * knob + cw, 200 a combo */
    uint32_t rollovers, reports;
} kb;

/* Vial security, locked at power-on. Unlocking: the host starts it, then OCT- + OCT+ held UNLOCK_HOLD_MS without a
 * break (our clock, not vial.c's 50 polls >= 100 ms apart: 10 s at the app's 200 ms polling). The counter Vial reads
 * is the time left in tenths of a second (20 .. 0): the app's progress bar takes the largest it saw as its maximum,
 * so it fills in step. */
#define UNLOCK_HOLD_MS 2000u
static struct {
    uint8_t unlocked, in_progress;
    uint32_t hold_t0;                       /* when both keys went down (0: not both held) */
    uint32_t last_poll;
} vl;

/* time left, in tenths of a second (UNLOCK_HOLD_MS / 100 when not holding) */
static uint32_t unlock_counter(void)
{
    uint32_t held = vl.hold_t0 ? kb_now() - vl.hold_t0 : 0;
    return held >= UNLOCK_HOLD_MS ? 0 : (UNLOCK_HOLD_MS - held + 99u) / 100u;
}

/* the screen's bar: 0..1000 */
static uint32_t unlock_permille(void)
{
    uint32_t held = vl.hold_t0 ? kb_now() - vl.hold_t0 : 0;
    return held >= UNLOCK_HOLD_MS ? 1000u : held * 1000u / UNLOCK_HOLD_MS;
}

/* while an unlock is in progress: the keys held long enough -> unlocked (every pass, polls or not) */
static void unlock_track(uint32_t btn_lvl)
{
    uint32_t now = kb_now();
    if (!vl.in_progress)
        return;
    if ((btn_lvl & 3u) != 3u) {
        vl.hold_t0 = 0;
        return;
    }
    if (!vl.hold_t0)
        vl.hold_t0 = now ? now : 1u;        /* (0 means not held; never a time after now: the subtraction wraps) */
    if (now - vl.hold_t0 >= UNLOCK_HOLD_MS) {
        vl.in_progress = 0;
        vl.unlocked = 1;
        vl.hold_t0 = 0;
    }
}

static int kb_muted(void) { return kb.off || vl.in_progress; }

static uint32_t pos_level(uint32_t pos)
{
    return pos < KB_NBTN ? (kb.lvl >> pos) & 1u : (kb.notes >> (pos - KB_NBTN)) & 1u;
}

/* ---- layers ---- */
static void lay_on(uint32_t layer, uint32_t kind)
{
    if (layer >= KB_LAYERS || kb.ls_n >= LS_N)
        return;
    kb.ls_layer[kb.ls_n] = (uint8_t)layer;
    kb.ls_kind[kb.ls_n] = (uint8_t)kind;
    kb.ls_n++;
}
static void lay_off(uint32_t layer, uint32_t kind)   /* the most recent such entry */
{
    uint32_t i = kb.ls_n;
    while (i--)
        if (kb.ls_layer[i] == layer && kb.ls_kind[i] == kind) {
            for (; i + 1u < kb.ls_n; i++) {
                kb.ls_layer[i] = kb.ls_layer[i + 1u];
                kb.ls_kind[i] = kb.ls_kind[i + 1u];
            }
            kb.ls_n--;
            return;
        }
}
static void tg_end(void)
{
    if (kb.tg_layer == POS_NONE)
        return;
    lay_off(kb.tg_layer, LK_TG);
    kb.tg_layer = POS_NONE;
    kb.tg_pos = POS_NONE;
}
/* a toggle key (TG, TT's taps) from control pos */
static void tg_press(uint32_t layer, uint32_t pos)
{
    if (kb.tg_layer != POS_NONE) {          /* back only from its own control (from none: the same keycode) */
        if (pos == kb.tg_pos && (pos != POS_NONE || layer == kb.tg_layer))
            tg_end();
        return;                             /* another toggle while one is on: ignored */
    }
    if (layer >= KB_LAYERS)
        return;
    lay_on(layer, LK_TG);
    kb.tg_layer = (uint8_t)layer;
    kb.tg_pos = (uint8_t)pos;
}
/* the layer the keyboard is on: the most recent temporary one, else the base */
static uint32_t layer_top(void)
{
    return kb.ls_n ? kb.ls_layer[kb.ls_n - 1u] : cfg.layer;
}
static int layer_is_on(uint32_t layer)
{
    uint32_t i;
    for (i = 0; i < kb.ls_n; i++)
        if (kb.ls_layer[i] == layer)
            return 1;
    return 0;
}

static uint16_t resolve_key(uint32_t pos)
{
    uint8_t r, c;
    uint32_t i = kb.ls_n;
    uint16_t kc;
    if (kb.tg_layer != POS_NONE && pos == kb.tg_pos)
        return TG(kb.tg_layer);             /* the toggle's control: back, whatever is there now */
    pos_rowcol(pos, &r, &c);
    while (i--) {
        kc = km_get(kb.ls_layer[i], r, c);
        if (kc != KC_TRNS)
            return kc;
    }
    kc = km_get(cfg.layer, r, c);
    return kc == KC_TRNS ? (uint16_t)KC_NO : kc;
}

static uint16_t resolve_enc(uint32_t knob, uint32_t cw)
{
    uint32_t i = kb.ls_n;
    uint16_t kc;
    while (i--) {
        kc = enc_get(kb.ls_layer[i], knob, cw);
        if (kc != KC_TRNS)
            return kc;
    }
    kc = enc_get(cfg.layer, knob, cw);
    return kc == KC_TRNS ? (uint16_t)KC_NO : kc;
}

/* ---- the HID state ---- */
static void key_add(uint8_t k)
{
    uint32_t i;
    for (i = 0; i < kb.kq_n; i++)
        if (kb.kq[i] == k) {
            kb.kq_cnt[i]++;
            return;
        }
    if (kb.kq_n < KQ_N) {
        kb.kq[kb.kq_n] = k;
        kb.kq_cnt[kb.kq_n] = 1;
        kb.kq_n++;
    }
}
static void key_del(uint8_t k)
{
    uint32_t i;
    for (i = 0; i < kb.kq_n; i++)
        if (kb.kq[i] == k) {
            if (--kb.kq_cnt[i])
                return;
            for (; i + 1u < kb.kq_n; i++) {
                kb.kq[i] = kb.kq[i + 1u];
                kb.kq_cnt[i] = kb.kq_cnt[i + 1u];
            }
            kb.kq_n--;
            return;
        }
}
/* QK_MODS' 5 bits -> the 8 modifier holders */
static void mods5(uint32_t m, int down)
{
    uint32_t b, base = m & MOD_BIT_RIGHT ? 4u : 0u;
    for (b = 0; b < 4u; b++)
        if (m >> b & 1u) {
            if (down)
                kb.mod_cnt[base + b]++;
            else if (kb.mod_cnt[base + b])
                kb.mod_cnt[base + b]--;
        }
}

static void report_build(uint8_t r[8])
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        r[i] = 0;
    if (kb_muted())
        return;
    for (i = 0; i < 8u; i++)
        if (kb.mod_cnt[i])
            r[0] |= (uint8_t)(1u << i);
    if (kb.kq_n > 6u) {                     /* HID 1.11 appendix C: ErrorRollOver */
        for (i = 2; i < 8u; i++)
            r[i] = 0x01;
        return;
    }
    for (i = 0; i < kb.kq_n; i++)
        r[2u + i] = kb.kq[i];
}

/* queue the keyboard state if it changed; 0: the queue was full (the caller retries) */
static int kbd_emit(void)
{
    uint8_t r[8];
    uint32_t i, same = 1;
    if (!kb_out_ready()) {
        kb.synced = 0;                      /* sent in full once the host is back */
        return 1;
    }
    report_build(r);
    for (i = 0; i < 8u; i++)
        if (r[i] != kb.sent[i])
            same = 0;
    if (same && kb.synced)
        return 1;
    if (!kb_out_push(r))
        return 0;
    for (i = 0; i < 8u; i++)
        kb.sent[i] = r[i];
    kb.synced = 1;
    kb.reports++;
    if (r[2] == 0x01)
        kb.rollovers++;
    return 1;
}

/* ---- mouse, system and consumer (QMK's keycodes -> HID usages: vial-qmk tmk_core/protocol/report.h) ---- */
#define MV_STEP_MS 16u                      /* mouse movement: a report every 16 ms while a cursor key is held */
#define MV_MIN 2                            /* pixels a step at first, ... */
#define MV_MAX 16                           /* ... after a second */
#define WH_DELAY_MS 300u                    /* a held wheel key: one step at once, then after this, ... */
#define WH_REPEAT_MS 80u                    /* ... one every 80 ms (a tap, an encoder detent: exactly one) */
static const uint16_t CONSUMER_USAGE[KC_CONSUMER_LAST - KC_MUTE + 1u] = {
    0x0E2, 0x0E9, 0x0EA, 0x0B5, 0x0B6, 0x0B7, 0x0CD,   /* MUTE VOLU VOLD MNXT MPRV MSTP MPLY */
    0x183, 0x0CC, 0x18A, 0x192, 0x194,                 /* MSEL EJCT MAIL CALC MYCM */
    0x221, 0x223, 0x224, 0x225, 0x226, 0x227, 0x22A,   /* WWW search home back forward stop refresh favorites */
    0x0B3, 0x0B4, 0x06F, 0x070,                        /* MFFD MRWD BRIU BRID */
    0x19F, 0x1CB, 0x29F, 0x2A0,                        /* control panel, assistant, mission control, launchpad */
};
static int kc_is_media(uint16_t kc) { return kc >= KC_PWR && kc <= KC_CONSUMER_LAST; }
static int kc_is_mouse(uint16_t kc) { return kc >= KC_MS_U && kc <= KC_MOUSE_LAST; }

static void media_down(uint16_t kc)
{
    uint32_t now = kb_now(), b;
    if (kc <= KC_WAKE)
        kb.system = (uint16_t)(0x81u + (kc - KC_PWR));
    else if (kc <= KC_CONSUMER_LAST)
        kb.consumer = CONSUMER_USAGE[kc - KC_MUTE];
    else if (kc <= KC_MS_R) {
        if (!kb.mv_keys) {
            kb.mv_t0 = now;
            kb.mv_next = now;
        }
        kb.mv_keys |= (uint8_t)(1u << (kc - KC_MS_U));
    } else if (kc < KC_WH_U) {
        kb.mbtn_cnt[kc - KC_BTN1]++;
    } else if (kc <= KC_WH_R) {
        b = kc - KC_WH_U;                   /* up, down, left, right: one step now */
        if (b < 2u)
            kb.p_wheel += b ? -1 : 1;
        else
            kb.p_pan += b == 2u ? -1 : 1;
        kb.wh_keys |= (uint8_t)(1u << b);
        kb.wh_next = now + WH_DELAY_MS;
    }                                       /* (0xDD..0xDF, acceleration: not used) */
}

static void media_up(uint16_t kc)
{
    if (kc <= KC_WAKE) {
        if (kb.system == 0x81u + (kc - KC_PWR))
            kb.system = 0;
    } else if (kc <= KC_CONSUMER_LAST) {
        if (kb.consumer == CONSUMER_USAGE[kc - KC_MUTE])
            kb.consumer = 0;
    } else if (kc <= KC_MS_R) {
        kb.mv_keys &= (uint8_t)~(1u << (kc - KC_MS_U));
    } else if (kc < KC_WH_U) {
        if (kb.mbtn_cnt[kc - KC_BTN1])
            kb.mbtn_cnt[kc - KC_BTN1]--;
    } else if (kc <= KC_WH_R) {
        kb.wh_keys &= (uint8_t)~(1u << (kc - KC_WH_U));
    }
}

/* the held cursor and wheel keys, in time (kb_task) */
static void mouse_task(void)
{
    uint32_t now = kb_now();
    if (kb.mv_keys && (int32_t)(now - kb.mv_next) >= 0) {
        uint32_t held = now - kb.mv_t0;
        int32_t sp = held >= 1000u ? MV_MAX : MV_MIN + (int32_t)(held * (MV_MAX - MV_MIN) / 1000u);
        kb.p_dx += ((kb.mv_keys >> 3 & 1) - (kb.mv_keys >> 2 & 1)) * sp;
        kb.p_dy += ((kb.mv_keys >> 1 & 1) - (kb.mv_keys & 1)) * sp;
        kb.mv_next = now + MV_STEP_MS;
    }
    if (kb.wh_keys && (int32_t)(now - kb.wh_next) >= 0) {
        kb.p_wheel += (kb.wh_keys & 1) - (kb.wh_keys >> 1 & 1);
        kb.p_pan += (kb.wh_keys >> 3 & 1) - (kb.wh_keys >> 2 & 1);
        kb.wh_next = now + WH_REPEAT_MS;
    }
}

static int8_t clamp127(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }

/* queue what changed on the mouse / system / consumer side; 0: the queue was full */
static int media_emit(void)
{
    uint8_t r[6], btn = 0;
    uint16_t cons = kb.consumer, sys = kb.system;
    uint32_t i;
    if (!kb_media_ok()) {
        kb.m_synced = 0;
        kb.p_dx = kb.p_dy = kb.p_wheel = kb.p_pan = 0;
        return 1;
    }
    for (i = 0; i < 8u; i++)
        if (kb.mbtn_cnt[i])
            btn |= (uint8_t)(1u << i);
    if (kb_muted()) {
        btn = 0;
        cons = sys = 0;
        kb.p_dx = kb.p_dy = kb.p_wheel = kb.p_pan = 0;
    }
    if (!kb.m_synced || cons != kb.m_consumer) {
        r[0] = 3;
        r[1] = (uint8_t)cons;
        r[2] = (uint8_t)(cons >> 8);
        if (!kb_media_push(r, 3))
            return 0;
        kb.m_consumer = cons;
    }
    if (!kb.m_synced || sys != kb.m_system) {
        r[0] = 2;
        r[1] = (uint8_t)sys;
        r[2] = (uint8_t)(sys >> 8);
        if (!kb_media_push(r, 3))
            return 0;
        kb.m_system = sys;
    }
    while (!kb.m_synced || btn != kb.m_buttons || kb.p_dx || kb.p_dy || kb.p_wheel || kb.p_pan) {
        int8_t dx = clamp127(kb.p_dx), dy = clamp127(kb.p_dy), wh = clamp127(kb.p_wheel), pan = clamp127(kb.p_pan);
        r[0] = 1;
        r[1] = btn;
        r[2] = (uint8_t)dx;
        r[3] = (uint8_t)dy;
        r[4] = (uint8_t)wh;
        r[5] = (uint8_t)pan;
        if (!kb_media_push(r, 6))
            return 0;
        kb.m_buttons = btn;
        kb.p_dx -= dx;
        kb.p_dy -= dy;
        kb.p_wheel -= wh;
        kb.p_pan -= pan;
        kb.m_synced = 1;
    }
    kb.m_synced = 1;
    return 1;
}

static int emit(void)
{
    return kbd_emit() && media_emit();
}

/* the base layer (PRESETS, OCT- / OCT+, LAYER-/+, DF, TO): a change ends the toggle */
static void layer_set(uint32_t l)
{
    l %= KB_LAYERS;
    tg_end();
    if (l != cfg.layer) {
        cfg.layer = (uint8_t)l;
        cfg_touch();
    }
}

/* ---- keycodes: what a press and a release do ---- */
static int kc_is_report(uint16_t kc)        /* changes a report: keyboard, mouse, system, consumer */
{
    return (kc >= KC_A && kc <= KC_KB_LAST) || KC_IS_MOD(kc) || (kc >= QK_MODS && kc <= QK_MODS_MAX) ||
           kc_is_media(kc) || kc_is_mouse(kc);
}
static uint32_t out_room(void)              /* reports both queues can still take */
{
    uint32_t a = kb_out_room(), b = kb_media_ok() ? kb_media_room() : a;
    return a < b ? a : b;
}

static void macro_start(uint32_t id);

static void kc_down(uint16_t kc)
{
    uint32_t n = kc & QK_LAYER_MAX_OFS;
    if (kc >= KC_A && kc <= KC_KB_LAST)
        key_add((uint8_t)kc);
    else if (KC_IS_MOD(kc))
        kb.mod_cnt[kc - KC_LCTL]++;
    else if (kc_is_media(kc) || kc_is_mouse(kc))
        media_down(kc);
    else if (kc >= QK_MODS && kc <= QK_MODS_MAX) {
        mods5(kc >> 8 & 0x1Fu, 1);
        if ((kc & 0xFFu) >= KC_A)
            kc_down(kc & 0xFFu);
    } else if ((kc >= QK_TO && kc <= QK_TO + QK_LAYER_MAX_OFS) ||
               (kc >= QK_DEF_LAYER && kc <= QK_DEF_LAYER + QK_LAYER_MAX_OFS)) {
        if (n < KB_LAYERS)
            layer_set(n);                               /* GO TO (DF); TO the same: "switch to it and stay" */
    } else if (kc >= QK_MOMENTARY && kc <= QK_MOMENTARY + QK_LAYER_MAX_OFS) {
        lay_on(n, LK_HOLD);
    } else if (kc >= QK_TOGGLE_LAYER && kc <= QK_TOGGLE_LAYER + QK_LAYER_MAX_OFS) {
        tg_press(n, kb.cur_pos);
    } else if (kc >= QK_MOD_TAP && kc <= QK_LAYER_TAP_MAX) {
        if ((kc & 0xFFu) >= KC_A)
            kc_down(kc & 0xFFu);            /* MT / LT not through the pipeline (an encoder, a macro): the tap */
    } else if (kc >= QK_LAYER_MOD && kc <= QK_LAYER_MOD_MAX) {
        lay_on(kc >> 5 & 0xFu, LK_HOLD);
        mods5(kc & 0x1Fu, 1);
    } else if (kc >= QK_ONE_SHOT_MOD && kc <= QK_ONE_SHOT_MOD + 0x1Fu) {
        mods5(kc & 0x1Fu, 1);               /* held: a modifier; let go before another key: the next key's */
        kb.osm_mods |= (uint8_t)(kc & 0x1Fu);
        kb.osm_holding = 1;
        kb.osm_used = 0;
    } else if (kc >= QK_ONE_SHOT_LAYER && kc <= QK_ONE_SHOT_LAYER + QK_LAYER_MAX_OFS) {
        if (n < KB_LAYERS) {
            lay_on(n, LK_OSL);
            kb.osl_layer = (uint8_t)n;
            kb.osl_holding = 1;
            kb.osl_used = 0;
        }
    } else if (kc >= QK_MACRO && kc <= QK_MACRO_MAX) {
        macro_start(kc - QK_MACRO);
    } else if (kc == QK_BOOT) {
        if (vl.unlocked)                                /* Vial's firewall: never while locked */
            kb_uboot();
    } else if (kc == FK_LAYER_PREV) {
        layer_set(cfg.layer + KB_LAYERS - 1u);
    } else if (kc == FK_LAYER_NEXT) {
        layer_set(cfg.layer + 1u);
    } else if (kc == FK_KB_OFF) {
        kb.off ^= 1u;
    }
    /* anything else (media, mouse, tap-hold: later phases; unknown codes): nothing, safely */
}

static void kc_up(uint16_t kc)
{
    uint32_t n = kc & QK_LAYER_MAX_OFS;
    if (kc >= KC_A && kc <= KC_KB_LAST)
        key_del((uint8_t)kc);
    else if (KC_IS_MOD(kc)) {
        if (kb.mod_cnt[kc - KC_LCTL])
            kb.mod_cnt[kc - KC_LCTL]--;
    } else if (kc_is_media(kc) || kc_is_mouse(kc))
        media_up(kc); else if (kc >= QK_MODS && kc <= QK_MODS_MAX) {
        if ((kc & 0xFFu) >= KC_A)
            kc_up(kc & 0xFFu);
        mods5(kc >> 8 & 0x1Fu, 0);
    } else if (kc >= QK_MOMENTARY && kc <= QK_MOMENTARY + QK_LAYER_MAX_OFS) {
        lay_off(n, LK_HOLD);
    } else if (kc >= QK_MOD_TAP && kc <= QK_LAYER_TAP_MAX) {
        if ((kc & 0xFFu) >= KC_A)
            kc_up(kc & 0xFFu);
    } else if (kc >= QK_LAYER_MOD && kc <= QK_LAYER_MOD_MAX) {
        mods5(kc & 0x1Fu, 0);
        lay_off(kc >> 5 & 0xFu, LK_HOLD);
    } else if (kc >= QK_ONE_SHOT_MOD && kc <= QK_ONE_SHOT_MOD + 0x1Fu) {
        kb.osm_holding = 0;
        if (kb.osm_used) {                  /* used as a modifier while held: done */
            mods5(kc & 0x1Fu, 0);
            kb.osm_mods &= (uint8_t)~(kc & 0x1Fu);
        } else {
            kb.osm_armed = 1;               /* for the next key */
            kb.osm_t = kb_now();
        }
    } else if (kc >= QK_ONE_SHOT_LAYER && kc <= QK_ONE_SHOT_LAYER + QK_LAYER_MAX_OFS) {
        kb.osl_holding = 0;
        if (kb.osl_used) {
            lay_off(n, LK_OSL);
        } else {
            kb.osl_armed = 1;
            kb.osl_t = kb_now();
        }
    }
}

/* ================================================================ the pipeline
 * physical events (ev_in) -> the combo stage (holds a key back up to the combo term, to see whether its combo's
 * other keys follow) -> the tapping stage (resolves each key when it reaches the head, so a layer turned on by the
 * keys before counts for it; a tap-hold key at the head waits for its decision, the keys behind it wait too, as
 * QMK) -> actions (down / up / tap) in a queue, executed in order with their reports. */
typedef struct {
    uint8_t pos, down, res;                 /* res: kc resolved (at the stage that needs it) */
    uint16_t kc;
    uint32_t t;
} ev_t;
typedef struct {
    ev_t e[EVQ_N];
    uint32_t r, w;
} evq_t;
static evq_t cq, tq;                        /* combo stage, tapping stage */
static uint32_t evq_n(const evq_t *q) { return q->w - q->r; }
static ev_t *evq_at(evq_t *q, uint32_t i) { return &q->e[(q->r + i) % EVQ_N]; }
static int evq_push(evq_t *q, const ev_t *e)
{
    if (evq_n(q) >= EVQ_N)
        return 0;
    q->e[q->w % EVQ_N] = *e;
    q->w++;
    return 1;
}

static void ev_in(uint32_t pos, uint32_t down)
{
    ev_t e;
    e.pos = (uint8_t)pos;
    e.down = (uint8_t)down;
    e.res = 0;
    e.kc = 0;
    e.t = kb_now();
    if (pos < KB_NPOS)
        kb.phys[pos] = (uint8_t)down;
    if (!evq_push(&cq, &e))
        kb.taps_dropped++;                  /* (32 events behind a tap-hold decision: not expected) */
}

/* ---- the action queue ---- */
static void aq_push_at(uint32_t op, uint16_t kc, uint32_t pos)
{
    if (kb.aq_w - kb.aq_r >= AQ_N) {
        kb.taps_dropped++;
        return;
    }
    kb.aq_kc[kb.aq_w % AQ_N] = kc;
    kb.aq_op[kb.aq_w % AQ_N] = (uint8_t)op;
    kb.aq_pos[kb.aq_w % AQ_N] = (uint8_t)pos;
    kb.aq_w++;
}
static void aq_push(uint32_t op, uint16_t kc) { aq_push_at(op, kc, POS_NONE); }
static void tap_queue(uint16_t kc) { aq_push(1, kc); }

/* a key that ends a one-shot mod (a keyboard key that is not itself a modifier) */
static int kc_takes_oneshot(uint16_t kc)
{
    return (kc >= KC_A && kc <= KC_KB_LAST) || (kc >= QK_MODS && kc <= QK_MODS_MAX && (kc & 0xFFu) >= KC_A);
}

/* the actions waiting, in order; stops while the report queues are full (a tap needs up to 4 places) */
static void aq_run(void)
{
    while (kb.aq_r != kb.aq_w) {
        uint16_t kc = kb.aq_kc[kb.aq_r % AQ_N];
        uint32_t op = kb.aq_op[kb.aq_r % AQ_N];
        kb.cur_pos = kb.aq_pos[kb.aq_r % AQ_N];
        int report = kc_is_report(kc), live = report && !kb_muted() && kb_out_ready();
        if (op == 1u && report && !live) {  /* a tap for a host that is not there: dropped */
            kb.aq_r++;
            continue;
        }
        if (live) {
            if (!emit() || out_room() < (op == 1u ? 4u : 2u))
                return;                     /* busy: next pass */
        }
        if (op == 1u && kb.aq_phase) {      /* the second half of a delayed tap */
            if ((int32_t)(kb_now() - kb.aq_wait) < 0)
                return;
            kb.aq_phase = 0;
            kc_up(kc);
            emit();
            kb.aq_r++;
            continue;
        }
        if (op != 3u) {
            kc_down(kc);
            emit();
            if (kb.osm_armed && kc_takes_oneshot(kc)) {   /* the one-shot mods went out with this key */
                mods5(kb.osm_mods, 0);
                kb.osm_mods = 0;
                kb.osm_armed = 0;
                emit();
            }
        }
        if (op == 1u && cfg.qs.tap_code_delay && report) {
            kb.aq_phase = 1;
            kb.aq_wait = kb_now() + cfg.qs.tap_code_delay;
            return;
        }
        if (op != 2u) {
            kc_up(kc);
            emit();
        }
        kb.aq_r++;
    }
    kb.cur_pos = POS_NONE;
}
#define taps_run aq_run

/* a control's press / release as actions (the keycode kept, so a layer change while it is held releases the same) */
static void exec_press(uint32_t pos, uint16_t kc)
{
    kb.down[pos] = 1;
    kb.held[pos] = kc;
    aq_push_at(2, kc, pos);
}
static void exec_release(uint32_t pos)
{
    if (!kb.down[pos])
        return;
    kb.down[pos] = 0;
    aq_push(3, kb.held[pos]);
}

/* ---- macros: VIA's buffer (NUL-separated, KB_MACROS of them), played one step at a time from kb_task, so the
 * main loop never waits (QMK's player blocks). A step that changes the report waits for room in the queue; a
 * delay step for its time. Bytes (vial-qmk dynamic_keymap.c dynamic_keymap_macro_send):
 *   a printable character (or \n \t \b, ESC)      typed, with Shift where the US layout needs it
 *   01 01 kc / 01 02 kc / 01 03 kc                 tap / press / release a basic keycode
 *   01 04 a b                                      wait (a - 1) + (b - 1) * 255 ms
 *   01 05 lo hi / 01 06 lo hi / 01 07 lo hi        Vial: tap / press / release any 16-bit keycode (0xFFnn is nn << 8)
 * A macro that does not end with NUL in the buffer, or a buffer whose last byte is not NUL (a write cut short):
 * nothing. Another macro key while one plays: ignored. A write to the buffer stops it. ---- */
#define SS_PREFIX 0x01u
/* US layout: ASCII 0x20..0x7E -> keycode, bit 7 = with Shift */
static const uint8_t ASCII_KC[95] = {
    0x2C, 0x9E, 0xB4, 0xA0, 0xA1, 0xA2, 0xA4, 0x34, 0xA6, 0xA7, 0xA5, 0xAE, 0x36, 0x2D, 0x37, 0x38,   /*  !"#$%&'()*+,-./ */
    0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0xB3, 0x33, 0xB6, 0x2E, 0xB7, 0xB8,   /* 0..9 :;<=>? */
    0x9F, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92,   /* @ A..O */
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x2F, 0x31, 0x30, 0xA3, 0xAD,   /* P..Z [\]^_ */
    0x35, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,   /* ` a..o */
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0xAF, 0xB1, 0xB0, 0xB5,         /* p..z {|}~ */
};
static uint16_t ascii_kc(uint8_t c)         /* 0: not typable */
{
    uint8_t k;
    if (c == '\n')
        return KC_ENT;
    if (c == '\t')
        return KC_TAB;
    if (c == '\b')
        return KC_BSPC;
    if (c == 0x1Bu)
        return KC_ESC;
    if (c < 0x20u || c > 0x7Eu)
        return 0;
    k = ASCII_KC[c - 0x20u];
    return k & 0x80u ? LSFT(k & 0x7Fu) : k;
}

static void macro_stop(void) { kb.mac_ofs = -1; }

static void macro_start(uint32_t id)
{
    uint32_t ofs = 0;
    if (kb.mac_ofs >= 0 || id >= KB_MACROS || cfg.macros[KB_MACRO_BYTES - 1u] != 0)
        return;
    while (id) {                            /* past id NULs */
        if (ofs >= KB_MACRO_BYTES)
            return;
        if (cfg.macros[ofs++] == 0)
            id--;
    }
    if (ofs >= KB_MACRO_BYTES || cfg.macros[ofs] == 0)
        return;                             /* empty */
    kb.mac_ofs = (int32_t)ofs;
    kb.mac_wait = kb_now();
    kb.mac_id = (uint8_t)id;
    kb.macros_run++;
}

static uint8_t mac_byte(uint32_t i) { return i < KB_MACRO_BYTES ? cfg.macros[i] : 0; }

/* one action with its report(s); 0: no room yet (nothing done) */
static int mac_act(uint32_t what, uint16_t kc)   /* what: 1 tap, 2 down, 3 up */
{
    int report = kc_is_report(kc);
    if (report && out_room() < (what == 1u ? 4u : 2u))
        return 0;
    kb.cur_pos = POS_NONE;
    if (what != 3u) {
        kc_down(kc);
        if (report)
            emit();
    }
    if (what != 2u) {
        kc_up(kc);
        if (report)
            emit();
    }
    return 1;
}

static void macro_run(void)
{
    uint32_t steps = 0;
    while (kb.mac_ofs >= 0 && steps++ < 8u) {
        uint32_t o = (uint32_t)kb.mac_ofs, c = mac_byte(o);
        if (!kb_out_ready() || kb_muted()) {
            macro_stop();                   /* no host to type to */
            return;
        }
        if ((int32_t)(kb_now() - kb.mac_wait) < 0)
            return;                         /* a delay */
        if (!emit())
            return;                         /* the state before the macro's next step: out first */
        if (c == 0) {
            macro_stop();                   /* the end */
            return;
        }
        if (c != SS_PREFIX) {               /* a character */
            uint16_t kc = ascii_kc((uint8_t)c);
            if (kc && !mac_act(1, kc))
                return;
            kb.mac_ofs = (int32_t)(o + 1u);
            continue;
        }
        c = mac_byte(o + 1u);
        if (c >= 1u && c <= 3u) {           /* tap / down / up, a basic keycode */
            uint8_t k = mac_byte(o + 2u);
            if (!k) {
                macro_stop();
                return;
            }
            if (!mac_act(c, k))
                return;
            kb.mac_ofs = (int32_t)(o + 3u);
        } else if (c == 4u) {               /* delay */
            uint32_t a = mac_byte(o + 2u), b = mac_byte(o + 3u);
            if (!a || !b) {
                macro_stop();
                return;
            }
            kb.mac_wait = kb_now() + (a - 1u) + (b - 1u) * 255u;
            kb.mac_ofs = (int32_t)(o + 4u);
        } else if (c >= 5u && c <= 7u) {    /* Vial: a 16-bit keycode, little-endian, 0xFFnn = nn << 8 */
            uint32_t lo = mac_byte(o + 2u), hi = mac_byte(o + 3u);
            uint16_t kc;
            if (!lo || !hi) {
                macro_stop();
                return;
            }
            kc = (uint16_t)(lo | hi << 8);
            if (kc > 0xFF00u)
                kc = (uint16_t)((kc & 0xFFu) << 8);
            if (kc >= QK_MACRO && kc <= QK_MACRO_MAX) {
                kb.mac_ofs = (int32_t)(o + 4u);   /* a macro inside a macro: skipped */
                continue;
            }
            if (!mac_act(c - 4u, kc))
                return;
            kb.mac_ofs = (int32_t)(o + 4u);
        } else {
            macro_stop();                   /* 01 then 00 or unknown: stop, as QMK */
            return;
        }
    }
}

/* ---- tap dance (vial.c: on_dance / on_dance_finished / on_dance_reset, dance_step) ---- */
enum { TD_SINGLE_TAP = 1, TD_SINGLE_HOLD, TD_DOUBLE_TAP, TD_DOUBLE_HOLD, TD_DOUBLE_SINGLE_TAP, TD_MORE_TAPS };
static uint16_t td_get(uint32_t idx, uint32_t field) { return le16(&cfg.td[idx][field * 2u]); }   /* 0 tap, 1 hold,
                                                                                * 2 double tap, 3 tap hold, 4 term */
static uint32_t td_term(uint32_t idx)
{
    uint16_t t = td_get(idx, 4);
    return t ? t : cfg.qs.tapping_term;
}

static void td_down(uint16_t kc)
{
    aq_push(2, kc);
    kb.td.down_kc = kc;
}

static void td_reset(void)
{
    if (kb.td.down_kc)
        aq_push(3, kb.td.down_kc);
    kb.td.down_kc = 0;
    kb.td.active = 0;
    kb.td.count = 0;
    kb.td.finished = 0;
}

static void td_finish(uint32_t interrupted)
{
    uint32_t i = kb.td.idx, step;
    uint16_t tap = td_get(i, 0), hold = td_get(i, 1), dtap = td_get(i, 2), thold = td_get(i, 3);
    if (!kb.td.active || kb.td.finished)
        return;
    kb.td.interrupted = (uint8_t)interrupted;
    if (kb.td.count == 1u)
        step = interrupted || !kb.td.pressed ? TD_SINGLE_TAP : TD_SINGLE_HOLD;
    else if (kb.td.count == 2u)
        step = interrupted ? TD_DOUBLE_SINGLE_TAP : kb.td.pressed ? TD_DOUBLE_HOLD : TD_DOUBLE_TAP;
    else
        step = TD_MORE_TAPS;
    switch (step) {
    case TD_SINGLE_TAP:
        if (tap)
            td_down(tap);
        break;
    case TD_SINGLE_HOLD:
        if (hold)
            td_down(hold);
        else if (tap)
            td_down(tap);
        break;
    case TD_DOUBLE_TAP:
        if (dtap)
            td_down(dtap);
        else if (tap) {
            aq_push(1, tap);
            td_down(tap);
        }
        break;
    case TD_DOUBLE_HOLD:
        if (thold)
            td_down(thold);
        else if (tap) {
            aq_push(1, tap);
            td_down(hold ? hold : tap);
        } else if (hold)
            td_down(hold);
        break;
    case TD_DOUBLE_SINGLE_TAP:
        if (tap) {
            aq_push(1, tap);
            td_down(tap);
        }
        break;
    }
    kb.td.finished = 1;
    if (!kb.td.pressed)
        td_reset();
}

static void td_press(uint32_t pos, uint16_t kc, uint32_t t)
{
    uint32_t idx = kc - QK_TAP_DANCE;
    uint16_t tap;
    kb.down[pos] = 1;
    kb.held[pos] = KC_NO;                   /* (the dance does the keys) */
    if (idx >= KB_TDS)
        return;
    if (kb.td.active && (kb.td.idx != idx || kb.td.finished)) {
        td_finish(1);
        if (kb.td.active)
            td_reset();
    }
    if (!kb.td.active) {
        kb.td.active = 1;
        kb.td.idx = (uint8_t)idx;
        kb.td.count = 0;
        kb.td.finished = 0;
        kb.td.down_kc = 0;
    }
    kb.td.count++;
    kb.td.pressed = 1;
    kb.td.pos = (uint8_t)pos;
    kb.td.t = t;
    tap = td_get(idx, 0);
    if (tap && kb.td.count == 3u) {         /* on_dance: three taps typed at the third, one more at each after */
        aq_push(1, tap);
        aq_push(1, tap);
        aq_push(1, tap);
    } else if (tap && kb.td.count > 3u)
        aq_push(1, tap);
}

static void td_release(void)
{
    uint32_t i = kb.td.idx;
    uint16_t tap = td_get(i, 0), hold = td_get(i, 1), dtap = td_get(i, 2), thold = td_get(i, 3);
    kb.td.pressed = 0;
    kb.down[kb.td.pos] = 0;
    if (kb.td.finished) {
        td_reset();
        return;
    }
    /* vial.c process_record_vial: decided at this release when nothing later could change it */
    if ((kb.td.count == 1u && tap && hold && !dtap && !thold) || (kb.td.count == 2u && dtap))
        td_finish(0);
}

static void td_task(uint32_t now)
{
    if (kb.td.active && !kb.td.finished && now - kb.td.t >= td_term(kb.td.idx))
        td_finish(0);
}

/* ---- tap-hold: MT, LT, TT ---- */
enum { TH_NONE, TH_MT, TH_LT, TH_TT };
enum { TH_WAIT, TH_TAP, TH_HOLD };
static uint32_t th_kind(uint16_t kc)
{
    if (kc >= QK_MOD_TAP && kc <= QK_MOD_TAP_MAX)
        return (kc >> 8 & 0x1Fu) ? TH_MT : TH_NONE;
    if (kc >= QK_LAYER_TAP && kc <= QK_LAYER_TAP_MAX)
        return TH_LT;
    if (kc >= QK_LAYER_TAP_TOGGLE && kc <= QK_LAYER_TAP_TOGGLE + QK_LAYER_MAX_OFS)
        return TH_TT;
    return TH_NONE;
}

/* the decision for the tap-hold key at the head of the tapping stage, from the events behind it and the time */
static uint32_t th_decide(const ev_t *e, uint32_t now, uint32_t *alone)
{
    uint32_t i, n = evq_n(&tq), term = cfg.qs.tapping_term;
    uint64_t seen = 0;
    *alone = 1;
    if (kb.last_tap_pos == (int16_t)e->pos && e->t - kb.last_tap_t < cfg.qs.quick_tap_term)
        return TH_TAP;                      /* tapped, pressed again soon: the tap, held (repeats) */
    for (i = 1; i < n; i++) {
        const ev_t *f = evq_at(&tq, i);
        if (f->pos >= POS_COMBO)
            continue;
        if (f->pos == e->pos && !f->down)
            return f->t - e->t < term ? TH_TAP : TH_HOLD;
        *alone = 0;
        if (f->down) {
            if (cfg.qs.tapping_v2 & 2u)     /* hold on other key press */
                return TH_HOLD;
            seen |= (uint64_t)1 << f->pos;
        } else if ((seen >> f->pos & 1u) && (cfg.qs.tapping_v2 & 1u))
            return TH_HOLD;                 /* permissive hold: another key pressed and released within */
    }
    return now - e->t >= term ? TH_HOLD : TH_WAIT;
}

static void th_act(uint32_t pos, uint16_t kc, uint32_t how, uint32_t alone, uint32_t t)
{
    uint32_t kind = th_kind(kc), layer = kind == TH_TT ? (kc & QK_LAYER_MAX_OFS) : (kc >> 8 & 0xFu);
    kb.th_orig[pos] = kc;
    kb.th_mode[pos] = (uint8_t)how;
    if (how == TH_TAP) {
        if (kind == TH_TT) {                /* TT: taps counted; tapping_toggle of them in a row toggle */
            if (kb.tt_layer != layer || t - kb.tt_last >= cfg.qs.tapping_term)
                kb.tt_count = 0;
            kb.tt_layer = (uint8_t)layer;
            kb.tt_last = t;
            if (++kb.tt_count >= cfg.qs.tapping_toggle) {
                aq_push_at(1, TG(layer), pos);
                kb.tt_count = 0;
            }
            kb.down[pos] = 1;
            kb.held[pos] = KC_NO;
        } else
            exec_press(pos, kc & 0xFFu);
        return;
    }
    if (kind == TH_MT)
        exec_press(pos, (uint16_t)((kc >> 8 & 0x1Fu) << 8));   /* its mods, as a QK_MODS code without a key */
    else
        exec_press(pos, MO(layer));
    kb.retro_pos = alone && (cfg.qs.tapping_v2 & 4u) && kind != TH_TT ? (int16_t)pos : -1;
    kb.retro_ok = 1;
}

static void release_pos(uint32_t pos, uint32_t t)
{
    if (kb.td.active && kb.td.pos == pos && kb.td.pressed) {
        td_release();
        return;
    }
    if (kb.th_mode[pos] == TH_TAP) {
        kb.last_tap_pos = (int16_t)pos;
        kb.last_tap_t = t;
    }
    exec_release(pos);
    if (kb.th_mode[pos] == TH_HOLD && kb.retro_pos == (int16_t)pos && kb.retro_ok)
        aq_push(1, kb.th_orig[pos] & 0xFFu);   /* retro tapping: held alone, let go: the tap after all */
    if (kb.retro_pos == (int16_t)pos)
        kb.retro_pos = -1;
    kb.th_mode[pos] = 0;
}

/* the tapping stage: everything decided in order; stops at an undecided tap-hold key, or while actions wait */
static void tapping_stage(uint32_t now)
{
    while (evq_n(&tq)) {
        ev_t *e = evq_at(&tq, 0);
        uint32_t how, alone, kind;
        aq_run();
        if (kb.aq_r != kb.aq_w)
            return;                         /* earlier actions first: a layer they turn on counts for this key */
        if (e->pos >= POS_COMBO) {          /* a combo's output */
            if (e->down) {
                kb.last_kc = e->kc;
                kb.last_pos = 200;
            }
            aq_push(e->down ? 2u : 3u, e->kc);
            tq.r++;
            continue;
        }
        if (!e->down) {
            release_pos(e->pos, e->t);
            tq.r++;
            continue;
        }
        if (!e->res) {
            e->kc = resolve_key(e->pos);
            e->res = 1;
            kb.last_kc = e->kc;
            kb.last_pos = (int16_t)e->pos;
            if (kb.osl_armed && !(e->kc >= QK_ONE_SHOT_LAYER && e->kc <= QK_ONE_SHOT_LAYER + QK_LAYER_MAX_OFS)) {
                lay_off(kb.osl_layer, LK_OSL);  /* the one-shot layer served this key */
                kb.osl_armed = 0;
            }
            if (kb.osm_holding)
                kb.osm_used = 1;
            if (kb.osl_holding)
                kb.osl_used = 1;
            if (kb.retro_pos >= 0)
                kb.retro_ok = 0;            /* another key while a tap-hold was held: no retro tap */
        }
        if (kb.td.active && !kb.td.finished &&
            !(e->kc >= QK_TAP_DANCE && e->kc <= QK_TAP_DANCE_MAX && e->kc - QK_TAP_DANCE == kb.td.idx)) {
            td_finish(1);                   /* another key interrupts the dance */
            continue;                       /* (its actions first) */
        }
        if (e->kc >= QK_TAP_DANCE && e->kc <= QK_TAP_DANCE_MAX) {
            td_press(e->pos, e->kc, e->t);
            tq.r++;
            continue;
        }
        kind = th_kind(e->kc);
        if (kind == TH_NONE) {
            exec_press(e->pos, e->kc);
            tq.r++;
            continue;
        }
        how = th_decide(e, now, &alone);
        if (how == TH_WAIT)
            return;
        th_act(e->pos, e->kc, how, alone, e->t);
        tq.r++;
    }
    aq_run();
}

/* ---- combos ---- */
static uint16_t cmb_in(uint32_t c, uint32_t i) { return le16(&cfg.combo[c][2u * i]); }
static uint16_t cmb_out(uint32_t c) { return le16(&cfg.combo[c][8]); }
static uint32_t cmb_size(uint32_t c)        /* its keys; < 2 (or no output): not a combo */
{
    uint32_t i = 0;
    if (!cmb_out(c))
        return 0;
    while (i < 4u && cmb_in(c, i))
        i++;
    return i >= 2u ? i : 0;
}
static int cmb_has(uint32_t c, uint16_t kc)
{
    uint32_t i, n = cmb_size(c);
    for (i = 0; i < n; i++)
        if (cmb_in(c, i) == kc)
            return 1;
    return 0;
}
/* the held-back keys (and kc, if not 0) all in combo c, without repeats */
static int cmb_covers(uint32_t c, uint16_t kc)
{
    uint32_t i, j;
    if (!cmb_size(c) || (kc && !cmb_has(c, kc)))
        return 0;
    for (i = 0; i < kb.cmb_n; i++) {
        uint16_t k = evq_at(&cq, i)->kc;
        if (!cmb_has(c, k) || k == kc)
            return 0;
        for (j = 0; j < i; j++)
            if (evq_at(&cq, j)->kc == k)
                return 0;
    }
    return 1;
}
static int32_t cmb_exact(void)              /* a combo whose keys are exactly the held-back ones, or -1 */
{
    uint32_t c;
    for (c = 0; c < KB_COMBOS; c++)
        if (cmb_size(c) == kb.cmb_n && cmb_covers(c, 0))
            return (int32_t)c;
    return -1;
}
static int cmb_bigger(uint32_t c)           /* another combo with all of c's keys and more: wait for it */
{
    uint32_t d, i, n = cmb_size(c);
    for (d = 0; d < KB_COMBOS; d++) {
        int all = cmb_size(d) > n;
        for (i = 0; all && i < n; i++)
            all = cmb_has(d, cmb_in(c, i));
        if (all)
            return 1;
    }
    return 0;
}
static int combos_any(void)
{
    uint32_t c;
    for (c = 0; c < KB_COMBOS; c++)
        if (cmb_size(c))
            return 1;
    return 0;
}

static void cmb_forward(uint32_t n)         /* the first n events to the tapping stage (resolved again there) */
{
    while (n--) {
        ev_t *e = evq_at(&cq, 0);
        e->res = 0;
        if (!evq_push(&tq, e))
            kb.taps_dropped++;
        cq.r++;
    }
}

static void cmb_fire(uint32_t c)
{
    ev_t o;
    uint32_t i;
    kb.cmb_mask[c] = 0;
    for (i = 0; i < kb.cmb_n; i++)
        kb.cmb_mask[c] |= (uint64_t)1 << evq_at(&cq, i)->pos;
    o.pos = (uint8_t)(POS_COMBO + c);
    o.down = 1;
    o.res = 1;
    o.kc = cmb_out(c);
    o.t = evq_at(&cq, kb.cmb_n - 1u)->t;
    cq.r += kb.cmb_n;
    kb.cmb_n = 0;
    kb.cmb_active[c] = 1;
    kb.cmb_released[c] = 0;
    if (!evq_push(&tq, &o))
        kb.taps_dropped++;
}

/* the combo stage: a key of some combo is held back while its combo can still complete (within combo_term) */
static void combo_stage(uint32_t now)
{
    if (!combos_any()) {
        cmb_forward(evq_n(&cq));
        kb.cmb_n = 0;
        return;
    }
    while (evq_n(&cq)) {
        if (kb.cmb_n < evq_n(&cq)) {
            ev_t *e = evq_at(&cq, kb.cmb_n);
            uint32_t c, ok = 0;
            if (e->down) {
                if (!e->res) {
                    e->kc = resolve_key(e->pos);
                    e->res = 1;
                }
                if (!kb.cmb_n || e->t - kb.cmb_t0 < cfg.qs.combo_term)
                    for (c = 0; c < KB_COMBOS && !ok; c++)
                        ok = cmb_covers(c, e->kc);
                if (ok) {
                    int32_t x;
                    if (!kb.cmb_n)
                        kb.cmb_t0 = e->t;
                    kb.cmb_n++;
                    x = cmb_exact();
                    if (x >= 0 && !cmb_bigger((uint32_t)x))
                        cmb_fire((uint32_t)x);
                    continue;
                }
            }
            if (kb.cmb_n) {                 /* not part of it: what was held back goes on as plain keys */
                cmb_forward(kb.cmb_n);
                kb.cmb_n = 0;
                continue;
            }
            if (!e->down) {                 /* a combo's key let go: its output goes up at the first */
                for (c = 0; c < KB_COMBOS; c++)
                    if (kb.cmb_active[c] && (kb.cmb_mask[c] >> e->pos & 1u)) {
                        ev_t o;
                        kb.cmb_mask[c] &= ~((uint64_t)1 << e->pos);
                        if (!kb.cmb_released[c]) {
                            o.pos = (uint8_t)(POS_COMBO + c);
                            o.down = 0;
                            o.res = 1;
                            o.kc = cmb_out(c);
                            o.t = e->t;
                            evq_push(&tq, &o);
                            kb.cmb_released[c] = 1;
                        }
                        if (!kb.cmb_mask[c])
                            kb.cmb_active[c] = 0;
                        break;
                    }
                if (c < KB_COMBOS) {
                    cq.r++;                 /* swallowed */
                    continue;
                }
            }
            cmb_forward(1);
        } else {                            /* all held back: complete in time, or let them go */
            if (now - kb.cmb_t0 >= cfg.qs.combo_term) {
                int32_t x = cmb_exact();
                if (x >= 0)
                    cmb_fire((uint32_t)x);
                else {
                    cmb_forward(kb.cmb_n);
                    kb.cmb_n = 0;
                }
                continue;
            }
            break;
        }
    }
}

static void pipeline_run(uint32_t now)
{
    combo_stage(now);
    tapping_stage(now);
    td_task(now);
    if (kb.osm_armed && cfg.qs.osk_timeout && now - kb.osm_t >= cfg.qs.osk_timeout) {
        mods5(kb.osm_mods, 0);              /* one-shot mods unused for too long */
        kb.osm_mods = 0;
        kb.osm_armed = 0;
    }
    if (kb.osl_armed && cfg.qs.osk_timeout && now - kb.osl_t >= cfg.qs.osk_timeout) {
        lay_off(kb.osl_layer, LK_OSL);
        kb.osl_armed = 0;
    }
    aq_run();
}

/* OCT- / OCT+ (pos 0, 1): fixed in firmware, never through the keymap. OCT+ alone (at the release, or
 * once held CHORD_MS): the editor (kb.editor_req; main.c); OCT- alone: nothing (free). Pressed together within
 * CHORD_MS: the chord, held CHORD_UBOOT_MS: UBOOT. Off while a Vial unlock is in progress (the unlock holds the same
 * two keys). */
static void oct_act(uint32_t pos)
{
    if (pos == POS_OCTUP)
        kb.editor_req++;
}

static void oct_chord(uint32_t now, uint32_t a, uint32_t b)
{
    uint32_t lvl = a | b << 1, rise = lvl & ~kb.ch_prev;
    kb.ch_prev = (uint8_t)lvl;
    switch (kb.ch) {
    case CH_IDLE:
        if (a || b) {
            kb.ch = CH_PEND;
            kb.ch_pos = a ? POS_OCTDN : POS_OCTUP;
            kb.ch_t0 = now;
            if (a && b)
                kb.ch = CH_CHORD;           /* both in the same pass */
        }
        break;
    case CH_PEND:
        if (a && b) {
            kb.ch = CH_CHORD;
            kb.ch_t0 = now;
        } else if (!(kb.ch_pos == POS_OCTDN ? a : b)) {           /* let go within the window: a tap */
            oct_act(kb.ch_pos);
            if (rise & (kb.ch_pos == POS_OCTDN ? 2u : 1u))       /* (the other pressed as this one went up) */
                oct_act(kb.ch_pos == POS_OCTDN ? POS_OCTUP : POS_OCTDN);
            kb.ch = a || b ? CH_FWD : CH_IDLE;
        } else if (now - kb.ch_t0 >= CHORD_MS) {
            oct_act(kb.ch_pos);
            kb.ch = CH_FWD;
        }
        break;
    case CH_FWD:                            /* a key is held (acted on): a new press of either acts at once */
        if (rise & 1u)
            oct_act(POS_OCTDN);
        if (rise & 2u)
            oct_act(POS_OCTUP);
        if (!a && !b)
            kb.ch = CH_IDLE;
        break;
    case CH_CHORD:
        if (a && b && now - kb.ch_t0 >= CHORD_UBOOT_MS)
            kb_uboot();
        if (!a && !b)
            kb.ch = CH_IDLE;
        break;
    }
}

/* the panel, once a main-loop pass: levels (bit per control), presses since the last pass (a tap shorter than a
 * pass counts), encoder detents (+ = clockwise) by felucca-bsp id */
static void kb_input(uint32_t btn_lvl, uint32_t note_lvl, uint32_t btn_pressed, uint32_t note_pressed,
                     const int32_t enc[KB_NENC])
{
    uint32_t now = kb_now(), p, e;
    uint32_t btn = btn_lvl | btn_pressed, notes = note_lvl | note_pressed;
    kb.lvl = btn_lvl;
    kb.notes = note_lvl;
    unlock_track(btn_lvl);
    if (vl.in_progress) {                   /* Vial unlock: the keys only count for it; nothing happens */
        kb.ch = CH_IDLE;
        kb.ch_prev = (uint8_t)(btn & 3u);
    } else {
        oct_chord(now, btn & 1u, btn >> 1 & 1u);
    }
    for (p = 2; p < KB_NPOS; p++) {
        uint32_t on = p < KB_NBTN ? btn >> p & 1u : notes >> (p - KB_NBTN) & 1u;
        if (on != kb.phys[p])
            ev_in(p, on);
    }
    pipeline_run(now);
    for (e = 0; e < KB_NENC; e++) {
        int32_t s = enc[e];
        if (!s)
            continue;
        if (e == ENC_PRESETS) {             /* fixed: the base layer */
            for (; s > 0; s--)
                layer_set(cfg.layer + 1u);
            for (; s < 0; s++)
                layer_set(cfg.layer + KB_LAYERS - 1u);
            continue;
        }
        if (e == ENC_SELECT || e == ENC_ALGORITHM) {   /* fixed: the UI's (main.c takes them) */
            if (e == ENC_SELECT)
                kb.sel_steps += s;
            else
                kb.alg_steps += s;
            continue;
        }
        {
            uint32_t k = e - ENC_KNOB1;
            for (; s > 0; s--)
                tap_queue(resolve_enc(k, 1));
            for (; s < 0; s++)
                tap_queue(resolve_enc(k, 0));
            kb.last_kc = resolve_enc(k, enc[e] > 0);
            kb.last_pos = (int16_t)(100 + 2u * k + (enc[e] > 0));   /* knob k, direction */
        }
    }
    aq_run();
    emit();
    macro_run();
}

/* the regular work, every main-loop pass (also without input) */
static void kb_task(void)
{
    uint32_t now = kb_now();
    unlock_track(kb.lvl);
    mouse_task();
    if (vl.in_progress && now - vl.last_poll > 3000u) {
        vl.in_progress = 0;                 /* the host went away during an unlock */
        vl.hold_t0 = 0;
    }
    pipeline_run(now);
    emit();
    macro_run();
}

/* the editor takes the panel (main.c): everything let go, nothing pending, the temporary layers off; the host gets
 * an empty report */
static void kb_release_all(void)
{
    uint32_t i;
    cq.r = cq.w = tq.r = tq.w = 0;
    kb.aq_r = kb.aq_w = 0;
    kb.aq_phase = 0;
    kb.cmb_n = 0;
    for (i = 0; i < KB_COMBOS; i++)
        kb.cmb_active[i] = 0;
    for (i = 0; i < KB_NPOS; i++) {
        kb.down[i] = 0;
        kb.th_mode[i] = 0;
    }
    for (i = 0; i < 8u; i++) {
        kb.mod_cnt[i] = 0;
        kb.mbtn_cnt[i] = 0;
    }
    kb.kq_n = 0;
    kb.consumer = kb.system = 0;
    kb.mv_keys = kb.wh_keys = 0;
    kb.osm_mods = kb.osm_armed = kb.osm_holding = 0;
    kb.osl_armed = kb.osl_holding = 0;
    kb.td.active = 0;
    kb.td.down_kc = 0;
    kb.retro_pos = -1;
    kb.ls_n = 0;
    kb.tg_layer = kb.tg_pos = POS_NONE;
    macro_stop();
    emit();
}

/* the editor gives the panel back: what is held now counts as already pressed (its release does nothing) */
static void kb_resync(uint32_t btn_lvl, uint32_t note_lvl)
{
    uint32_t p;
    for (p = 0; p < KB_NPOS; p++)
        kb.phys[p] = (uint8_t)(p < KB_NBTN ? btn_lvl >> p & 1u : note_lvl >> (p - KB_NBTN) & 1u);
    kb.ch = (btn_lvl & 3u) ? CH_FWD : CH_IDLE;   /* (OCT keys still down: no layer change when they go up) */
    kb.ch_prev = (uint8_t)(btn_lvl & 3u);
    kb.lvl = btn_lvl;
    kb.notes = note_lvl;
    kb.sel_steps = kb.alg_steps = 0;
}

static void kb_init(void)
{
    uint32_t i;
    cfg_defaults();
    for (i = 0; i < sizeof kb; i++)
        ((uint8_t *)&kb)[i] = 0;
    cq.r = cq.w = tq.r = tq.w = 0;
    kb.last_kc = 0xFFFFu;
    kb.last_pos = -1;
    kb.mac_ofs = -1;
    kb.last_tap_pos = -1;
    kb.retro_pos = -1;
    kb.tg_layer = POS_NONE;
    kb.tg_pos = POS_NONE;
    kb.cur_pos = POS_NONE;
    vl.unlocked = 0;
    vl.in_progress = 0;
}

/* ================================================================ Vial: the raw HID requests
 * One 32-byte request in m, answered in place. Returns 1 when the device must enter UBOOT once the reply has gone
 * (bootloader jump). */
#define VIA_N 32u
static void matrix_rows(uint8_t *out)       /* id_switch_matrix_state: 2 bytes a row (16 columns), MSB first */
{
    uint32_t r, p;
    for (r = 0; r < KB_ROWS; r++) {
        uint32_t v = 0;
        for (p = 0; p < KB_NPOS; p++) {
            uint8_t pr, pc;
            pos_rowcol(p, &pr, &pc);
            if (pr == r && pos_level(p))
                v |= 1u << pc;
        }
        out[2u * r] = (uint8_t)(v >> 8);
        out[2u * r + 1u] = (uint8_t)v;
    }
}

/* ---- QMK settings (vial-qmk qmk_settings.c: qsid, size; bits of tapping_v2 read and written as 0 / 1) ---- */
static const uint16_t QS_IDS[] = {2, 6, 7, 18, 20, 22, 23, 24, 25};
static uint32_t qs_get(uint32_t id, uint32_t *v)   /* the size in bytes, 0: not ours */
{
    switch (id) {
    case 2: *v = cfg.qs.combo_term; return 2;
    case 6: *v = cfg.qs.osk_timeout; return 2;
    case 7: *v = cfg.qs.tapping_term; return 2;
    case 18: *v = cfg.qs.tap_code_delay; return 2;
    case 20: *v = cfg.qs.tapping_toggle; return 1;
    case 22: case 23: case 24: *v = cfg.qs.tapping_v2 >> (id - 22u) & 1u; return 1;
    case 25: *v = cfg.qs.quick_tap_term; return 2;
    }
    return 0;
}
static int qs_set(uint32_t id, uint32_t v)
{
    switch (id) {
    case 2: cfg.qs.combo_term = (uint16_t)v; break;
    case 6: cfg.qs.osk_timeout = (uint16_t)v; break;
    case 7: cfg.qs.tapping_term = (uint16_t)(v ? v : 1u); break;
    case 18: cfg.qs.tap_code_delay = (uint16_t)(v > 1000u ? 1000u : v); break;
    case 20: cfg.qs.tapping_toggle = (uint8_t)(v ? v : 1u); break;
    case 22: case 23: case 24:
        if (v & 0xFFu)
            cfg.qs.tapping_v2 |= (uint8_t)(1u << (id - 22u));
        else
            cfg.qs.tapping_v2 &= (uint8_t)~(1u << (id - 22u));
        break;
    case 25: cfg.qs.quick_tap_term = (uint16_t)v; break;
    default: return 0;
    }
    cfg_touch();
    return 1;
}

static void combos_reset(void)
{
    uint32_t c;
    for (c = 0; c < KB_COMBOS; c++)
        kb.cmb_active[c] = 0;
    cmb_forward(kb.cmb_n);
    kb.cmb_n = 0;
}

static int vial_sub(uint8_t *m)
{
    uint32_t i;
    switch (m[1]) {
    case 0x00: {                            /* get keyboard id: Vial protocol (u32 LE), UID */
        for (i = 0; i < VIA_N; i++)
            m[i] = 0;
        m[0] = VIAL_PROTOCOL & 0xFFu;
        for (i = 0; i < 8u; i++)
            m[4u + i] = VIAL_UID[i];
        break;
    }
    case 0x01: {                            /* definition size, u32 LE */
        uint32_t sz = sizeof VIAL_DEF;
        m[0] = (uint8_t)sz;
        m[1] = (uint8_t)(sz >> 8);
        m[2] = (uint8_t)(sz >> 16);
        m[3] = (uint8_t)(sz >> 24);
        break;
    }
    case 0x02: {                            /* definition page (32 bytes) */
        uint32_t start = (m[2] | (uint32_t)m[3] << 8) * VIA_N, end = start + VIA_N;
        if (start >= sizeof VIAL_DEF)
            break;
        if (end > sizeof VIAL_DEF)
            end = sizeof VIAL_DEF;
        for (i = start; i < end; i++)
            m[i - start] = VIAL_DEF[i];
        break;
    }
    case 0x03: {                            /* get encoder: layer, index -> CCW then CW, BE */
        uint32_t layer = m[2], idx = m[3];
        put_be16(&m[0], enc_get(layer, idx, 0));
        put_be16(&m[2], enc_get(layer, idx, 1));
        break;
    }
    case 0x04: {                            /* set encoder: layer, index, direction, keycode BE */
        uint16_t kc = be16(&m[5]);
        if (kc == QK_BOOT && !vl.unlocked)
            kc = KC_NO;
        if (m[2] < KB_LAYERS && m[3] < KB_NKNOB) {
            put_be16(&cfg.enc[enc_ofs(m[2], m[3], m[4] != 0)], kc);
            cfg_touch();
        }
        break;
    }
    case 0x05:                              /* unlock status: unlocked, in progress, the unlock keys (row, col) */
        for (i = 0; i < VIA_N; i++)
            m[i] = 0xFF;
        m[0] = vl.unlocked;
        m[1] = vl.in_progress;
        m[2] = 0;                           /* OCT- */
        m[3] = 0;
        m[4] = 0;                           /* OCT+ */
        m[5] = 1;
        break;
    case 0x06:                              /* unlock start: the hold counts from now (keys already down: too) */
        vl.in_progress = 1;
        vl.hold_t0 = 0;
        vl.last_poll = kb_now();
        unlock_track(kb.lvl);
        break;
    case 0x07:                              /* unlock poll: where the hold is (unlock_track does the unlocking) */
        vl.last_poll = kb_now();
        unlock_track(kb.lvl);
        m[0] = vl.unlocked;
        m[1] = vl.in_progress;
        m[2] = (uint8_t)(vl.in_progress ? unlock_counter() : 0);
        break;
    case 0x08:                              /* lock */
        vl.unlocked = 0;
        break;
    case 0x09: {                            /* QMK settings query: the qsids above the one given, LE, then FF */
        uint32_t gt = m[2] | (uint32_t)m[3] << 8, k = 0;
        for (i = 0; i < VIA_N; i++)
            m[i] = 0xFF;
        for (i = 0; i < sizeof QS_IDS / sizeof QS_IDS[0] && k + 2u <= VIA_N; i++)
            if (QS_IDS[i] > gt) {
                m[k++] = (uint8_t)QS_IDS[i];
                m[k++] = (uint8_t)(QS_IDS[i] >> 8);
            }
        break;
    }
    case 0x0A: {                            /* QMK setting get: status, then the value (LE) */
        uint32_t id = m[2] | (uint32_t)m[3] << 8, v, n = qs_get(id, &v);
        for (i = 0; i < VIA_N; i++)
            m[i] = 0;
        if (!n) {
            m[0] = 0xFF;                    /* (-1: not a setting of ours) */
            break;
        }
        for (i = 0; i < n; i++)
            m[1u + i] = (uint8_t)(v >> (8u * i));
        break;
    }
    case 0x0B: {                            /* QMK setting set: the value from m[4] (LE) */
        uint32_t id = m[2] | (uint32_t)m[3] << 8;
        m[0] = qs_set(id, m[4] | (uint32_t)m[5] << 8) ? 0u : 0xFFu;
        break;
    }
    case 0x0C:                              /* QMK settings reset */
        cfg_qs_defaults();
        cfg_touch();
        break;
    case 0x0D:                              /* dynamic entries */
        switch (m[2]) {
        case 0x00:                          /* counts: tap dance, combo, key override, alt repeat; features */
            for (i = 0; i < VIA_N; i++)
                m[i] = 0;
            m[0] = KB_TDS;
            m[1] = KB_COMBOS;
            break;
        case 0x01:                          /* tap dance get: status, the entry */
        case 0x03: {                        /* combo get */
            uint32_t idx = m[3], max = m[2] == 0x01 ? KB_TDS : KB_COMBOS;
            const uint8_t *src = m[2] == 0x01 ? cfg.td[idx < KB_TDS ? idx : 0] : cfg.combo[idx < KB_COMBOS ? idx : 0];
            for (i = 0; i < VIA_N; i++)
                m[i] = 0;
            if (idx >= max) {
                m[0] = 0xFF;
                break;
            }
            for (i = 0; i < 10u; i++)
                m[1u + i] = src[i];
            break;
        }
        case 0x02:                          /* tap dance set: the entry from m[4] (QK_BOOT kept out while locked) */
        case 0x04: {                        /* combo set */
            uint32_t idx = m[3], max = m[2] == 0x02 ? KB_TDS : KB_COMBOS, f;
            uint8_t *dst;
            if (idx >= max) {
                m[0] = 0xFF;
                break;
            }
            dst = m[2] == 0x02 ? cfg.td[idx] : cfg.combo[idx];
            for (i = 0; i < 10u; i++)
                dst[i] = m[4u + i];
            for (f = 0; f < 5u; f++)        /* (vial.c's firewall: the keycodes; a tap dance's term is no key) */
                if (!(m[2] == 0x02 && f == 4u) && le16(&dst[2u * f]) == QK_BOOT && !vl.unlocked) {
                    dst[2u * f] = 0;
                    dst[2u * f + 1u] = 0;
                }
            if (m[2] == 0x04)
                combos_reset();             /* (a changed combo: nothing held back or active any more) */
            m[0] = 0;
            cfg_touch();
            break;
        }
        default:                            /* key overrides, alt repeat keys: none */
            m[0] = 0xFF;
        }
        break;
    }
    return 0;
}

static int vial_cmd_(uint8_t *m);
static int vial_cmd(uint8_t *m)
{
    uint32_t t0 = cfg_touches;
    int r = vial_cmd_(m);
    if (cfg_touches != t0)
        cfg_vial_gen++;
    return r;
}
static int vial_cmd_(uint8_t *m)
{
    uint32_t i;
    if (vl.in_progress) {                   /* during an unlock only these (via.c) */
        if (m[0] != 0xFE || !(m[1] <= 0x02 || (m[1] >= 0x05 && m[1] <= 0x07)))
            return 0;
    }
    switch (m[0]) {
    case 0x01:                              /* protocol version */
        m[1] = VIA_PROTOCOL >> 8;
        m[2] = VIA_PROTOCOL & 0xFFu;
        break;
    case 0x02:                              /* get keyboard value */
        switch (m[1]) {
        case 0x01: {                        /* uptime */
            uint32_t t = kb_now();
            m[2] = (uint8_t)(t >> 24);
            m[3] = (uint8_t)(t >> 16);
            m[4] = (uint8_t)(t >> 8);
            m[5] = (uint8_t)t;
            break;
        }
        case 0x02:                          /* layout options: none */
            m[2] = m[3] = m[4] = m[5] = 0;
            break;
        case 0x03:                          /* switch matrix state: the key tester, only when unlocked */
            if (!vl.unlocked)
                return 0;
            matrix_rows(&m[2]);
            break;
        case 0x80:                          /* fm1_keyboard's status (vendor sub-id: via.c passes unknown ones to
                                             * the keyboard): read-only counters, tools/vialcli.py status */
            for (i = 2; i < VIA_N; i++)
                m[i] = 0;
            kb_diag(&m[2]);
            break;
        default:
            m[0] = 0xFF;
        }
        break;
    case 0x03:                              /* set keyboard value: layout options only (none) */
        if (m[1] != 0x02)
            m[0] = 0xFF;
        break;
    case 0x04:                              /* get keycode: layer, row, col */
        put_be16(&m[4], km_get(m[1], m[2], m[3]));
        break;
    case 0x05: {                            /* set keycode */
        uint16_t kc = be16(&m[4]);
        if (kc == QK_BOOT && !vl.unlocked)
            kc = KC_NO;
        if (m[1] < KB_LAYERS && m[2] < KB_ROWS && m[3] < KB_COLS) {
            put_be16(&cfg.keymap[km_ofs(m[1], m[2], m[3])], kc);
            cfg_touch();
        }
        break;
    }
    case 0x06:                              /* keymap reset */
        cfg_keymap_defaults();
        cfg_touch();
        break;
    case 0x0B:                              /* bootloader jump: when unlocked, after the reply */
        if (!vl.unlocked)
            return 0;
        return 1;
    case 0x0C:                              /* macro count */
        m[1] = KB_MACROS;
        break;
    case 0x0D:                              /* macro buffer size, BE */
        m[1] = (uint8_t)(KB_MACRO_BYTES >> 8);
        m[2] = (uint8_t)KB_MACRO_BYTES;
        break;
    case 0x0E:                              /* macro buffer get: offset BE, size <= 28 */
    case 0x0F: {                            /* set: only when unlocked */
        uint32_t ofs = be16(&m[1]), n = m[3];
        if (n > 28u)
            break;
        if (m[0] == 0x0F && !vl.unlocked)
            return 0;
        for (i = 0; i < n; i++) {
            if (ofs + i >= KB_MACRO_BYTES)
                break;
            if (m[0] == 0x0E)
                m[4u + i] = cfg.macros[ofs + i];
            else
                cfg.macros[ofs + i] = m[4u + i];
        }
        if (m[0] == 0x0F) {
            macro_stop();                   /* the buffer changes under a running macro */
            cfg_touch();
        }
        break;
    }
    case 0x10:                              /* macro reset */
        macro_stop();
        for (i = 0; i < KB_MACRO_BYTES; i++)
            cfg.macros[i] = 0;
        cfg_touch();
        break;
    case 0x11:                              /* layer count */
        m[1] = KB_LAYERS;
        break;
    case 0x12:                              /* keymap buffer get: offset BE, size <= 28 */
    case 0x13: {                            /* set (QK_BOOT kept out while locked, element by element) */
        uint32_t ofs = be16(&m[1]), n = m[3];
        if (n > 28u)
            break;
        for (i = 0; i < n && ofs + i < CFG_KEYMAP_BYTES; i++) {
            if (m[0] == 0x12)
                m[4u + i] = cfg.keymap[ofs + i];
            else
                cfg.keymap[ofs + i] = m[4u + i];
        }
        if (m[0] == 0x13) {
            if (!vl.unlocked) {
                uint32_t e;
                for (e = ofs & ~1u; e < ofs + n && e + 1u < CFG_KEYMAP_BYTES; e += 2u)
                    if (be16(&cfg.keymap[e]) == QK_BOOT)
                        put_be16(&cfg.keymap[e], KC_NO);
            }
            cfg_touch();
        }
        break;
    }
    case 0xFE:                              /* Vial */
        return vial_sub(m);
    default:                                /* lighting (none), eeprom reset, anything else */
        m[0] = 0xFF;
    }
    return 0;
}
