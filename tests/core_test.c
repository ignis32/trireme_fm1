/* Host test of firmware/kbcore.c: the engine (layers, transparency, mods, a layer change while a key is held,
 * encoders, the OCT chord, rollover, mute) and the Vial / VIA command handler, request by request, against
 * vial-qmk's via.c / vial.c behaviour. Exit status 1 on a failure. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t t_ms;
static int t_ready = 1, t_uboot;
static uint8_t t_q[256][8];
static uint32_t t_qn, t_room = 16;
static uint32_t kb_now(void) { return t_ms; }
static int kb_out_ready(void) { return t_ready; }
static uint32_t kb_out_room(void) { return t_room; }
static int kb_out_push(const uint8_t r[8])
{
    if (!t_room)
        return 0;
    memcpy(t_q[t_qn++ % 256], r, 8);
    return 1;
}
static void kb_diag(uint8_t *out) { (void)out; }
static int t_media = 1;                     /* the media interface there */
static uint8_t t_mq[512][8], t_mqn[512];
static uint32_t t_mn, t_mroom = 16;
static int kb_media_ok(void) { return t_media && t_ready; }
static uint32_t kb_media_room(void) { return t_mroom; }
static int kb_media_push(const uint8_t *r, uint32_t n)
{
    if (!t_mroom)
        return 0;
    memcpy(t_mq[t_mn % 512], r, n);
    t_mqn[t_mn % 512] = (uint8_t)n;
    t_mn++;
    return 1;
}
static void kb_uboot(void) { t_uboot++; }
#include "../firmware/kbcore.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int32_t NOENC[KB_NENC];
static uint32_t btn, notes;
static void pass(void)                       /* one main-loop pass with the current levels */
{
    kb_input(btn, notes, 0, 0, NOENC);
    kb_task();
}
static void step(uint32_t ms)                /* time passes, passes run */
{
    while (ms--) {
        t_ms++;
        pass();
    }
}
static const uint8_t *last(void) { return t_q[(t_qn - 1u) % 256]; }
static int has_key(const uint8_t *r, uint8_t k)
{
    int i;
    for (i = 2; i < 8; i++)
        if (r[i] == k)
            return 1;
    return 0;
}
static void set_key(uint32_t layer, uint32_t pos, uint16_t kc)
{
    uint8_t r, c;
    pos_rowcol(pos, &r, &c);
    put_be16(&cfg.keymap[km_ofs(layer, r, c)], kc);
}
#define NOTE(n) (KB_NBTN + (n))
#define NB(n) (1u << (n))

static void test_map(void)
{
    uint32_t p, seen[KB_ROWS][KB_COLS] = {{0}};
    for (p = 0; p < KB_NPOS; p++) {
        uint8_t r, c;
        pos_rowcol(p, &r, &c);
        CHECK(r < KB_ROWS && c < KB_COLS && !seen[r][c]++, "pos %u -> %u,%u", p, r, c);
        CHECK(rowcol_pos(r, c) == (int32_t)p, "rowcol_pos %u,%u", r, c);
    }
    {   /* F3 white col 0, F#3 black col 0, G5 (note 26) white col 15, F#5 (note 25) black col 10 */
        uint8_t r, c;
        pos_rowcol(NOTE(0), &r, &c);
        CHECK(r == 2 && c == 0, "F3 %u,%u", r, c);
        pos_rowcol(NOTE(1), &r, &c);
        CHECK(r == 1 && c == 0, "F#3 %u,%u", r, c);
        pos_rowcol(NOTE(26), &r, &c);
        CHECK(r == 2 && c == 15, "G5 %u,%u", r, c);
        pos_rowcol(NOTE(25), &r, &c);
        CHECK(r == 1 && c == 10, "F#5 %u,%u", r, c);
    }
}

static void test_keys(void)
{
    uint32_t q0;
    kb_init();
    btn = notes = 0;
    pass();
    CHECK(t_qn >= 1 && !memcmp(last(), "\0\0\0\0\0\0\0\0", 8), "first report: empty");
    notes = NB(0);                           /* F3: A */
    pass();
    CHECK(has_key(last(), KC_A), "A down");
    notes = 0;
    pass();
    CHECK(!has_key(last(), KC_A), "A up");
    /* a tap shorter than a pass: press edge without level */
    q0 = t_qn;
    kb_input(0, 0, 0, NB(1), NOENC);
    CHECK(t_qn == q0 + 1 && has_key(last(), KC_B), "short tap: B down");
    pass();
    CHECK(!has_key(last(), KC_B), "short tap: B up next pass");
    /* mods: LCTL(KC_C) on note 2 */
    set_key(0, NOTE(2), LCTL(KC_C));
    notes = NB(2);
    pass();
    CHECK(last()[0] == 0x01 && has_key(last(), KC_C), "LCTL(C): %02x", last()[0]);
    notes = 0;
    pass();
    CHECK(last()[0] == 0 && !has_key(last(), KC_C), "LCTL(C) released");
    /* rollover: 7 keys */
    notes = 0x7Fu;
    pass();
    CHECK(last()[2] == 0x01 && last()[7] == 0x01, "7 keys: ErrorRollOver");
    notes = 0;
    pass();
    CHECK(last()[2] == 0, "rollover released");
}

static void test_layers(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    set_key(0, NOTE(3), MO(4));
    set_key(4, NOTE(0), KC_X);               /* layer 4: X on F3, the rest KC_TRNS (SAME) */
    notes = NB(0);
    pass();
    CHECK(has_key(last(), KC_A), "base: A");
    notes = 0;
    pass();
    notes = NB(3);                           /* hold MO(4) */
    pass();
    CHECK(kb.ls_n == 1 && layer_top() == 4, "MO(4) on: top %u", layer_top());
    notes = NB(3) | NB(0);
    pass();
    CHECK(has_key(last(), KC_X), "layer 4: X");
    notes = NB(3) | NB(0) | NB(4);           /* note 4 is SAME on layer 4: the base's A..E -> E */
    pass();
    CHECK(has_key(last(), KC_E), "SAME falls through: E");
    notes = NB(0) | NB(4);                   /* MO released while X held: X stays X until released */
    pass();
    CHECK(kb.ls_n == 0 && has_key(last(), KC_X), "held key keeps its keycode");
    notes = 0;
    pass();
    CHECK(!has_key(last(), KC_X) && !has_key(last(), KC_E), "all up");
    /* the base: PRESETS (fixed, not through the keymap) */
    {
        int32_t enc[KB_NENC] = {0};
        enc[ENC_PRESETS] = 1;
        kb_input(0, 0, 0, 0, enc);
        CHECK(cfg.layer == 1, "PRESETS CW: layer %u", cfg.layer);
        enc[ENC_PRESETS] = -2;
        kb_input(0, 0, 0, 0, enc);
        CHECK(cfg.layer == KB_LAYERS - 1u, "PRESETS CCW x2 wraps: layer %u", cfg.layer);
        notes = NB(0);                       /* layer 4's F3: X; its note 1: SAME on the base = nothing */
        pass();
        CHECK(has_key(last(), KC_X), "base 4: X");
        notes = NB(1);
        pass();
        CHECK(!has_key(last(), KC_B) && !has_key(last(), KC_X), "SAME on the base: nothing");
        notes = 0;
        pass();
        /* SELECT / ALGORITHM: fixed, the UI's; nothing typed */
        enc[ENC_PRESETS] = 0;
        enc[ENC_SELECT] = 2;
        enc[ENC_ALGORITHM] = -1;
        {
            uint32_t q0 = t_qn;
            kb_input(0, 0, 0, 0, enc);
            CHECK(kb.sel_steps == 2 && kb.alg_steps == -1 && t_qn == q0, "SELECT / ALGORITHM: to the UI");
        }
    }
    /* the layer switched to wins, whatever its number: base 3, MO(1) held -> layer 1's keys */
    kb_init();
    pass();
    cfg.layer = 3;
    set_key(3, NOTE(5), MO(1));
    set_key(3, NOTE(0), KC_Q);
    set_key(1, NOTE(0), KC_W);
    notes = NB(5);
    pass();
    notes = NB(5) | NB(0);
    pass();
    CHECK(layer_top() == 1 && has_key(last(), KC_W), "HOLD 1 over base 3: W");
    notes = 0;
    pass();
    notes = NB(0);
    pass();
    CHECK(has_key(last(), KC_Q), "released: base 3, Q");
    notes = 0;
    pass();
}

/* TG: the same control takes you back (whatever it is on the toggled layer), no toggle inside a toggle, a base
 * change ends it, HOLD works on top */
static void test_toggle(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    set_key(0, NOTE(6), TG(2));
    set_key(0, NOTE(7), TG(3));
    set_key(2, NOTE(6), KC_Z);               /* on layer 2 the toggle's own key is Z: still toggles back */
    set_key(2, NOTE(7), TG(3));              /* another toggle on layer 2: ignored while toggled */
    set_key(2, NOTE(8), MO(4));
    set_key(2, NOTE(0), KC_Y);
    set_key(4, NOTE(0), KC_X);
    notes = NB(6);
    pass();
    notes = 0;
    pass();
    CHECK(layer_top() == 2 && kb.tg_layer == 2, "TG 2: on (top %u)", layer_top());
    notes = NB(0);
    pass();
    CHECK(has_key(last(), KC_Y), "toggled: layer 2's Y");
    notes = 0;
    pass();
    notes = NB(7);                           /* TG 3 while toggled: ignored */
    pass();
    notes = 0;
    pass();
    CHECK(layer_top() == 2 && kb.ls_n == 1, "TG 3 inside the toggle: ignored (top %u, %u on)", layer_top(), kb.ls_n);
    notes = NB(8);                           /* HOLD 4 on top of the toggle */
    pass();
    notes = NB(8) | NB(0);
    pass();
    CHECK(layer_top() == 4 && has_key(last(), KC_X), "HOLD 4 over the toggle: X");
    notes = 0;
    pass();
    CHECK(layer_top() == 2, "HOLD released: back on the toggled layer");
    notes = NB(6);                           /* the same control: back (not Z) */
    pass();
    CHECK(!has_key(last(), KC_Z), "the toggle's key: no Z");
    notes = 0;
    pass();
    CHECK(kb.ls_n == 0 && kb.tg_layer == POS_NONE, "TG again: off");
    /* a base change ends the toggle */
    notes = NB(6);
    pass();
    notes = 0;
    pass();
    {
        int32_t enc[KB_NENC] = {0};
        enc[ENC_PRESETS] = 1;
        kb_input(0, 0, 0, 0, enc);
    }
    CHECK(cfg.layer == 1 && kb.ls_n == 0 && kb.tg_layer == POS_NONE, "PRESETS ends the toggle");
    /* DF (GO TO): the base, stays */
    set_key(1, NOTE(9), DF(3));
    notes = NB(9);
    pass();
    notes = 0;
    pass();
    CHECK(cfg.layer == 3, "GO TO 3: base %u", cfg.layer);
}

static void test_encoders(void)
{
    int32_t enc[KB_NENC] = {0};
    uint32_t q0;
    kb_init();
    pass();
    q0 = t_qn;
    enc[ENC_KNOB1 + 2] = 3;                  /* KNOB3 CW = Right, three detents: 3 x (down, up) */
    kb_input(0, 0, 0, 0, enc);
    CHECK(t_qn == q0 + 6, "3 detents -> 6 reports, got %u", t_qn - q0);
    CHECK(has_key(t_q[q0 % 256], KC_RGHT) && !has_key(t_q[(q0 + 1) % 256], KC_RGHT), "detent: Right down, up");
    /* a busy queue: the taps wait */
    t_room = 1;
    q0 = t_qn;
    enc[ENC_KNOB1 + 2] = -1;
    kb_input(0, 0, 0, 0, enc);
    CHECK(t_qn == q0 && kb.aq_r != kb.aq_w, "room 1: the tap waits");
    t_room = 16;
    kb_task();
    CHECK(t_qn == q0 + 2 && has_key(t_q[q0 % 256], KC_LEFT), "then goes: Left");
}

static void test_chord(void)
{
    uint32_t q0, i;
    kb_init();
    btn = notes = 0;
    pass();
    /* OCT- alone: nothing (free) */
    q0 = t_qn;
    btn = NB(0);
    step(150);
    btn = 0;
    step(5);
    CHECK(cfg.layer == 0 && kb.editor_req == 0 && t_qn == q0, "OCT-: nothing (layer %u)", cfg.layer);
    /* OCT+ tapped: the editor; held past the window: the editor once */
    btn = NB(1);
    step(20);
    btn = 0;
    step(5);
    CHECK(kb.editor_req == 1 && cfg.layer == 0, "OCT+ tap: editor (%u)", kb.editor_req);
    btn = NB(1);
    step(300);
    btn = 0;
    step(5);
    CHECK(kb.editor_req == 2, "OCT+ held: editor once (%u)", kb.editor_req);
    /* the chord, tapped: nothing; held 5 s: UBOOT */
    t_uboot = 0;
    btn = NB(0);
    step(30);
    btn = NB(0) | NB(1);
    step(300);
    btn = 0;
    step(5);
    CHECK(kb.editor_req == 2 && !t_uboot, "chord tap: nothing (editor %u)", kb.editor_req);
    btn = NB(0) | NB(1);
    for (i = 0; i < 49u; i++)
        step(100);
    CHECK(!t_uboot, "chord 4.9 s: not yet");
    step(200);
    CHECK(t_uboot, "chord 5 s: UBOOT");
    btn = 0;
    step(5);
    /* OCT positions in the keymap are ignored: a keycode there does nothing */
    set_key(0, POS_OCTDN, KC_Q);
    q0 = t_qn;
    btn = NB(0);
    step(20);
    btn = 0;
    step(5);
    CHECK(t_qn == q0, "OCT- with Q in the keymap: no Q");
}

static void cmd(uint8_t *m, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    memset(m, 0, 32);
    m[0] = a;
    m[1] = b;
    m[2] = c;
    m[3] = d;
}

static void test_vial(void)
{
    uint8_t m[32];
    uint32_t i;
    kb_init();
    cmd(m, 0x01, 0, 0, 0);
    vial_cmd(m);
    CHECK(m[0] == 1 && m[1] == 0 && m[2] == 9, "VIA protocol 9");
    cmd(m, 0xFE, 0x00, 0, 0);
    vial_cmd(m);
    CHECK(m[0] == 6 && m[1] == 0 && !memcmp(&m[4], VIAL_UID, 8), "Vial protocol 6 + UID");
    cmd(m, 0xFE, 0x01, 0, 0);
    vial_cmd(m);
    CHECK((m[0] | m[1] << 8) == sizeof VIAL_DEF, "definition size");
    {   /* the definition, page by page, decompresses in the Python test; here: the bytes match */
        uint32_t pg, ok = 1;
        for (pg = 0; pg * 32u < sizeof VIAL_DEF; pg++) {
            cmd(m, 0xFE, 0x02, (uint8_t)pg, (uint8_t)(pg >> 8));
            vial_cmd(m);
            for (i = 0; i < 32u && pg * 32u + i < sizeof VIAL_DEF; i++)
                ok &= m[i] == VIAL_DEF[pg * 32u + i];
        }
        CHECK(ok, "definition pages");
    }
    cmd(m, 0x11, 0, 0, 0);
    vial_cmd(m);
    CHECK(m[1] == KB_LAYERS, "layer count");
    cmd(m, 0x04, 0, 2, 0);                   /* layer 0, row 2, col 0 = F3 = A */
    vial_cmd(m);
    CHECK(m[4] == 0 && m[5] == KC_A, "get keycode F3: %02x%02x", m[4], m[5]);
    cmd(m, 0x05, 0, 2, 0);
    m[4] = 0x01;                             /* LCTL(KC_Z)? no: 0x011D = LCTL(KC_Z) */
    m[5] = 0x1D;
    vial_cmd(m);
    CHECK(km_get(0, 2, 0) == 0x011D, "set keycode");
    cmd(m, 0x12, 0, 0, 28);                  /* keymap buffer: the first 28 bytes */
    vial_cmd(m);
    CHECK(m[4] == 0x7E && m[5] == 0x00, "buffer[0] = row 0 col 0 = OCT- = PRESET-: %02x%02x", m[4], m[5]);
    /* buffer set of QK_BOOT while locked: refused */
    cmd(m, 0x13, 0, 4, 2);
    m[4] = 0x7C;
    m[5] = 0x00;
    vial_cmd(m);
    CHECK(be16(&cfg.keymap[4]) == KC_NO, "QK_BOOT kept out while locked: %04x", be16(&cfg.keymap[4]));
    /* encoders */
    cmd(m, 0xFE, 0x03, 0, 2);                /* knob 2 = KNOB3 */
    vial_cmd(m);
    CHECK(be16(&m[0]) == KC_LEFT && be16(&m[2]) == KC_RGHT, "get encoder: %04x %04x", be16(&m[0]), be16(&m[2]));
    cmd(m, 0xFE, 0x04, 1, 3);                /* KNOB4 */
    m[4] = 1;                                /* CW */
    m[5] = 0x00;
    m[6] = KC_UP;
    vial_cmd(m);
    CHECK(enc_get(1, 3, 1) == KC_UP, "set encoder");
    /* macros */
    cmd(m, 0x0C, 0, 0, 0);
    vial_cmd(m);
    CHECK(m[1] == KB_MACROS, "macro count");
    cmd(m, 0x0D, 0, 0, 0);
    vial_cmd(m);
    CHECK((m[1] << 8 | m[2]) == KB_MACRO_BYTES, "macro size");
    cmd(m, 0x0F, 0, 0, 3);
    m[4] = 'h';
    vial_cmd(m);
    CHECK(cfg.macros[0] == 0, "macro set refused while locked");
    /* unlock: start, then polls >= 100 ms apart with OCT- + OCT+ held */
    cmd(m, 0xFE, 0x05, 0, 0);
    vial_cmd(m);
    CHECK(m[0] == 0 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[5] == 1 && m[6] == 0xFF,
          "unlock status: locked, keys (0,0) (0,1)");
    cmd(m, 0xFE, 0x06, 0, 0);
    vial_cmd(m);
    cmd(m, 0x04, 0, 2, 0);
    vial_cmd(m);
    CHECK(m[0] == 0x04 && m[4] == 0, "during unlock: other requests come back unchanged");
    /* 3 s held, our clock (polls every 200 ms, as the Vial app): the counter reports tenths left; a release resets */
    btn = NB(0) | NB(1);
    {
        uint32_t t0 = t_ms, first = 0, last = 99;
        for (i = 0; i < 100u && !vl.unlocked; i++) {
            t_ms += 50;
            kb_input(btn, 0, 0, 0, NOENC);
            if (i == 20u) {                 /* let go for a moment at 1 s: starts again */
                kb_input(0, 0, 0, 0, NOENC);
                CHECK(unlock_counter() == 30u, "released: counter back to 30 (%u)", unlock_counter());
                t0 = t_ms;
                kb_input(btn, 0, 0, 0, NOENC);
            }
            if (i % 4u == 0u) {
                cmd(m, 0xFE, 0x07, 0, 0);
                vial_cmd(m);
                if (!first)
                    first = m[2];
                last = m[2];
            }
        }
        CHECK(vl.unlocked && !vl.in_progress, "unlocked");
        CHECK(t_ms - t0 >= 3000u && t_ms - t0 <= 3050u, "unlock took %u ms after the last press", t_ms - t0);
        CHECK(first >= 29u && first <= 30u && last <= 2u, "counter %u .. %u", first, last);
    }
    btn = 0;
    kb_input(0, 0, 0, 0, NOENC);
    cmd(m, 0x0F, 0, 0, 3);
    memcpy(&m[4], "hi", 3);
    vial_cmd(m);
    CHECK(!memcmp(cfg.macros, "hi", 3), "macro set when unlocked");
    cmd(m, 0x0E, 0, 0, 3);
    vial_cmd(m);
    CHECK(!memcmp(&m[4], "hi", 3), "macro get");
    cmd(m, 0x02, 0x03, 0, 0);                /* key tester: F3 (row 2 col 0) */
    kb_input(0, NB(0), 0, 0, NOENC);
    vial_cmd(m);
    CHECK(m[2] == 0 && m[3] == 0 && m[4] == 0 && m[5] == 0 && m[6] == 0 && m[7] == 1, "matrix: F3 at row 2 col 0");
    kb_input(0, 0, 0, 0, NOENC);
    cmd(m, 0x0B, 0, 0, 0);
    CHECK(vial_cmd(m) == 1, "bootloader jump when unlocked");
    cmd(m, 0xFE, 0x08, 0, 0);
    vial_cmd(m);
    CHECK(!vl.unlocked, "lock");
    cmd(m, 0x0B, 0, 0, 0);
    CHECK(vial_cmd(m) == 0, "bootloader jump refused when locked");
    cmd(m, 0xFE, 0x0D, 0x00, 0);
    m[31] = 0x55;
    vial_cmd(m);
    CHECK(m[0] == KB_TDS && m[1] == KB_COMBOS && m[2] == 0 && m[3] == 0 && m[31] == 0,
          "dynamic entries: %u tap dances, %u combos, no overrides", m[0], m[1]);
    cmd(m, 0xFE, 0x09, 0, 0);                /* settings above qsid 0: 2, 6, 7, ... then FF FF */
    vial_cmd(m);
    CHECK(m[0] == 2 && m[1] == 0 && m[2] == 6 && m[4] == 7 && m[18] == 0xFF && m[19] == 0xFF, "QMK settings query");
    cmd(m, 0xFE, 0x0A, 7, 0);
    vial_cmd(m);
    CHECK(m[0] == 0 && (m[1] | m[2] << 8) == 200, "tapping term 200: %u", m[1] | m[2] << 8);
    cmd(m, 0xFE, 0x0B, 7, 0);
    m[4] = 150;
    vial_cmd(m);
    CHECK(m[0] == 0 && cfg.qs.tapping_term == 150, "tapping term set 150");
    cmd(m, 0xFE, 0x0B, 22, 0);
    m[4] = 1;
    vial_cmd(m);
    CHECK(cfg.qs.tapping_v2 == 1, "permissive hold on");
    cmd(m, 0xFE, 0x0A, 22, 0);
    vial_cmd(m);
    CHECK(m[0] == 0 && m[1] == 1, "permissive hold reads 1");
    cmd(m, 0xFE, 0x0A, 99, 0);
    vial_cmd(m);
    CHECK(m[0] == 0xFF, "an unknown qsid");
    cmd(m, 0xFE, 0x0C, 0, 0);
    vial_cmd(m);
    CHECK(cfg.qs.tapping_term == 200 && cfg.qs.tapping_v2 == 0, "settings reset");
    /* a tap dance entry: set, get */
    cmd(m, 0xFE, 0x0D, 0x02, 3);
    m[4] = KC_A;                             /* on_tap A, on_hold LCTL, on_double_tap B, -, term 180 */
    m[6] = KC_LCTL;
    m[8] = KC_B;
    m[12] = 180;
    vial_cmd(m);
    cmd(m, 0xFE, 0x0D, 0x01, 3);
    vial_cmd(m);
    CHECK(m[0] == 0 && m[1] == KC_A && m[3] == KC_LCTL && m[5] == KC_B && m[9] == 180, "tap dance 3 round trip");
    memset(cfg.td, 0, sizeof cfg.td);
    cmd(m, 0x08, 0, 0, 0);
    vial_cmd(m);
    CHECK(m[0] == 0xFF, "lighting get: unhandled");
    /* an unlock abandoned by the host times out */
    cmd(m, 0xFE, 0x06, 0, 0);
    vial_cmd(m);
    t_ms += 3500;
    kb_task();
    CHECK(!vl.in_progress, "abandoned unlock times out");
}

/* play until the macro ends (or limit ms), recording what the host gets: "+X" X down, "-X" up, as a string */
static char typed[512];
static uint32_t rec_from;                   /* the first report macro_play describes (set by macro_play, or before) */
static void macro_play_from(uint32_t q0, uint32_t limit);
static void macro_play(uint32_t limit) { macro_play_from(t_qn, limit); }
static void macro_play_from(uint32_t q0, uint32_t limit)
{
    uint32_t i, n = 0;
    uint8_t prev[8] = {0};
    while (kb.mac_ofs >= 0 && limit--) {
        t_ms++;
        kb_task();
    }
    typed[0] = 0;
    for (i = q0; i < t_qn; i++) {
        const uint8_t *r = t_q[i % 256];
        uint32_t k;
        for (k = 0; k < 8; k++)             /* every modifier: C S A G, then the right-hand ones c s a g */
            if ((r[0] ^ prev[0]) >> k & 1u)
                n += (uint32_t)snprintf(typed + n, sizeof typed - n, "%c%c ", r[0] >> k & 1u ? '+' : '-',
                                        "CSAGcsag"[k]);
        for (k = 2; k < 8; k++)
            if (r[k] && !has_key(prev, r[k]))
                n += (uint32_t)snprintf(typed + n, sizeof typed - n, "+%02x ", r[k]);
        for (k = 2; k < 8; k++)
            if (prev[k] && !has_key(r, prev[k]))
                n += (uint32_t)snprintf(typed + n, sizeof typed - n, "-%02x ", prev[k]);
        memcpy(prev, r, 8);
    }
}

static void test_macros(void)
{
    static const uint8_t BUF[] = "Hi\n\0"                         /* M0: H (shifted) i Enter */
                                 "\x01\x02\x04\x01\x04\x33\x01\x01\x03\x04\0"   /* M1: down A, wait 50 ms, up A */
                                 "\x01\x05\x06\xff\0"           /* M2: Vial tap 0xFF06 -> 0x0600 = LCTL|LSFT mods only? */
                                 "\x01\x05\x19\x01\0";          /* M3: Vial tap 0x0119 = LCTL(KC_V) */
    uint32_t t0;
    kb_init();
    btn = notes = 0;
    pass();
    memcpy(cfg.macros, BUF, sizeof BUF);
    set_key(0, NOTE(0), (uint16_t)(QK_MACRO + 0));
    rec_from = t_qn;
    notes = NB(0);
    pass();
    notes = 0;
    pass();
    macro_play_from(rec_from, 100);
    /* (Shift and its key go up in one report: valid HID; the description lists the modifier first) */
    CHECK(!strcmp(typed, "+S +0b -S -0b +0c -0c +28 -28 "), "M0 Hi\\n by its key: %s", typed);
    /* M1: the delay */
    macro_start(1);
    t0 = t_ms;
    while (kb.mac_ofs >= 0 && t_ms - t0 < 200u) {
        t_ms++;
        kb_task();
    }
    CHECK(t_ms - t0 >= 50u && t_ms - t0 < 55u, "M1 delay 50 ms: took %u", t_ms - t0);
    /* M3: a Vial 16-bit keycode */
    macro_start(3);
    macro_play(50);
    CHECK(!strcmp(typed, "+C +19 -C -19 "), "M3 LCTL(V): %s", typed);
    /* a busy queue: the macro waits, nothing is lost */
    macro_start(0);
    t_room = 0;
    kb_task();
    kb_task();
    CHECK(kb.mac_ofs >= 0, "room 0: the macro waits");
    t_room = 16;
    macro_play(100);
    CHECK(!strcmp(typed, "+S +0b -S -0b +0c -0c +28 -28 "), "M0 after waiting: %s", typed);
    /* a torn buffer (last byte not NUL): nothing plays */
    cfg.macros[KB_MACRO_BYTES - 1] = 'x';
    macro_start(0);
    CHECK(kb.mac_ofs < 0, "torn buffer: no macro");
    cfg.macros[KB_MACRO_BYTES - 1] = 0;
    /* an empty macro (M9): nothing */
    macro_start(9);
    CHECK(kb.mac_ofs < 0, "empty macro: nothing");
    /* a buffer write stops a running macro */
    {
        uint8_t m[32];
        vl.unlocked = 1;
        macro_start(1);
        CHECK(kb.mac_ofs >= 0, "M1 running");
        cmd(m, 0x0F, 0, 40, 1);
        m[4] = 0;
        vial_cmd(m);
        CHECK(kb.mac_ofs < 0, "a buffer write stops it");
        vl.unlocked = 0;
    }
    /* no host: the macro is dropped */
    macro_start(0);
    t_ready = 0;
    kb_task();
    CHECK(kb.mac_ofs < 0, "no host: dropped");
    t_ready = 1;
}

/* the mouse / system / consumer reports */
static void test_media(void)
{
    int32_t enc[KB_NENC] = {0};
    uint32_t m0, i, steps, sum;
    kb_init();
    btn = notes = 0;
    pass();
    m0 = t_mn;
    CHECK(m0 >= 3, "first pass: consumer, system, mouse sent in their idle state (%u)", m0);
    /* KNOB1 CW: Volume Up, pressed and released */
    m0 = t_mn;
    enc[ENC_KNOB1] = 1;
    kb_input(0, 0, 0, 0, enc);
    CHECK(t_mn == m0 + 2 && t_mq[m0 % 512][0] == 3 && t_mq[m0 % 512][1] == 0xE9 && t_mq[(m0 + 1) % 512][1] == 0,
          "KNOB1 CW: consumer 0x00E9 then 0 (%u reports)", t_mn - m0);
    /* KNOB2 CW: one wheel step down, exactly one (a detent) */
    m0 = t_mn;
    enc[ENC_KNOB1] = 0;
    enc[ENC_KNOB1 + 1] = 1;
    kb_input(0, 0, 0, 0, enc);
    for (i = 0; i < 500u; i++) {
        t_ms++;
        kb_task();
    }
    sum = 0;
    for (i = m0; i < t_mn; i++)
        if (t_mq[i % 512][0] == 1)
            sum += (uint32_t)(int8_t)t_mq[i % 512][4];
    CHECK((int32_t)sum == -1, "KNOB2 CW: one wheel step down (%d)", (int32_t)sum);
    /* a held wheel key: one at once, then from 300 ms one every 80 ms */
    enc[ENC_KNOB1 + 1] = 0;
    set_key(0, NOTE(5), KC_WH_U);
    m0 = t_mn;
    notes = NB(5);
    pass();
    for (i = 0; i < 1000u; i++)
        step(1);
    notes = 0;
    pass();
    sum = 0;
    for (i = m0; i < t_mn; i++)
        if (t_mq[i % 512][0] == 1)
            sum += (uint32_t)(int8_t)t_mq[i % 512][4];
    CHECK(sum >= 9u && sum <= 10u, "WH_U held 1 s: %u steps (1 + (1000-300)/80)", sum);
    /* a cursor key held: moves, faster over a second */
    set_key(0, NOTE(6), KC_MS_R);
    m0 = t_mn;
    notes = NB(6);
    pass();
    steps = 0;
    sum = 0;
    for (i = 0; i < 1100u; i++) {
        step(1);
    }
    notes = 0;
    pass();
    {
        int32_t first = 0, lastdx = 0;
        for (i = m0; i < t_mn; i++)
            if (t_mq[i % 512][0] == 1 && t_mq[i % 512][2]) {
                int32_t dx = (int8_t)t_mq[i % 512][2];
                if (!first)
                    first = dx;
                lastdx = dx;
                steps++;
                sum += (uint32_t)dx;
            }
        CHECK(first == MV_MIN && lastdx == MV_MAX && steps >= 60u && steps <= 72u,
              "MS_R held 1.1 s: %u steps, first %d, last %d px", steps, first, lastdx);
    }
    /* a mouse button */
    set_key(0, NOTE(7), KC_BTN1);
    notes = NB(7);
    pass();
    CHECK(t_mq[(t_mn - 1u) % 512][0] == 1 && t_mq[(t_mn - 1u) % 512][1] == 1, "BTN1 down");
    notes = 0;
    pass();
    CHECK(t_mq[(t_mn - 1u) % 512][1] == 0, "BTN1 up");
    /* system: Sleep */
    set_key(0, NOTE(8), KC_SLEP);
    notes = NB(8);
    pass();
    CHECK(t_mq[(t_mn - 1u) % 512][0] == 2 && t_mq[(t_mn - 1u) % 512][1] == 0x82, "SLEP: system 0x82");
    notes = 0;
    pass();
    CHECK(t_mq[(t_mn - 1u) % 512][0] == 2 && t_mq[(t_mn - 1u) % 512][1] == 0, "SLEP released");
    /* KB OFF mutes media too */
    kb.off = 1;
    set_key(0, NOTE(9), KC_MPLY);
    m0 = t_mn;
    notes = NB(9);
    pass();
    CHECK(t_mn == m0, "muted: no play / pause (%u reports)", t_mn - m0);
    notes = 0;
    pass();
    kb.off = 0;
    /* no media interface (the console flavour): nothing, and no harm */
    t_media = 0;
    m0 = t_mn;
    enc[ENC_KNOB1] = 3;
    kb_input(0, 0, 0, 0, enc);
    kb_task();
    CHECK(t_mn == m0 && kb.aq_r == kb.aq_w, "no media interface: taps dropped (%u)", t_mn - m0);
    t_media = 1;
}

/* a pass every ms for ms milliseconds, recording the keyboard reports from now */
static uint32_t rec0;
static void rec(void) { rec0 = t_qn; }
static const char *got(void)
{
    macro_play_from(rec0, 0);                /* (macro_play_from only describes, with limit 0) */
    return typed;
}
static void hold_ms(uint32_t n) { step(n); }
static void at(uint32_t notes_now, uint32_t ms_after) { notes = notes_now; pass(); step(ms_after); }

static void test_tapping(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    set_key(0, NOTE(10), MT(MOD_BIT_SFT, KC_A));   /* note 10: Shift when held, A when tapped */
    set_key(0, NOTE(11), LT(4, KC_B));             /* note 11: layer 4 when held, B when tapped */
    set_key(4, NOTE(0), KC_X);                     /* layer 4: X on F3 */
    /* MT tapped */
    rec();
    at(NB(10), 50);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 -04 "), "MT tap: %s", got());
    /* MT held past the term, then a key: Shift + that key (300 ms on: not a quick tap of the last) */
    step(300);
    rec();
    at(NB(10), 250);
    at(NB(10) | NB(2), 10);
    at(NB(10), 10);
    at(0, 5);
    CHECK(!strcmp(got(), "+S +06 -06 -S "), "MT hold: %s", got());
    /* rolling within the term (default: no permissive hold): the tap, then the other key */
    step(300);
    rec();
    at(NB(10), 30);
    at(NB(10) | NB(2), 30);
    at(NB(10), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 +06 -06 -04 "), "MT roll: %s", got());
    /* permissive hold: the same roll gives Shift + C */
    cfg.qs.tapping_v2 = 1;
    step(300);
    rec();
    at(NB(10), 30);
    at(NB(10) | NB(2), 30);
    at(NB(10), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+S +06 -06 -S "), "MT permissive hold: %s", got());
    /* hold on other key press: a press within the term is enough */
    cfg.qs.tapping_v2 = 2;
    step(300);
    rec();
    at(NB(10), 30);
    at(NB(10) | NB(2), 5);
    CHECK(!strcmp(got(), "+S +06 "), "MT hold on other key: %s", got());
    at(NB(10), 5);
    at(0, 5);
    cfg.qs.tapping_v2 = 0;
    /* quick tap: tapped, then held again soon: A held (no Shift) */
    step(300);
    rec();
    at(NB(10), 30);
    at(0, 50);
    at(NB(10), 400);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 -04 +04 -04 "), "MT quick tap: %s", got());
    /* retro tapping: held alone past the term, let go: Shift, then A */
    step(300);
    cfg.qs.tapping_v2 = 4;
    rec();
    at(NB(10), 300);
    at(0, 5);
    CHECK(!strcmp(got(), "+S -S +04 -04 "), "MT retro tap: %s", got());
    cfg.qs.tapping_v2 = 0;
    /* LT held: layer 4 for the key pressed meanwhile; tapped: B */
    step(300);
    rec();
    at(NB(11), 250);
    at(NB(11) | NB(0), 10);
    at(NB(11), 10);
    at(0, 5);
    CHECK(!strcmp(got(), "+1b -1b ") && kb.ls_n == 0, "LT hold: %s (%u on)", got(), kb.ls_n);
    rec();
    at(NB(11), 50);
    at(0, 5);
    CHECK(!strcmp(got(), "+05 -05 "), "LT tap: %s", got());
    /* LT roll without permissive hold: B, then the key on the base layer */
    step(300);
    rec();
    at(NB(11), 30);
    at(NB(11) | NB(0), 30);
    at(NB(11), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+05 +04 -04 -05 "), "LT roll: %s", got());
}

static void test_oneshot_tt_lm(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    set_key(0, NOTE(12), OSM(MOD_BIT_SFT));
    set_key(0, NOTE(13), OSL(4));
    set_key(0, NOTE(14), TT(3));
    set_key(0, NOTE(15), LM(4, MOD_BIT_CTL));
    set_key(4, NOTE(0), KC_X);
    set_key(3, NOTE(0), KC_Y);
    /* OSM tapped: the next key is shifted, the one after is not */
    rec();
    at(NB(12), 30);
    at(0, 30);
    at(NB(0), 30);
    at(0, 30);
    at(NB(0), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+S +04 -S -04 +04 -04 "), "OSM: %s", got());
    /* OSM held while another key: an ordinary Shift, nothing left over */
    rec();
    at(NB(12), 30);
    at(NB(12) | NB(0), 30);
    at(NB(12), 30);
    at(0, 30);
    at(NB(0), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+S +04 -04 -S +04 -04 "), "OSM held: %s", got());
    /* OSL tapped: the next key from layer 4, then back */
    rec();
    at(NB(13), 30);
    at(0, 30);
    at(NB(0), 30);
    at(0, 30);
    at(NB(0), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+1b -1b +04 -04 ") && kb.ls_n == 0, "OSL: %s", got());
    /* TT held: layer 3 while held; tapped 5 times: toggled on */
    rec();
    at(NB(14), 250);
    at(NB(14) | NB(0), 10);
    at(NB(14), 10);
    at(0, 5);
    CHECK(!strcmp(got(), "+1c -1c ") && kb.ls_n == 0, "TT hold: %s", got());
    {
        uint32_t i;
        for (i = 0; i < 5u; i++) {
            at(NB(14), 20);
            at(0, 20);
        }
        CHECK(kb.tg_layer == 3 && layer_top() == 3, "TT x5: layer 3 toggled on (top %u)", layer_top());
        at(NB(14), 20);                      /* the toggle's own control: back at the first tap */
        at(0, 20);
        CHECK(kb.ls_n == 0 && kb.tg_layer == POS_NONE, "TT toggled: one tap back (%u on)", kb.ls_n);
    }
    /* LM held: layer 4 with Ctrl */
    rec();
    at(NB(15), 10);
    at(NB(15) | NB(0), 10);
    at(NB(15), 10);
    at(0, 5);
    CHECK(!strcmp(got(), "+C +1b -1b -C "), "LM: %s", got());
}

static void td_set(uint32_t idx, uint16_t tap, uint16_t hold, uint16_t dtap, uint16_t thold, uint16_t term)
{
    uint16_t v[5] = {tap, hold, dtap, thold, term};
    uint32_t f;
    for (f = 0; f < 5u; f++) {
        cfg.td[idx][2u * f] = (uint8_t)v[f];
        cfg.td[idx][2u * f + 1u] = (uint8_t)(v[f] >> 8);
    }
}

static void test_tapdance(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    td_set(0, KC_A, KC_LCTL, KC_B, 0, 200);
    set_key(0, NOTE(16), TD(0));
    /* single tap: A (after the term) */
    rec();
    at(NB(16), 30);
    at(0, 250);
    CHECK(!strcmp(got(), "+04 -04 "), "TD tap: %s", got());
    /* hold: Ctrl while held */
    rec();
    at(NB(16), 250);
    CHECK(!strcmp(got(), "+C "), "TD hold: %s", got());
    at(0, 10);
    CHECK(!strcmp(got(), "+C -C "), "TD hold released: %s", got());
    /* double tap: B, decided at the second release (on_double_tap set) */
    rec();
    at(NB(16), 30);
    at(0, 30);
    at(NB(16), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+05 -05 "), "TD double tap: %s", got());
    /* tapped, then another key: interrupted -> A, then that key */
    step(300);
    rec();
    at(NB(16), 30);
    at(0, 30);
    at(NB(2), 30);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 -04 +06 -06 "), "TD interrupted: %s", got());
    /* tap and hold only (no double): decided at the first release, no wait */
    td_set(1, KC_A, KC_LCTL, 0, 0, 200);
    set_key(0, NOTE(17), TD(1));
    step(300);
    rec();
    at(NB(17), 30);
    at(0, 1);
    CHECK(!strcmp(got(), "+04 -04 "), "TD tap+hold only: at once: %s", got());
}

static void cmb_set(uint32_t c, uint16_t a, uint16_t b, uint16_t out)
{
    memset(cfg.combo[c], 0, 10);
    cfg.combo[c][0] = (uint8_t)a;
    cfg.combo[c][2] = (uint8_t)b;
    cfg.combo[c][8] = (uint8_t)out;
    cfg.combo[c][9] = (uint8_t)(out >> 8);
}

static void test_combos(void)
{
    kb_init();
    btn = notes = 0;
    pass();
    cmb_set(0, KC_A, KC_C, KC_ESC);          /* F3 (A) + G3 (C): Esc */
    /* both within the term: Esc, held until one is let go */
    rec();
    at(NB(0), 20);
    at(NB(0) | NB(2), 30);
    CHECK(!strcmp(got(), "+29 "), "combo: %s", got());
    at(NB(2), 10);
    at(0, 5);
    CHECK(!strcmp(got(), "+29 -29 "), "combo released: %s", got());
    /* one alone: itself, once the term is over */
    rec();
    at(NB(0), 100);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 -04 "), "combo key alone: %s", got());
    /* the second too late: two keys */
    rec();
    at(NB(0), 80);
    at(NB(0) | NB(2), 20);
    at(0, 5);
    CHECK(!strcmp(got(), "+04 +06 -04 -06 "), "too late: %s", got());
    /* a key that is in no combo: no delay */
    rec();
    at(NB(4), 0);
    CHECK(!strcmp(got(), "+08 "), "other key at once: %s", got());
    at(0, 5);
}

int main(void)
{
    test_map();
    test_keys();
    test_layers();
    test_toggle();
    test_encoders();
    test_chord();
    test_vial();
    test_macros();
    test_media();
    test_tapping();
    test_oneshot_tt_lm();
    test_tapdance();
    test_combos();
    printf("core_test: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
