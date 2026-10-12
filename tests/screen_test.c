/* Host test of firmware/ui.c: kbcore.c + the screen drawn into a 240 x 240 picture. Scenes are written as PPM files
 * (build/host/screens/NAME.ppm; tests/ppm2png.py makes PNGs to look at) and checked for basic sanity: nothing drawn
 * outside the screen, the redraw-on-change regions do redraw nothing when nothing changed. Exit status 1 on a
 * failure. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t t_ms;
static uint32_t kb_now(void) { return t_ms; }
static int kb_out_ready(void) { return 1; }
static uint32_t kb_out_room(void) { return 16; }
static int kb_out_push(const uint8_t r[8]) { (void)r; return 1; }
static void kb_diag(uint8_t *out) { (void)out; }
static int kb_media_ok(void) { return 1; }
static uint32_t kb_media_room(void) { return 16; }
static int kb_media_push(const uint8_t *r, uint32_t n) { (void)r; (void)n; return 1; }
static void kb_uboot(void) {}
#include "../firmware/kbcore.c"

static uint16_t fb[240 * 240];
static uint32_t px_drawn, out_of_screen;
static void gfx_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        out_of_screen++;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
    px_drawn += w * h;
}
static void gfx_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        out_of_screen++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
    px_drawn += w * h;
}
static void gfx_sync(void) {}
#define GFX_PANEL_SWAP 0
#include "../firmware/ui.c"
#include "../firmware/editor.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void save(const char *name)
{
    char path[256];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof path, "build/host/screens/%s.ppm", name);
    f = fopen(path, "wb");
    if (!f) {
        fails++;
        printf("FAIL cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6 240 240 255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11 & 31) * 255 / 31, f);
        fputc((c >> 5 & 63) * 255 / 63, f);
        fputc((c & 31) * 255 / 31, f);
    }
    fclose(f);
}

static ui_status_t st = {4, 0, 0, 0, 0, 0};
static const int32_t NOENC[KB_NENC];
static void frame(void)
{
    st.now = t_ms;
    ui_normal(&st);
}
static void press_note(uint32_t n)
{
    kb_input(0, 1u << n, 0, 0, NOENC);
    kb_input(0, 0, 0, 0, NOENC);
}

int main(void)
{
    uint32_t before;
    kb_init();
    ui_invalidate();
    frame();
    save("normal_start");
    before = px_drawn;
    frame();
    CHECK(px_drawn == before, "nothing changed: nothing redrawn (%u px)", px_drawn - before);

    {   /* a note key touched: the KEYS page, the key highlighted, its detail */
        press_note(4);
        frame();
        save("normal_keys");
    }
    {   /* a knob turned */
        int32_t enc[KB_NENC] = {0};
        enc[ENC_KNOB1] = 1;
        kb_input(0, 0, 0, 0, enc);
        frame();
        save("normal_knob");
    }
    {   /* a layer held: the legend follows it; some bindings of every kind */
        uint8_t r, c;
        pos_rowcol(KB_NBTN + 26u, &r, &c);
        put_be16(&cfg.keymap[km_ofs(0, r, c)], MO(1));
        pos_rowcol(2, &r, &c);
        put_be16(&cfg.keymap[km_ofs(1, r, c)], LCTL(KC_C));
        pos_rowcol(3, &r, &c);
        put_be16(&cfg.keymap[km_ofs(1, r, c)], MT(MOD_BIT_CTL, KC_ESC));
        pos_rowcol(4, &r, &c);
        put_be16(&cfg.keymap[km_ofs(1, r, c)], QK_MACRO + 3u);
        pos_rowcol(5, &r, &c);
        put_be16(&cfg.keymap[km_ofs(1, r, c)], TG(2));
        put_be16(&cfg.enc[enc_ofs(1, 0, 1)], KC_WH_D);
        kb_input(0, 1u << 26, 0, 0, NOENC);
        kb_input(4u, 1u << 26, 0, 0, NOENC);   /* SEL (button 2) pressed while the layer is held */
        t_ms += 10;
        frame();
        save("normal_layer_held");
        kb_input(0, 0, 0, 0, NOENC);
    }
    {   /* an empty layer: SAME everywhere on the base = nothing */
        int32_t enc[KB_NENC] = {0};
        enc[ENC_PRESETS] = 2;
        kb_input(0, 0, 0, 0, enc);
        t_ms += 3000;
        frame();
        save("normal_empty_layer");
        enc[ENC_PRESETS] = -2;
        kb_input(0, 0, 0, 0, enc);
    }
    {   /* Vial unlock in progress */
        st.vial = 2;
        st.unlock_permille = 400;
        t_ms += 3000;
        frame();
        save("normal_unlock");
        st.vial = 1;
        frame();
        save("normal_vial_open");
    }
    {   /* the font: every character, both sizes */
        char all[128], line[25];
        uint32_t n = 0, i, y = 0;
        for (i = 0x20; i <= 0x7E; i++)
            all[n++] = (char)i;
        for (i = 0; i < FONT_SYMS; i++)
            all[n++] = (char)(0x80u + i);
        gfx_fill(0, 0, 240, 240, C_BLACK);
        for (i = 0; i < n; i += 24, y += 18) {
            uint32_t k;
            for (k = 0; k < 24u && i + k < n; k++)
                line[k] = all[i + k];
            line[k] = 0;
            gfx_text(0, y, line, F_S, 2, C_WHITE, C_BLACK);
        }
        gfx_text(0, y + 4u, "Hello fr 12345", F_L, 2, C_GREEN, C_BLACK);
        gfx_text(0, y + 32u, "MACRO 3 \"Hey\"", F_L, 2, C_YELLOW, C_BLACK);
        gfx_text(0, y + 60u, "Big L 3x", F_L, 3, C_CYAN, C_BLACK);
        save("font");
    }
    /* ---- the editor: driven as the panel would drive it ---- */
    {
        int32_t enc[KB_NENC];
        uint16_t kc;
#define ED_PASS(btn_, notes_) do { memset(enc, 0, sizeof enc); t_ms += 20; ed_input(btn_, notes_, 0, 0, enc, t_ms); \
                                   ed_input(0, 0, 0, 0, enc, t_ms); } while (0)
#define ED_TURN(e_, n_) do { memset(enc, 0, sizeof enc); enc[e_] = n_; t_ms += 20; ed_input(0, 0, 0, 0, enc, t_ms); } while (0)
#define ED_FRAME(name_) do { ed_draw(t_ms, "FM-1_961", 3, 1); save(name_); } while (0)
        kb_init();
        /* OCT+ opens it */
        kb_input(2, 0, 0, 0, NOENC);
        t_ms += 30;
        kb_input(0, 0, 0, 0, NOENC);
        CHECK(kb.editor_req == 1, "OCT+: editor request");
        ed_enter(t_ms);
        ED_PASS(0, 0);
        t_ms += 3000;
        ED_FRAME("ed_choose");
        CHECK(ed_led(POS_OCTDN) == LV_ON && ed_led(POS_OCTUP) == LV_OFF, "CHOOSE, nothing pointed: OCT- lit, OCT+ off");
        CHECK(ui_led_normal(POS_OCTUP, 0) == LV_ON && ui_led_normal(POS_OCTDN, 0) == LV_OFF, "NORMAL: OCT+ lit only");
        /* REC pointed at, OCT+: EDITING REC on layer 0 (REC = F24 by default) */
        ED_PASS(1u << 13, 0);
        CHECK(ed_led(POS_OCTUP) == LV_ON, "a control pointed at: OCT+ lit");
        CHECK(ed.ptr == 13, "REC pointed at: %d", ed.ptr);
        ED_FRAME("ed_choose_rec");
        ED_PASS(2, 0);
        CHECK(ed.st == ED_EDIT && ed.cat == C_FKEYS, "EDITING REC: F-KEYS (cat %u)", ed.cat);
        ED_FRAME("ed_edit_fkeys");
        /* category back to LETTERS (PRESETS), C (ALGORITHM 2), Ctrl (FX) */
        ED_TURN(ENC_PRESETS, -3);
        CHECK(ed.cat == C_ALNUM, "PRESETS -3: letters (cat %u)", ed.cat);
        ED_TURN(ENC_ALGORITHM, 2);
        ED_PASS(1u << 2, 0);
        CHECK(!ed_compose(&kc, &(const char *){0}) && kc == LCTL(KC_C), "Ctrl+C composed: %04x", kc);
        ED_FRAME("ed_edit_ctrl_c");
        {   /* a short category after a full one: nothing left of the old grid */
            uint32_t x, y, lit = 0;
            ED_TURN(ENC_PRESETS, 10);        /* LETTERS -> FM-1 (3 cells, one row) */
            CHECK(ed.cat == C_FM1, "FM-1 (cat %u)", ed.cat);
            ed_draw(t_ms, "FM-1_961", 3, 1);
            for (y = Y_E_GRID + E_CELL_H; y < Y_E_GRID + ED_ROWS * E_CELL_H; y++)
                for (x = 0; x < 240u; x++)
                    lit += fb[y * 240u + x] != C_BLACK;
            CHECK(lit == 0, "the old grid cleared: %u pixels left", lit);
            save("ed_edit_short_cat");
            ED_TURN(ENC_PRESETS, -10);
            ED_TURN(ENC_ALGORITHM, 2);       /* (a new category starts at its first cell) C again */
        }
        ED_PASS(2, 0);                       /* OCT+: saved */
        CHECK(ed.st == ED_CHOOSE && km_get(0, 0, 13) == LCTL(KC_C) && cfg.dirty, "saved: REC = %04x", km_get(0, 0, 13));
        /* the next: a key, LAYER mode: tap Space, hold L2 */
        ED_PASS(0, 1u << 2);                 /* G3 */
        ED_PASS(2, 0);
        ED_TURN(ENC_PRESETS, 1);             /* (from LETTERS) -> SYMBOLS */
        ED_TURN(ENC_PRESETS, 1);             /* -> EDIT / NAV */
        ED_TURN(ENC_ALGORITHM, 4);           /* Space */
        ED_PASS(1u << 7, 0);                 /* GLO: MODE -> HOLD */
        ED_PASS(1u << 7, 0);                 /* -> ONE-SHOT */
        ED_PASS(1u << 7, 0);                 /* -> LAYER */
        ED_FRAME("ed_edit_layer_nolayer");
        CHECK(ed_led(POS_OCTUP) == LV_OFF && ed_led(POS_OCTDN) == LV_ON, "EDITING, cannot save: OCT+ off");
        ED_PASS(1u << 4, 0);                 /* ENV: L2 */
        CHECK(!ed_compose(&kc, &(const char *){0}) && kc == LT(2, KC_SPC), "LT(2, Space): %04x", kc);
        ED_FRAME("ed_edit_lt");
        ED_PASS(2, 0);
        /* symbols: '!' (Shift shown); then cancel */
        ED_PASS(0, 1u << 4);
        ED_PASS(2, 0);
        ED_TURN(ENC_PRESETS, 1);             /* E: LETTERS -> SYMBOLS */
        CHECK(ed.cat == C_SYM, "symbols (cat %u)", ed.cat);
        ED_TURN(ENC_SELECT, 1);
        ED_TURN(ENC_ALGORITHM, 5);           /* row 1 col 5: '!' is index 11 */
        CHECK(!ed_compose(&kc, &(const char *){0}) && kc == LSFT(KC_1), "!: %04x (cur %u)", kc, ed.cur);
        ED_FRAME("ed_edit_symbol");
        ED_PASS(1, 0);                       /* OCT-: cancel */
        CHECK(ed.st == ED_CHOOSE && km_get(0, 2, 2) == KC_E, "cancelled: still E (%04x)", km_get(0, 2, 2));
        /* a value set in Vial the editor cannot show: KEEP */
        {
            uint8_t r, c;
            pos_rowcol(KB_NBTN + 5u, &r, &c);
            put_be16(&cfg.keymap[km_ofs(0, r, c)], TT(1));
            ED_PASS(0, 1u << 5);
            ED_PASS(2, 0);
            CHECK(ed.has_keep && ed.cat == C_EMPTY && ed.cur == 2u, "TT(1): KEEP");
            ED_FRAME("ed_edit_keep");
            ED_PASS(2, 0);
            CHECK(km_get(0, r, c) == TT(1), "KEEP saved as is");
        }
        /* LAYERS category on a knob: HOLD refused, GO TO fine */
        {
            memset(enc, 0, sizeof enc);
            enc[ENC_KNOB1 + 1] = 1;          /* KNOB2 CW */
            t_ms += 20;
            ed_input(0, 0, 0, 0, enc, t_ms);
            CHECK(ed.ptr == 100 + 2 * 1 + 1, "KNOB2 CW pointed (%d)", ed.ptr);
            ED_PASS(2, 0);
            ED_TURN(ENC_PRESETS, 1);         /* (wheel down:) MOUSE -> LAYERS */
            CHECK(ed.cat == C_LAYERS, "layers (cat %u)", ed.cat);
            CHECK(ed_compose(&kc, &(const char *){0}), "knob: HOLD 0 refused");
            ED_FRAME("ed_edit_knob_layers");
            ED_TURN(ENC_SELECT, 2);          /* GO TO row */
            ED_TURN(ENC_ALGORITHM, 3);
            CHECK(!ed_compose(&kc, &(const char *){0}) && kc == DF(3), "knob: GO TO 3 (%04x)", kc);
            ED_PASS(2, 0);
            CHECK(enc_get(0, 1, 1) == DF(3), "knob saved");
        }
        /* COPY: REC -> PLAY, then SWAP FX / SEL */
        ED_TURN(ENC_SELECT, 1);
        CHECK(ed.action == A_COPY, "action COPY");
        ED_PASS(1u << 13, 0);
        ED_PASS(2, 0);
        CHECK(ed.st == ED_PICK2, "COPY: pick the target");
        ED_PASS(1u << 12, 0);
        ED_FRAME("ed_copy");
        ED_PASS(2, 0);
        CHECK(km_get(0, 0, 12) == LCTL(KC_C), "pasted on PLAY: %04x", km_get(0, 0, 12));
        ED_PASS(1, 0);
        CHECK(ed.st == ED_CHOOSE && ed.action == A_EDIT, "COPY done");
        /* COPY LAYER 0 -> 1, INFO */
        ED_TURN(ENC_SELECT, 5);
        CHECK(ed.action == A_COPYL, "action COPY LAYER (%u)", ed.action);
        ED_PASS(2, 0);
        ED_FRAME("ed_copy_layer");
        ED_PASS(2, 0);
        CHECK(km_get(1, 0, 13) == LCTL(KC_C) && ed.layer == 1, "layer copied");
        ED_TURN(ENC_SELECT, 7);
        ED_PASS(2, 0);
        ED_FRAME("ed_info");
        ED_PASS(1, 0);
        /* leave: back to NORMAL on the layer edited, nothing typed */
        ED_PASS(1, 0);
        CHECK(!ed_on() && cfg.layer == 1, "left on layer 1 (%u)", cfg.layer);
    }
    CHECK(!out_of_screen, "drawing outside the screen: %u", out_of_screen);
    printf("screen_test: %s (%d failures), pictures in build/host/screens/\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
