/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: the on-device editor, portable: kbcore.c's cfg and ui.c's drawing.
 *   CHOOSE   nothing being edited: a control pressed / a knob turned is pointed at; PRESETS the layer to edit,
 *            SELECT the action, OCT+ does it, OCT- leaves
 *   EDITING  one control: PRESETS the category, SELECT the grid's rows (and pages), ALGORITHM across; the cursor
 *            is the value; FX..EDIT modifiers (MODE LAYER: the hold layer), GLO the MODE, HOME..REC its light;
 *            white keys jump to the grid's pages; OCT+ saves, OCT- cancels
 * The platform calls ed_enter (OCT+ in NORMAL), then, while ed_on(), ed_input instead of kb_input, ed_draw for the
 * screen and ed_led for the LEDs. Leaving gives the panel back (kb_resync) on the layer edited. */

enum { ED_OFF, ED_CHOOSE, ED_EDIT, ED_PICK2, ED_CONFIRM, ED_COPYL, ED_INFO, ED_LEDTEST };
enum { A_EDIT, A_COPY, A_SWAP, A_CLEAR, A_DEFAULT, A_COPYL, A_CLEARL, A_INFO, A_LEDTEST, A_N };
static const char *const A_NAMES[A_N] = {"EDIT", "COPY", "SWAP", "CLEAR", "DEFAULT", "COPY LAYER", "CLEAR LAYER",
                                         "INFO", "LED TEST"};
enum { M_WITH, M_HOLD, M_ONESHOT, M_LAYER, M_N };
static const char *const M_NAMES[M_N] = {"WITH", "HOLD", "ONE-SHOT", "LAYER"};
#define ED_KEEP 0xFFFFu                     /* the grid cell "keep the value set in Vial" */
#define ED_NONE_LAYER 0xFFu
#define ED_ROWS 6u                          /* grid rows on the screen */
#define ED_MSG_MS 2500u

/* ---------------------------------------------------------------- the categories */
static const uint16_t CAT_EMPTY[] = {KC_TRNS, KC_NO, ED_KEEP};
static const uint16_t CAT_ALNUM[] = {
    KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G, KC_H, KC_I, KC_J, KC_K, KC_L, KC_M, KC_N, KC_O, KC_P, KC_Q, KC_R,
    KC_S, KC_T, KC_U, KC_V, KC_W, KC_X, KC_Y, KC_Z, KC_1, KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9, KC_0};
#define S_(k) ((uint16_t)(MOD_BIT_SFT << 8 | (k)))
static const uint16_t CAT_SYM[] = {
    0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,                  /* - = [ ] \ ; ' ` , . / */
    S_(0x1E), S_(0x1F), S_(0x20), S_(0x21), S_(0x22), S_(0x23), S_(0x24), S_(0x25),    /* ! @ # $ % ^ & * */
    S_(0x26), S_(0x27), S_(0x2D), S_(0x2E), S_(0x2F), S_(0x30), S_(0x31), S_(0x33),    /* ( ) _ + { } | : */
    S_(0x34), S_(0x35), S_(0x36), S_(0x37), S_(0x38)};                                 /* " ~ < > ? */
static const char SYM_CHARS[] = "-=[]\\;'`,./!@#$%^&*()_+{}|:\"~<>?";
static const uint16_t CAT_NAV[] = {
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x4C, 0x49, 0x39,                                     /* Enter .. Del Ins Caps */
    0x50, 0x52, 0x51, 0x4F, 0x4A, 0x4D, 0x4B, 0x4E,                                     /* arrows, Home End PgUp/Dn */
    0x46, 0x47, 0x48, 0x65};                                                            /* PrtSc ScrLk Pause Menu */
static const uint16_t CAT_FKEYS[] = {
    0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
    0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73};
static const uint16_t CAT_NUMPAD[] = {
    0x5F, 0x60, 0x61, 0x54, 0x5C, 0x5D, 0x5E, 0x55, 0x59, 0x5A, 0x5B, 0x56, 0x62, 0x63, 0x58, 0x57, 0x53, 0x67};
static const uint16_t CAT_MEDIA[] = {
    KC_VOLU, KC_VOLD, KC_MUTE, KC_MPLY, KC_MNXT, KC_MPRV, KC_MSTP, 0xBB, 0xBC, 0xBD, 0xBE, 0xB7, 0xB6, 0xB9, 0xBA,
    0xB4, 0xB5, 0xB3, 0xB8, 0xB1, 0xB2, 0xB0, 0xC0, 0xBF, 0xC1, 0xC2, KC_PWR, KC_SLEP, KC_WAKE};
static const uint16_t CAT_MOUSE[] = {
    KC_MS_U, KC_MS_D, KC_MS_L, KC_MS_R, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, KC_WH_U, KC_WH_D, KC_WH_L, KC_WH_R,
    0xDD, 0xDE, 0xDF};
#define LAYER_ROW(base) (base), (base) + 1u, (base) + 2u, (base) + 3u, (base) + 4u
static const uint16_t CAT_LAYERS[] = {LAYER_ROW(QK_MOMENTARY), LAYER_ROW(QK_ONE_SHOT_LAYER),
                                      LAYER_ROW(QK_DEF_LAYER), LAYER_ROW(QK_TOGGLE_LAYER)};
static const uint16_t CAT_MACROS[] = {
    0x7700, 0x7701, 0x7702, 0x7703, 0x7704, 0x7705, 0x7706, 0x7707,
    0x7708, 0x7709, 0x770A, 0x770B, 0x770C, 0x770D, 0x770E, 0x770F};
static const uint16_t CAT_TD[] = {
    0x5700, 0x5701, 0x5702, 0x5703, 0x5704, 0x5705, 0x5706, 0x5707,
    0x5708, 0x5709, 0x570A, 0x570B, 0x570C, 0x570D, 0x570E, 0x570F};
static const uint16_t CAT_FM1[] = {FK_LAYER_PREV, FK_LAYER_NEXT, FK_KB_OFF};
static const uint16_t CAT_OTHER[] = {
    0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x66, 0x85,
    0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x82, 0x83, 0x84, 0x64, 0x32};
typedef struct {
    const char *name;
    const uint16_t *items;
    uint8_t n, cols;
} ed_cat_t;
#define CAT(name, a, cols) {name, a, sizeof a / sizeof a[0], cols}
enum { C_EMPTY, C_ALNUM, C_SYM, C_NAV, C_FKEYS, C_NUMPAD, C_MEDIA, C_MOUSE, C_LAYERS, C_MACROS, C_TD, C_FM1, C_OTHER,
       C_N };
static const ed_cat_t CATS[C_N] = {
    CAT("EMPTY", CAT_EMPTY, 3), CAT("LETTERS + DIGITS", CAT_ALNUM, 6), CAT("SYMBOLS", CAT_SYM, 6),
    CAT("EDIT / NAV", CAT_NAV, 4), CAT("F-KEYS", CAT_FKEYS, 6), CAT("NUMPAD", CAT_NUMPAD, 6),
    CAT("MEDIA", CAT_MEDIA, 4), CAT("MOUSE", CAT_MOUSE, 4), CAT("LAYERS", CAT_LAYERS, 5),
    CAT("MACROS", CAT_MACROS, 4), CAT("TAP DANCE", CAT_TD, 4), CAT("FM-1", CAT_FM1, 3),
    CAT("OTHER", CAT_OTHER, 4),
};
static const char *const OTHER_S[] = {"EXEC", "HELP", "MENU", "SEL", "STOP", "AGIN", "UNDO", "CUT", "COPY", "PAST",
                                      "FIND", "MUTE", "VOL+", "VOL-", "POWR", "K,"};

/* ---------------------------------------------------------------- state */
static struct {
    uint8_t st, layer, action;
    int16_t ptr, src;                       /* the control pointed at (pos, or 100 + 2 * knob + cw; -1 none);
                                             * COPY / SWAP's source */
    uint8_t dest;                           /* COPY LAYER's destination */
    /* EDITING */
    uint16_t orig_kc, keep_kc;
    uint8_t orig_light, light;
    uint8_t cat, top, mods, mode, hold_layer, has_keep;
    uint16_t cur;
    /* input */
    uint8_t oct_down, oct_both;             /* OCT- / OCT+ held (bits 0, 1); both were down in this press */
    uint32_t both_t0;                       /* when both went down (0: not both): held 5 s, UBOOT as outside */
    uint8_t drawn_cat;                      /* the grid as drawn (another category: cleared first) */
    uint32_t prev_btn, prev_notes;
    /* messages */
    const char *msg;
    uint32_t msg_t;
    uint16_t msg_c;
    uint32_t gen;                           /* cfg_vial_gen when last seen */
    uint8_t redraw;
} ed;

static int ed_on(void) { return ed.st != ED_OFF; }
static int t_is_knob(int32_t t) { return t >= 100; }
static void ed_say(const char *m, uint16_t c, uint32_t now)
{
    ed.msg = m;
    ed.msg_c = c;
    ed.msg_t = now;
}

/* a target's stored binding and light on a layer */
static uint16_t t_get(uint32_t layer, int32_t t)
{
    uint8_t r, c;
    if (t_is_knob(t))
        return enc_get(layer, (uint32_t)(t - 100) / 2u, (uint32_t)(t - 100) & 1u);
    pos_rowcol((uint32_t)t, &r, &c);
    return km_get(layer, r, c);
}
static void t_set(uint32_t layer, int32_t t, uint16_t kc)
{
    uint8_t r, c;
    if (t_is_knob(t))
        put_be16(&cfg.enc[enc_ofs(layer, (uint32_t)(t - 100) / 2u, (uint32_t)(t - 100) & 1u)], kc);
    else {
        pos_rowcol((uint32_t)t, &r, &c);
        put_be16(&cfg.keymap[km_ofs(layer, r, c)], kc);
    }
}
static uint8_t t_light(uint32_t layer, int32_t t) { return t_is_knob(t) ? LIGHT_AUTO : cfg.light[layer][t]; }
static void t_set_light(uint32_t layer, int32_t t, uint8_t l)
{
    if (!t_is_knob(t))
        cfg.light[layer][t] = l;
}
static uint16_t t_default(uint32_t layer, int32_t t)
{
    return t_is_knob(t) ? default_knob(layer, (uint32_t)(t - 100) / 2u, (uint32_t)(t - 100) & 1u) :
                          default_key(layer, (uint32_t)t);
}

/* ---------------------------------------------------------------- the value: grid cell + modifiers + mode */
static uint16_t ed_item(void) { return CATS[ed.cat].items[ed.cur]; }
static int kc_is_key(uint16_t kc) { return (kc >= KC_A && kc <= KC_KB_LAST) || (kc >= KC_LCTL && kc <= KC_RGUI); }
static int kc_is_layer_cell(uint16_t kc) { return kc >= QK_TO && kc <= QK_ONE_SHOT_LAYER + QK_LAYER_MAX_OFS; }

/* the modifiers alone as a keycode: one of them a modifier key, more: QMK's mods with no key */
static uint16_t mods_only(uint32_t m)
{
    uint32_t b, n = 0, last = 0;
    for (b = 0; b < 4u; b++)
        if (m >> b & 1u) {
            n++;
            last = b;
        }
    if (n == 1u)
        return (uint16_t)(KC_LCTL + last + (m & MOD_BIT_RIGHT ? 4u : 0u));
    return (uint16_t)((m & 0x1Fu) << 8);
}

/* the binding the editor shows; 0 = valid, else *err says why it cannot be saved */
static int ed_compose(uint16_t *out, const char **err)
{
    uint16_t it = ed_item(), base = it;
    uint32_t m = ed.mods, knob = t_is_knob(ed.ptr);
    *err = 0;
    *out = KC_NO;
    if (ed.mode == M_LAYER)
        m = 0;                              /* (the buttons are the hold layer then) */
    if (knob && ed.mode != M_WITH) {
        *err = "KNOB: NOTHING TO HOLD";
        return -1;
    }
    if (it == ED_KEEP) {
        *out = ed.keep_kc;
        if (m || ed.mode != M_WITH)
            *err = "SET IN VIAL: KEPT AS IS";
        return *err ? -1 : 0;
    }
    if (it >= QK_MODS && it <= QK_MODS_MAX) {   /* a shifted symbol: its key with Shift */
        base = it & 0xFFu;
        if (ed.mode == M_HOLD || ed.mode == M_LAYER) {
            *err = "TAP KEY: NO SHIFTED KEYS";
            return -1;
        }
        m |= it >> 8 & 0x1Fu;
    }
    switch (ed.mode) {
    case M_ONESHOT:
        if (!m) {
            *err = "ONE-SHOT: PICK MODIFIERS";
            return -1;
        }
        *out = OSM(m);
        return 0;
    case M_HOLD:
        if (!kc_is_key(base) || base >= KC_LCTL) {
            *err = "HOLD: PICK A KEY";
            return -1;
        }
        if (!m) {
            *err = "HOLD: PICK MODIFIERS";
            return -1;
        }
        *out = MT(m, base);
        return 0;
    case M_LAYER:
        if (!kc_is_key(base) || base >= KC_LCTL) {
            *err = "LAYER: PICK A KEY";
            return -1;
        }
        if (ed.hold_layer == ED_NONE_LAYER) {
            *err = "LAYER: PICK L0-L4";
            return -1;
        }
        *out = LT(ed.hold_layer, base);
        return 0;
    }
    /* WITH */
    if (base == KC_TRNS) {
        if (m)
            *err = "SAME: NO MODIFIERS";
        *out = KC_TRNS;
        return *err ? -1 : 0;
    }
    if (base == KC_NO) {
        *out = m ? mods_only(m) : (uint16_t)KC_NO;
        return 0;
    }
    if (kc_is_key(base)) {
        *out = m ? (uint16_t)((m & 0x1Fu) << 8 | base) : base;   /* QMK's mods + key (QK_MODS range) */
        return 0;
    }
    if (kc_is_layer_cell(base)) {
        uint32_t n = base & QK_LAYER_MAX_OFS;
        int held = base >= QK_MOMENTARY && base <= QK_MOMENTARY + QK_LAYER_MAX_OFS;
        if (knob && !(base >= QK_DEF_LAYER && base <= QK_DEF_LAYER + QK_LAYER_MAX_OFS)) {
            *err = "KNOB: GO TO ONLY";
            return -1;
        }
        if (m && !held) {
            *err = "MODIFIERS: HOLD ROW ONLY";
            return -1;
        }
        *out = m ? LM(n, m) : base;
        return 0;
    }
    if (m) {
        *err = "MODIFIERS: KEYS ONLY";
        return -1;
    }
    if (knob && base >= QK_TAP_DANCE && base <= QK_TAP_DANCE_MAX) {
        *err = "KNOB: NO TAP DANCE";
        return -1;
    }
    *out = base;
    return 0;
}

/* the cell holding kc (first category with it), 1 = found */
static int ed_find(uint16_t kc)
{
    uint32_t c, i;
    for (c = 0; c < C_N; c++)
        for (i = 0; i < CATS[c].n; i++)
            if (CATS[c].items[i] == kc && kc != ED_KEEP) {
                ed.cat = (uint8_t)c;
                ed.cur = (uint16_t)i;
                return 1;
            }
    return 0;
}

/* the grid row showing the cursor */
static void ed_scroll(void)
{
    uint32_t row = ed.cur / CATS[ed.cat].cols;
    while (row < ed.top)
        ed.top = (uint8_t)(ed.top >= ED_ROWS - 1u ? ed.top - (ED_ROWS - 1u) : 0u);
    while (row >= ed.top + ED_ROWS)
        ed.top = (uint8_t)(ed.top + (ED_ROWS - 1u));
}

/* a binding into the editor's terms; what does not come back the same way: KEEP (set in Vial) */
static void ed_load(uint16_t kc)
{
    uint16_t again;
    const char *err;
    int ok = 0;
    ed.mods = 0;
    ed.mode = M_WITH;
    ed.hold_layer = ED_NONE_LAYER;
    ed.has_keep = 0;
    ed.top = 0;
    if (ed_find(kc))
        ok = 1;
    else if (kc >= KC_LCTL && kc <= KC_RGUI) {
        ed.mods = (uint8_t)(1u << ((kc - KC_LCTL) & 3u) | (kc >= KC_RCTL ? MOD_BIT_RIGHT : 0u));
        ok = ed_find(KC_NO);
    } else if (kc >= QK_MODS && kc <= QK_MODS_MAX) {
        uint32_t m = kc >> 8 & 0x1Fu, base = kc & 0xFFu;
        if ((m & 0x1Fu) == MOD_BIT_SFT && ed_find(S_(base)))
            ok = 1;                         /* a shifted symbol */
        else {
            ed.mods = (uint8_t)m;
            ok = ed_find(base ? (uint16_t)base : (uint16_t)KC_NO);
        }
    } else if (kc >= QK_MOD_TAP && kc <= QK_MOD_TAP_MAX) {
        ed.mode = M_HOLD;
        ed.mods = (uint8_t)(kc >> 8 & 0x1Fu);
        ok = ed_find(kc & 0xFFu);
    } else if (kc >= QK_LAYER_TAP && kc <= QK_LAYER_TAP_MAX) {
        ed.mode = M_LAYER;
        ed.hold_layer = (uint8_t)(kc >> 8 & 0xFu);
        ok = ed_find(kc & 0xFFu);
    } else if (kc >= QK_ONE_SHOT_MOD && kc <= QK_ONE_SHOT_MOD + 0x1Fu) {
        ed.mode = M_ONESHOT;
        ed.mods = (uint8_t)(kc & 0x1Fu);
        ok = ed_find(KC_NO);
    } else if (kc >= QK_LAYER_MOD && kc <= QK_LAYER_MOD_MAX) {
        ed.mods = (uint8_t)(kc & 0x1Fu);
        ok = ed_find(MO(kc >> 5 & 0xFu));
    }
    if (ok && (ed_compose(&again, &err) || again != kc))
        ok = 0;                             /* (e.g. a layer beyond 4, a combination the editor would change) */
    if (!ok) {
        ed.mods = 0;
        ed.mode = M_WITH;
        ed.hold_layer = ED_NONE_LAYER;
        ed.has_keep = 1;
        ed.keep_kc = kc;
        ed.cat = C_EMPTY;
        ed.cur = 2;                         /* KEEP */
    }
    ed_scroll();
}

/* ---------------------------------------------------------------- entering, leaving */
static void ed_enter(uint32_t now)
{
    kb_release_all();
    ed.st = ED_CHOOSE;
    ed.layer = cfg.layer;
    ed.action = A_EDIT;
    ed.ptr = -1;
    ed.oct_down = 0;
    ed.oct_both = 0;
    ed.both_t0 = 0;
    ed.prev_btn = 0xFFFFFFFFu;              /* what is held now (the chord) is not a press */
    ed.prev_notes = 0xFFFFFFFFu;
    ed.msg = 0;
    ed.gen = cfg_vial_gen;
    ed.redraw = 1;
    ed_say("EDITOR", C_CYAN, now);
}

static void ed_leave(uint32_t btn, uint32_t notes)
{
    ed.st = ED_OFF;
    kb_resync(btn, notes);
    layer_set(ed.layer);                    /* where you edited: try it at once */
    un.drawn_page = 0xFF;                   /* (the NORMAL screen again, all of it) */
    ui_invalidate();
}

static void ed_edit_begin(void)
{
    ed.orig_kc = t_get(ed.layer, ed.ptr);
    ed.orig_light = t_light(ed.layer, ed.ptr);
    ed.light = ed.orig_light;
    ed_load(ed.orig_kc);
    ed.st = ED_EDIT;
    ed.msg = 0;                             /* (CHOOSE's last message is not about this) */
    ed.redraw = 1;
}

/* ---------------------------------------------------------------- input */
static void ed_target_from(uint32_t pos_pressed, int32_t knob_turned)
{
    if (knob_turned >= 0)
        ed.ptr = (int16_t)knob_turned;
    else
        ed.ptr = (int16_t)pos_pressed;
}

static void ed_action(uint32_t now)
{
    switch (ed.action) {
    case A_EDIT:
        if (ed.ptr < 0) {
            ed_say("PRESS A CONTROL FIRST", C_RED, now);
            return;
        }
        ed_edit_begin();
        return;
    case A_COPY:
    case A_SWAP:
        if (ed.ptr < 0) {
            ed_say("PRESS A CONTROL FIRST", C_RED, now);
            return;
        }
        ed.src = ed.ptr;
        ed.ptr = -1;
        ed.st = ED_PICK2;
        ed.redraw = 1;
        return;
    case A_CLEAR:
    case A_DEFAULT:
        if (ed.ptr < 0) {
            ed_say("PRESS A CONTROL FIRST", C_RED, now);
            return;
        }
        t_set(ed.layer, ed.ptr, ed.action == A_CLEAR ? (uint16_t)KC_TRNS : t_default(ed.layer, ed.ptr));
        t_set_light(ed.layer, ed.ptr, LIGHT_AUTO);
        cfg_touch();
        ed.gen = cfg_vial_gen;
        ed_say(ed.action == A_CLEAR ? "CLEARED" : "BACK TO DEFAULT", C_GREEN, now);
        ed.action = A_EDIT;
        return;
    case A_COPYL:
        ed.dest = (uint8_t)((ed.layer + 1u) % KB_LAYERS);
        ed.st = ED_COPYL;
        ed.redraw = 1;
        return;
    case A_CLEARL:
        ed.st = ED_CONFIRM;
        ed.redraw = 1;
        return;
    case A_INFO:
        ed.st = ED_INFO;
        ed.redraw = 1;
        return;
    case A_LEDTEST:
        ed.st = ED_LEDTEST;
        ed.redraw = 1;
        return;
    }
}

static void ed_copy_layer(uint32_t from, uint32_t to)
{
    uint32_t p, k;
    for (p = 0; p < KB_NPOS; p++) {
        t_set(to, (int32_t)p, t_get(from, (int32_t)p));
        cfg.light[to][p] = cfg.light[from][p];
    }
    for (k = 0; k < 2u * KB_NKNOB; k++)
        t_set(to, (int32_t)(100 + k), t_get(from, (int32_t)(100 + k)));
}

/* OCT+ (ok) / OCT- (back), released without the other pressed meanwhile */
static void ed_ok(uint32_t now)
{
    switch (ed.st) {
    case ED_CHOOSE:
        ed_action(now);
        break;
    case ED_EDIT: {
        uint16_t kc;
        const char *err;
        if (ed_compose(&kc, &err)) {
            ed_say(err, C_RED, now);
            return;
        }
        if (kc != ed.orig_kc || ed.light != ed.orig_light) {
            t_set(ed.layer, ed.ptr, kc);
            t_set_light(ed.layer, ed.ptr, ed.light);
            cfg_touch();
            ed.gen = cfg_vial_gen;
            ed_say("SAVED", C_GREEN, now);
        }
        ed.st = ED_CHOOSE;
        ed.redraw = 1;
        break;
    }
    case ED_PICK2:
        if (ed.ptr < 0 || ed.ptr == ed.src) {
            ed_say("PRESS ANOTHER CONTROL", C_YELLOW, now);
            return;
        }
        if (t_is_knob(ed.ptr) != t_is_knob(ed.src)) {
            ed_say(t_is_knob(ed.src) ? "KNOB TO KNOB ONLY" : "NOT TO A KNOB", C_RED, now);
            return;
        }
        if (ed.action == A_SWAP) {
            uint16_t a = t_get(ed.layer, ed.src), b = t_get(ed.layer, ed.ptr);
            uint8_t la = t_light(ed.layer, ed.src), lb = t_light(ed.layer, ed.ptr);
            t_set(ed.layer, ed.src, b);
            t_set(ed.layer, ed.ptr, a);
            t_set_light(ed.layer, ed.src, lb);
            t_set_light(ed.layer, ed.ptr, la);
            ed_say("SWAPPED", C_GREEN, now);
            ed.st = ED_CHOOSE;
            ed.action = A_EDIT;
            ed.redraw = 1;
        } else {
            t_set(ed.layer, ed.ptr, t_get(ed.layer, ed.src));
            t_set_light(ed.layer, ed.ptr, t_light(ed.layer, ed.src));
            ed_say("PASTED: NEXT, OR OCT- DONE", C_GREEN, now);
        }
        cfg_touch();
        ed.gen = cfg_vial_gen;
        break;
    case ED_CONFIRM: {
        uint32_t p, k;
        for (p = 0; p < KB_NPOS; p++)
            if (p != POS_OCTDN && p != POS_OCTUP) {
                t_set(ed.layer, (int32_t)p, KC_TRNS);
                cfg.light[ed.layer][p] = LIGHT_AUTO;
            }
        for (k = 0; k < 2u * KB_NKNOB; k++)
            t_set(ed.layer, (int32_t)(100 + k), KC_TRNS);
        cfg_touch();
        ed.gen = cfg_vial_gen;
        ed_say("LAYER CLEARED", C_GREEN, now);
        ed.st = ED_CHOOSE;
        ed.action = A_EDIT;
        ed.redraw = 1;
        break;
    }
    case ED_COPYL:
        ed_copy_layer(ed.layer, ed.dest);
        cfg_touch();
        ed.gen = cfg_vial_gen;
        ed_say("LAYER COPIED", C_GREEN, now);
        ed.layer = ed.dest;
        ed.st = ED_CHOOSE;
        ed.action = A_EDIT;
        ed.redraw = 1;
        break;
    case ED_INFO:
    case ED_LEDTEST:
        ed.st = ED_CHOOSE;
        ed.action = A_EDIT;
        ed.redraw = 1;
        break;
    }
}

static void ed_back(uint32_t btn, uint32_t notes, uint32_t now)
{
    switch (ed.st) {
    case ED_CHOOSE:
        ed_leave(btn, notes);
        break;
    case ED_EDIT:
        ed_say("CANCELLED", C_GREY, now);
        /* fall through */
    default:
        if (ed.st == ED_PICK2 && ed.action == A_COPY)
            ed.action = A_EDIT;
        ed.st = ED_CHOOSE;
        ed.redraw = 1;
        break;
    }
}

/* the tool buttons while EDITING (pos 2..13) */
static void ed_tool(uint32_t pos, uint32_t now)
{
    if (pos >= 2u && pos <= 6u) {           /* FX SEL ENV LFO EDIT: Ctrl Shift Alt GUI Right, or a hold layer */
        uint32_t i = pos - 2u;
        if (ed.mode == M_LAYER)
            ed.hold_layer = ed.hold_layer == i ? ED_NONE_LAYER : (uint8_t)i;
        else
            ed.mods ^= (uint8_t)(1u << i);
    } else if (pos == 7u) {                 /* GLO: MODE */
        ed.mode = (uint8_t)((ed.mode + 1u) % M_N);
    } else if (pos >= 8u && pos <= 13u) {   /* HOME..REC: OFF DIM MID ON BREATHE BLINK; the same again: AUTO */
        uint8_t st = (uint8_t)(LIGHT_OFF + (pos - 8u));
        if (t_is_knob(ed.ptr)) {
            ed_say("KNOBS HAVE NO LIGHT", C_YELLOW, now);
            return;
        }
        ed.light = ed.light == st ? (uint8_t)LIGHT_AUTO : st;
    }
}

/* the white keys: their index 0..15 (note -> white key), or -1 */
static int32_t white_index(uint32_t note)
{
    uint32_t i, w = 0;
    if (note_is_black(note))
        return -1;
    for (i = 0; i < note; i++)
        if (!note_is_black(i))
            w++;
    return (int32_t)w;
}
static uint32_t ed_pages(void)
{
    uint32_t rows = (CATS[ed.cat].n + CATS[ed.cat].cols - 1u) / CATS[ed.cat].cols;
    return rows <= ED_ROWS ? 1u : 1u + (rows - ED_ROWS + ED_ROWS - 2u) / (ED_ROWS - 1u);
}

/* a pass of the panel (as kb_input's arguments) */
static void ed_input(uint32_t btn_lvl, uint32_t note_lvl, uint32_t btn_pressed, uint32_t note_pressed,
                     const int32_t enc[KB_NENC], uint32_t now)
{
    uint32_t btn = btn_lvl | btn_pressed, notes = note_lvl | note_pressed, e, p;
    uint32_t pr_btn = btn & ~(ed.prev_btn == 0xFFFFFFFFu ? btn : ed.prev_btn) & ~3u;
    uint32_t pr_notes = notes & ~(ed.prev_notes == 0xFFFFFFFFu ? notes : ed.prev_notes);
    uint32_t oct = btn & 3u;
    if (ed.prev_btn == 0xFFFFFFFFu)         /* the chord that opened the editor: its release is no OK / back */
        ed.oct_both = 1;
    ed.prev_btn = btn_lvl;
    ed.prev_notes = note_lvl;
    if (cfg_vial_gen != ed.gen) {           /* Vial changed the configuration meanwhile */
        ed.gen = cfg_vial_gen;
        if (ed.st == ED_EDIT && (t_get(ed.layer, ed.ptr) != ed.orig_kc || t_light(ed.layer, ed.ptr) != ed.orig_light)) {
            ed.st = ED_CHOOSE;
            ed_say("CHANGED IN VIAL: CANCELLED", C_YELLOW, now);
        } else
            ed_say("CHANGED IN VIAL", C_YELLOW, now);
        ui_invalidate();
        ed.redraw = 1;
    }
    /* OCT- / OCT+: on release, if the other was not pressed during it; both held CHORD_UBOOT_MS: UBOOT */
    if (oct == 3u) {
        ed.oct_both = 1;
        if (!ed.both_t0)
            ed.both_t0 = now ? now : 1u;
        else if (now - ed.both_t0 >= CHORD_UBOOT_MS)
            kb_uboot();
    } else
        ed.both_t0 = 0;
    if (ed.oct_down && !oct) {
        uint32_t which = ed.oct_down;
        int both = ed.oct_both;
        ed.oct_down = 0;
        ed.oct_both = 0;
        if (!both) {
            if (which & 2u)
                ed_ok(now);
            else
                ed_back(btn_lvl, note_lvl, now);
            if (!ed_on())
                return;
        }
    } else
        ed.oct_down |= (uint8_t)oct;
    if (!ed.oct_down)
        ed.oct_both = 0;

    switch (ed.st) {
    case ED_CHOOSE:
    case ED_PICK2: {
        int32_t knob = -1;
        for (e = ENC_KNOB1; e <= ENC_KNOB4; e++)
            if (enc[e])
                knob = 100 + 2 * (int32_t)(e - ENC_KNOB1) + (enc[e] > 0);
        for (p = 2; p < KB_NPOS; p++)
            if ((p < KB_NBTN ? pr_btn >> p : pr_notes >> (p - KB_NBTN)) & 1u)
                ed_target_from(p, -1);
        if (knob >= 0)
            ed_target_from(0, knob);
        if (ed.st == ED_CHOOSE) {
            if (enc[ENC_PRESETS])
                ed.layer = (uint8_t)((ed.layer + KB_LAYERS * 8u + (uint32_t)enc[ENC_PRESETS]) % KB_LAYERS);
            if (enc[ENC_SELECT])
                ed.action = (uint8_t)((ed.action + A_N * 8u + (uint32_t)enc[ENC_SELECT]) % A_N);
        }
        break;
    }
    case ED_COPYL:
        if (enc[ENC_PRESETS]) {
            ed.dest = (uint8_t)((ed.dest + KB_LAYERS * 8u + (uint32_t)enc[ENC_PRESETS]) % KB_LAYERS);
            if (ed.dest == ed.layer)
                ed.dest = (uint8_t)((ed.dest + KB_LAYERS + (enc[ENC_PRESETS] > 0 ? 1u : KB_LAYERS - 1u)) % KB_LAYERS);
        }
        break;
    case ED_EDIT: {
        const ed_cat_t *c = &CATS[ed.cat];
        if (enc[ENC_PRESETS]) {             /* the category; the cursor to its first cell */
            ed.cat = (uint8_t)((ed.cat + C_N * 8u + (uint32_t)enc[ENC_PRESETS]) % C_N);
            ed.cur = 0;
            ed.top = 0;
            c = &CATS[ed.cat];
        }
        if (enc[ENC_ALGORITHM]) {           /* across, wrapping into the next row */
            int32_t v = (int32_t)ed.cur + enc[ENC_ALGORITHM];
            ed.cur = (uint16_t)(v < 0 ? 0 : v >= c->n ? c->n - 1 : v);
        }
        if (enc[ENC_SELECT]) {              /* down / up a row */
            int32_t v = (int32_t)ed.cur + enc[ENC_SELECT] * (int32_t)c->cols;
            if (v < 0)
                v = (int32_t)ed.cur % c->cols;
            if (v >= c->n)
                v = c->n - 1;
            ed.cur = (uint16_t)v;
        }
        if (ed.cat == C_EMPTY && !ed.has_keep && ed.cur == 2u)
            ed.cur = 1;                     /* (KEEP only for a value set in Vial) */
        for (p = 2; p < KB_NBTN; p++)
            if (pr_btn >> p & 1u)
                ed_tool(p, now);
        for (p = 0; p < KB_NNOTE; p++)
            if (pr_notes >> p & 1u) {
                int32_t w = white_index(p);
                if (w >= 0 && (uint32_t)w < ed_pages()) {
                    uint32_t col = ed.cur % c->cols, row = (uint32_t)w * (ED_ROWS - 1u) + 0u;
                    uint32_t v = row * c->cols + col;
                    ed.top = (uint8_t)row;
                    ed.cur = (uint16_t)(v < c->n ? v : c->n - 1u);
                }
            }
        ed_scroll();
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- the LEDs (main.c: every pass, per control) */
/* OCT+ / OCT- lit when pressing them does something now */
static int ed_ok_means(void)
{
    uint16_t kc;
    const char *err;
    switch (ed.st) {
    case ED_CHOOSE:
        return ed.ptr >= 0 || (ed.action != A_EDIT && ed.action != A_COPY && ed.action != A_SWAP &&
                               ed.action != A_CLEAR && ed.action != A_DEFAULT);
    case ED_EDIT:
        return !ed_compose(&kc, &err);
    case ED_PICK2:
        return ed.ptr >= 0 && ed.ptr != ed.src && t_is_knob(ed.ptr) == t_is_knob(ed.src);
    default:
        return 1;
    }
}

static uint32_t ed_led(uint32_t pos)
{
    uint8_t r, c;
    if (pos == POS_OCTDN)
        return LV_ON;                       /* back / cancel / leave: always something */
    if (pos == POS_OCTUP)
        return ed_ok_means() ? LV_ON : LV_OFF;
    if (ed.st == ED_EDIT) {
        if ((int32_t)pos == ed.ptr) {       /* the light being chosen, as it will look */
            uint16_t kc;
            const char *err;
            if (ed_compose(&kc, &err))
                kc = ed.orig_kc;
            return light_level(ed.light == LIGHT_AUTO ? light_auto(kc) : ed.light);
        }
        if (pos >= 2u && pos <= 6u) {
            uint32_t i = pos - 2u;
            return (ed.mode == M_LAYER ? ed.hold_layer == i : ed.mods >> i & 1u) ? LV_ON : LV_DIM;
        }
        if (pos == 7u) {
            static const uint8_t ML[M_N] = {LV_DIM, LV_ON, LV_BLINK_SLOW, LV_BREATHE};
            return ML[ed.mode];
        }
        if (pos >= 8u && pos <= 13u)
            return t_is_knob(ed.ptr) ? LV_OFF : light_level(LIGHT_OFF + (pos - 8u));
        if (pos >= KB_NBTN) {               /* the white keys with a page: dim, the current one lit */
            int32_t w = white_index(pos - KB_NBTN);
            if (w >= 0 && (uint32_t)w < ed_pages() && ed_pages() > 1u)
                return (uint32_t)w == ed.top / (ED_ROWS - 1u) ? LV_ON : LV_DIM;
        }
        return LV_OFF;
    }
    if (ed.st == ED_LEDTEST)
        return LV_ON;
    if ((int32_t)pos == ed.ptr || (ed.st == ED_PICK2 && (int32_t)pos == ed.src))
        return LV_BLINK_SLOW;
    pos_rowcol(pos, &r, &c);                /* CHOOSE: the layer as it will look */
    return light_level(light_style(ed.layer, pos, km_get(ed.layer, r, c)));
}

/* ---------------------------------------------------------------- the screens */
enum { E_HEAD = R_N, E_STRIP1, E_STRIP2 = E_STRIP1 + 6, E_CAT = E_STRIP2 + 6, E_GRID, E_DESC = E_GRID + ED_ROWS * 6,
       E_HINT, E_MSG, E_N };
typedef char ed_regions_fit[E_N <= UI_REGIONS ? 1 : -1];
#define Y_E_HEAD 0u
#define Y_E_STRIP1 22u
#define Y_E_STRIP2 42u
#define Y_E_CAT 64u
#define Y_E_GRID 84u
#define E_CELL_H 19u
#define Y_E_DESC 202u
#define Y_E_HINT 222u

#define ed_box ui_box
#define ed_invalidate ui_invalidate

static void ed_draw_edit(uint32_t now)
{
    char b[32], *p;
    uint16_t kc;
    const char *err;
    uint32_t i, knob = t_is_knob(ed.ptr);
    const ed_cat_t *c = &CATS[ed.cat];
    static const char *const MODS_L[5] = {"CT", "SH", "AL", "GU", "RT"};
    static const char *const LIGHT_L[6] = {"OF", "DI", "MI", "ON", "BR", "BL"};
    int bad = ed_compose(&kc, &err);
    /* the bar: what is edited, where */
    p = ui_put(b, "EDIT ");
    pos_label(ed.ptr, p);
    p = b + gfx_len(b);
    p = ui_num(ui_put(p, "  L"), ed.layer);
    ed_box(E_HEAD, 0, Y_E_HEAD, GFX_W, 20, b, F_S, 2, C_BLACK, C_CYAN, 0, 0);
    /* the tool strip: FX..GLO, HOME..REC as they are */
    for (i = 0; i < 6u; i++) {
        int on;
        const char *l;
        char lb[4];
        if (i < 5u) {
            if (ed.mode == M_LAYER) {
                lb[0] = 'L';
                lb[1] = (char)('0' + i);
                lb[2] = 0;
                l = lb;
                on = ed.hold_layer == i;
            } else {
                uint32_t m = ed.mods;
                if (ed_item() >= QK_MODS && ed_item() <= QK_MODS_MAX)
                    m |= ed_item() >> 8 & 0x1Fu;   /* (a shifted symbol: SH shows) */
                l = MODS_L[i];
                on = m >> i & 1u;
            }
        } else {
            l = "MD";
            on = ed.mode != M_WITH;
        }
        ed_box(E_STRIP1 + i, i * 40u, Y_E_STRIP1, 40, 19, l, F_S, 2, on ? C_BLACK : C_GREY, on ? C_WHITE : C_BLACK,
               C_DARK, 1);
        on = !knob && ed.light == LIGHT_OFF + i;
        ed_box(E_STRIP2 + i, i * 40u, Y_E_STRIP2, 40, 19, knob ? "" : LIGHT_L[i], F_S, 2, on ? C_BLACK : C_GREY,
               on ? C_WHITE : C_BLACK, C_DARK, 1);
    }
    /* the category line: its name and page on the left, the mode (if not WITH) on the right */
    p = ui_put(b, c->name);
    if (ed_pages() > 1u) {
        p = ui_num(ui_put(p, " "), ed.top / (ED_ROWS - 1u) + 1u);
        p = ui_num(ui_put(p, "/"), ed_pages());
    }
    ed_box(E_CAT, 0, Y_E_CAT, 160, 18, b, F_S, 2, C_YELLOW, C_BLACK, 0, 0);
    ed_box(E_MSG, 160, Y_E_CAT, 80, 18, ed.mode == M_WITH ? "" : M_NAMES[ed.mode], F_S, 2, C_BLACK,
           ed.mode == M_WITH ? C_BLACK : C_CYAN, 0, 1);
    /* the grid (another category: its geometry changes, so all of it again) */
    if (ed.drawn_cat != ed.cat) {
        ed.drawn_cat = ed.cat;
        gfx_fill(0, Y_E_GRID, GFX_W, ED_ROWS * E_CELL_H, C_BLACK);
        for (i = 0; i < ED_ROWS * 6u; i++)
            ui_reg[E_GRID + i].valid = 0;
    }
    for (i = 0; i < ED_ROWS * 6u; i++) {
        uint32_t row = i / 6u, col = i % 6u, idx = (ed.top + row) * c->cols + col, w = GFX_W / c->cols;
        int grey = ed.mode == M_ONESHOT;
        if (col >= c->cols)
            continue;
        if (idx >= c->n) {
            ed_box(E_GRID + i, col * w, Y_E_GRID + row * E_CELL_H, w, E_CELL_H, "", F_S, 2, C_BLACK, C_BLACK, 0, 1);
            continue;
        }
        {
            uint16_t it = c->items[idx];
            int sel = idx == ed.cur;
            if (ed.cat == C_SYM) {
                b[0] = SYM_CHARS[idx];
                b[1] = 0;
            } else if (ed.cat == C_EMPTY)
                ui_put(b, it == KC_TRNS ? "SAME" : it == KC_NO ? "NONE" : ed.has_keep ? "KEEP" : "");
            else if (ed.cat == C_OTHER && idx < sizeof OTHER_S / sizeof OTHER_S[0])
                ui_put(b, OTHER_S[idx]);
            else if (ed.cat == C_OTHER && it >= 0x87u && it <= 0x8Fu)
                ui_num(ui_put(b, "INT"), it - 0x86u);
            else if (ed.cat == C_OTHER && it >= 0x90u && it <= 0x98u)
                ui_num(ui_put(b, "LNG"), it - 0x8Fu);
            else if (ed.cat == C_OTHER && it >= 0x82u && it <= 0x84u)
                ui_put(b, it == 0x82u ? "LCAP" : it == 0x83u ? "LNUM" : "LSCR");
            else if (ed.cat == C_LAYERS) {
                static const char *const K[4] = {"H", "1S", "GO", "TG"};
                ui_num(ui_put(b, K[idx / 5u]), idx % 5u);
            } else
                kc_label(it, b, 1);
            ed_box(E_GRID + i, col * w, Y_E_GRID + row * E_CELL_H, w, E_CELL_H, b, F_S, 2,
                   sel ? C_BLACK : grey ? C_DARK : C_WHITE, sel ? (bad ? C_RED : C_WHITE) : C_BLACK,
                   sel ? C_WHITE : C_DARK, 1);
        }
    }
    /* the description: the value in words, or why it cannot be saved; a message over it for a while */
    if (ed.msg && now - ed.msg_t < ED_MSG_MS)
        ed_box(E_DESC, 0, Y_E_DESC, GFX_W, 18, ed.msg, F_S, 2, ed.msg_c, C_BLACK, 0, 0);
    else if (bad)
        ed_box(E_DESC, 0, Y_E_DESC, GFX_W, 18, err, F_S, 2, C_RED, C_BLACK, 0, 0);
    else if (ed.has_keep && ed_item() == ED_KEEP) {
        p = ui_put(b, "IN VIAL: ");         /* (set in Vial, not editable here: KEEP keeps it) */
        kc_label(kc, p, 0);
        ed_box(E_DESC, 0, Y_E_DESC, GFX_W, 18, b, F_S, 2, C_YELLOW, C_BLACK, 0, 0);
    } else {
        kc_label(kc, b, 0);
        if (kc >= QK_MACRO && kc <= QK_MACRO_MAX) {   /* a macro: its first characters */
            uint32_t id = kc - QK_MACRO, o = 0, n;
            while (id && o < KB_MACRO_BYTES)
                if (!cfg.macros[o++])
                    id--;
            p = b + gfx_len(b);
            *p++ = ' ';
            *p++ = '"';
            for (n = 0; n < 10u && o < KB_MACRO_BYTES && cfg.macros[o]; o++)
                if (cfg.macros[o] >= 0x20u && cfg.macros[o] < 0x7Fu) {
                    *p++ = (char)cfg.macros[o];
                    n++;
                }
            *p++ = '"';
            *p = 0;
            if (n == 0u)
                ui_put(b + gfx_len(b) - 2u, " (EMPTY)");
        }
        ed_box(E_DESC, 0, Y_E_DESC, GFX_W, 18, b, F_S, 2, C_WHITE, C_BLACK, 0, 0);
    }
    ed_box(E_HINT, 0, Y_E_HINT, GFX_W, 18, bad ? "OCT- CANCEL" : "OCT- CANCEL  OCT+ SAVE", F_S, 2, C_GREY, C_BLACK, 0, 1);
}

static void ed_draw_choose(uint32_t now)
{
    char b[32], *p;
    int pick = ed.st == ED_PICK2;
    int32_t shown = ed.ptr >= 0 ? ed.ptr : pick ? ed.src : -1;
    /* the bar: the layer, the action (reversed if not EDIT) */
    p = ui_num(ui_put(b, "LAYER "), ed.layer);
    ed_box(E_HEAD, 0, Y_E_HEAD, 110, 20, b, F_S, 2, C_WHITE, C_BLACK, 0, 0);
    if (pick)
        ui_put(b, ed.action == A_SWAP ? "SWAP WITH" : "PASTE TO");
    else
        ui_put(b, A_NAMES[ed.action]);
    ed_box(E_STRIP1, 110, Y_E_HEAD, 130, 20, b, F_S, 2, ed.action == A_EDIT ? C_CYAN : C_BLACK,
           ed.action == A_EDIT ? C_BLACK : C_YELLOW, 0, 0);
    /* the layer's map, the control pointed at highlighted (its page) */
    ui_view = ed.layer;
    if (shown >= 0)
        un.page = shown >= (int32_t)KB_NBTN && shown < (int32_t)KB_NPOS ? PG_KEYS : PG_PANEL;
    if (un.page != un.drawn_page) {
        uint32_t i;
        un.drawn_page = un.page;
        gfx_fill(0, Y_PAGE, GFX_W, Y_DETAIL - Y_PAGE, C_BLACK);
        for (i = R_CELL0; i < R_DETAIL_POS; i++)
            ui_reg[i].valid = 0;
    }
    ui_box(R_PAGE, 0, Y_PAGE, GFX_W, 16, un.page == PG_PANEL ? "PANEL" : "KEYS", F_S, 2, C_GREY, C_BLACK, 0, 0);
    if (un.page == PG_PANEL)
        ui_page_panel(ed.ptr);
    else
        ui_page_keys(ed.ptr);
    ui_view = -1;
    /* the detail: the control pointed at, its binding on this layer */
    if (pick) {
        p = ui_put(b, "FROM ");
        pos_label(ed.src, p);
    } else if (ed.ptr >= 0)
        pos_label(ed.ptr, b);
    else
        ui_put(b, "PRESS A CONTROL");
    ui_box(R_DETAIL_POS, 0, Y_DETAIL, GFX_W, 18, b, F_S, 2, !pick && ed.ptr < 0 ? C_YELLOW : C_GREY, C_BLACK, 0, 0);
    if (ed.ptr >= 0)
        kc_label(t_get(ed.layer, ed.ptr), b, 0);
    else
        b[0] = 0;
    ui_box(R_DETAIL_KC, 0, Y_DETAIL + 20u, GFX_W, 26, b, F_L, 2, C_WHITE, C_BLACK, 0, 0);
    /* the bottom: a message, or the hint */
    if (vl.in_progress)
        ed_box(E_MSG, 0, 206, GFX_W, 16, "EXIT EDITOR TO UNLOCK", F_S, 2, C_YELLOW, C_BLACK, 0, 1);
    else if (ed.msg && now - ed.msg_t < ED_MSG_MS)
        ed_box(E_MSG, 0, 206, GFX_W, 16, ed.msg, F_S, 2, ed.msg_c, C_BLACK, 0, 1);
    else
        ed_box(E_MSG, 0, 206, GFX_W, 16, "", F_S, 2, C_BLACK, C_BLACK, 0, 1);
    ed_box(E_HINT, 0, Y_E_HINT, GFX_W, 18, pick ? "OCT- BACK  OCT+ DO IT" :
           ed.action == A_EDIT ? "OCT- EXIT  OCT+ EDIT" : "OCT- EXIT  OCT+ DO IT", F_S, 2, C_GREY, C_BLACK, 0, 1);
}

static void ed_draw_panel(const char *title, const char *l1, const char *l2, const char *l3, const char *hint)
{
    ed_box(E_HEAD, 0, Y_E_HEAD, GFX_W, 20, title, F_S, 2, C_BLACK, C_YELLOW, 0, 0);
    ed_box(E_CAT, 0, 60, GFX_W, 26, l1, F_L, 2, C_WHITE, C_BLACK, 0, 1);
    ed_box(E_DESC, 0, 100, GFX_W, 26, l2, F_L, 2, C_WHITE, C_BLACK, 0, 1);
    ed_box(E_MSG, 0, 140, GFX_W, 18, l3, F_S, 2, C_GREY, C_BLACK, 0, 1);
    ed_box(E_HINT, 0, Y_E_HINT, GFX_W, 18, hint, F_S, 2, C_GREY, C_BLACK, 0, 1);
}

static void ed_draw(uint32_t now, const char *fw_id, uint32_t saves, uint32_t slot)
{
    static uint8_t drawn_st = 0xFF;
    char b1[24], b2[24], b3[32];
    uint32_t st_kind = ed.st == ED_EDIT ? 1u : ed.st == ED_CHOOSE || ed.st == ED_PICK2 ? 2u : 3u;
    if (ed.redraw || drawn_st != st_kind) {
        ed.redraw = 0;
        drawn_st = (uint8_t)st_kind;
        gfx_fill(0, 0, GFX_W, GFX_H, C_BLACK);
        ed_invalidate();
        un.drawn_page = 0xFF;
        ed.drawn_cat = 0xFF;
    }
    switch (ed.st) {
    case ED_EDIT:
        ed_draw_edit(now);
        break;
    case ED_CHOOSE:
    case ED_PICK2:
        ed_draw_choose(now);
        break;
    case ED_CONFIRM:
        ui_num(ui_put(b1, "LAYER "), ed.layer);
        ed_draw_panel("CLEAR LAYER", b1, "ALL TO SAME?", "", "OCT- BACK  OCT+ CLEAR");
        break;
    case ED_COPYL:
        ui_num(ui_put(b1, "FROM LAYER "), ed.layer);
        ui_num(ui_put(b2, "TO LAYER "), ed.dest);
        ed_draw_panel("COPY LAYER", b1, b2, "PRESETS: THE TARGET", "OCT- BACK  OCT+ COPY");
        break;
    case ED_INFO:
        ui_num(ui_put(b2, "SAVES "), saves);
        ui_put(ui_num(ui_put(b3, "CONFIG "), (uint32_t)CFG_PERSIST_BYTES), slot == 1u ? " B, SLOT A" :
                                                                            slot == 2u ? " B, SLOT B" : " B");
        ed_draw_panel("INFO", fw_id, b2, b3, "OCT- / OCT+ BACK");
        break;
    case ED_LEDTEST:
        ed_draw_panel("LED TEST", "ALL LIT", "", "", "OCT- / OCT+ BACK");
        break;
    }
}
