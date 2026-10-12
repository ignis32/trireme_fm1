/* firmware/kbcore.c as a shared library for tests/vialgui_test.py: vial-gui's own protocol code talks to it
 * through ctypes (a request in, the reply in place), with a clock and the panel the test controls. */
#include <stdint.h>
#include <string.h>

static uint32_t t_ms;
static uint32_t kb_now(void) { return t_ms; }
static int kb_out_ready(void) { return 1; }
static uint32_t kb_out_room(void) { return 16; }
static int kb_out_push(const uint8_t r[8]) { (void)r; return 1; }
static void kb_diag(uint8_t *out) { (void)out; }
static int kb_media_ok(void) { return 0; }
static uint32_t kb_media_room(void) { return 0; }
static int kb_media_push(const uint8_t *r, uint32_t n) { (void)r; (void)n; return 0; }
static void kb_uboot(void) {}
#include "../firmware/kbcore.c"

void lib_init(void) { kb_init(); }
int lib_cmd(uint8_t *m) { return vial_cmd(m); }
void lib_advance(uint32_t ms) { t_ms += ms; kb_task(); }
void lib_panel(uint32_t buttons, uint32_t notes)
{
    static const int32_t none[KB_NENC];
    kb_input(buttons, notes, 0, 0, none);
}
int lib_npos(void) { return KB_NPOS; }
int lib_pos_rowcol(int pos) { uint8_t r, c; pos_rowcol((uint32_t)pos, &r, &c); return r << 8 | c; }
const char *lib_fk_name(int i) { return i < (int)FK_COUNT ? FK_NAMES[i] : 0; }
int lib_km_get(int layer, int row, int col) { return km_get((uint32_t)layer, (uint32_t)row, (uint32_t)col); }
int lib_enc_get(int layer, int enc, int cw) { return enc_get((uint32_t)layer, (uint32_t)enc, (uint32_t)cw); }
int lib_default_key(int layer, int pos) { return default_key((uint32_t)layer, (uint32_t)pos); }
int lib_default_enc(int layer, int knob, int cw) { return default_knob((uint32_t)layer, (uint32_t)knob, (uint32_t)cw); }
const uint8_t *lib_macros(void) { return cfg.macros; }
int lib_unlocked(void) { return vl.unlocked; }
int lib_qs(int id) { uint32_t v = 0; return qs_get((uint32_t)id, &v) ? (int)v : -1; }
int lib_td(int idx, int field) { return le16(&cfg.td[idx][2 * field]); }
int lib_combo(int idx, int field) { return le16(&cfg.combo[idx][2 * field]); }
