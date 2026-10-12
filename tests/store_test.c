/* Host test of firmware/store.c with kbcore.c's cfg: a simulated NOR flash (erase to 0xFF, programming only clears
 * bits) that can lose power at any byte of a save (an erase cut short leaves the sector garbage). After every cut
 * the "reboot" must load either the old configuration or the new one, never anything else. Exit status 1 on a
 * failure. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t t_ms;
static uint32_t kb_now(void) { return t_ms; }
static int kb_out_ready(void) { return 0; }
static uint32_t kb_out_room(void) { return 16; }
static int kb_out_push(const uint8_t r[8]) { (void)r; return 1; }
static void kb_diag(uint8_t *out) { (void)out; }
static int kb_media_ok(void) { return 0; }
static uint32_t kb_media_room(void) { return 0; }
static int kb_media_push(const uint8_t *r, uint32_t n) { (void)r; (void)n; return 0; }
static void kb_uboot(void) {}
#include "../firmware/kbcore.c"

#define FL_BASE 0xFC000u                    /* store.c ST_SLOT_A .. ST_SLOT_B + one sector */
#define FL_SIZE 0x2000u
static uint8_t fl[FL_SIZE];
static int32_t budget = -1;                 /* bytes (an erase: 1) still written before the power goes; -1: no cut */
static int dead;
static uint32_t rnd = 12345;

static int spend(void)
{
    if (dead)
        return 0;
    if (budget == 0) {
        dead = 1;
        return 0;
    }
    if (budget > 0)
        budget--;
    return 1;
}
static int st_erase(uint32_t off)
{
    uint32_t i;
    if (off < FL_BASE || off + 0x1000u > FL_BASE + FL_SIZE || (off & 0xFFFu))
        return -1;
    if (!spend()) {                         /* cut during the erase: the sector is garbage */
        for (i = 0; i < 0x1000u; i++) {
            rnd = rnd * 1103515245u + 12345u;
            fl[off - FL_BASE + i] &= (uint8_t)(rnd >> 16);
        }
        return -1;
    }
    memset(&fl[off - FL_BASE], 0xFF, 0x1000u);
    return 0;
}
static int st_write(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    uint32_t i;
    if (off < FL_BASE || off + n > FL_BASE + FL_SIZE)
        return -1;
    for (i = 0; i < n; i++) {
        if (!spend())
            return -1;
        fl[off - FL_BASE + i] &= s[i];      /* NOR: bits only go to 0 */
    }
    return 0;
}
static int st_read(uint32_t off, void *p, uint32_t n)
{
    if (off < FL_BASE || off + n > FL_BASE + FL_SIZE)
        return -1;
    memcpy(p, &fl[off - FL_BASE], n);
    return 0;
}
#include "../firmware/store.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reboot(void)
{
    dead = 0;
    budget = -1;
    memset(&st, 0, sizeof st);
    kb_init();                              /* defaults, as at power-on */
    st_load();
}
static uint8_t snap[2][CFG_PERSIST_BYTES];
static void edit(uint32_t v)                /* a recognisable configuration */
{
    put_be16(&cfg.keymap[0], (uint16_t)(0x0100u + v));
    cfg.macros[0] = (uint8_t)('a' + v);
    cfg.macros[KB_MACRO_BYTES - 2] = (uint8_t)v;
    cfg.layer = (uint8_t)(v % KB_LAYERS);
    cfg_touch();
}

int main(void)
{
    uint8_t defaults[CFG_PERSIST_BYTES];
    uint32_t cut, total, olds = 0, news = 0;
    memset(fl, 0xFF, sizeof fl);
    reboot();
    memcpy(defaults, cfg_persist(), CFG_PERSIST_BYTES);
    CHECK(st.loaded == 0, "empty flash: defaults");

    edit(1);
    CHECK(st_save() == 0 && st.slot == ST_SLOT_A && st.seq == 1, "first save to A, seq 1");
    memcpy(snap[0], cfg_persist(), CFG_PERSIST_BYTES);
    reboot();
    CHECK(st.loaded == 1 && !memcmp(cfg_persist(), snap[0], CFG_PERSIST_BYTES), "reboot: A loaded");

    edit(2);
    CHECK(st_save() == 0 && st.slot == ST_SLOT_B && st.seq == 2, "second save to B, seq 2");
    memcpy(snap[1], cfg_persist(), CFG_PERSIST_BYTES);
    reboot();
    CHECK(st.loaded == 2 && !memcmp(cfg_persist(), snap[1], CFG_PERSIST_BYTES), "reboot: B (newer) loaded");

    /* power cut at every point of the next save (A gets overwritten): old (B) or new, never else */
    total = 1u + CFG_PERSIST_BYTES + ST_HDR;
    for (cut = 0; cut <= total; cut = cut < 40u || cut >= total - 40u ? cut + 1u :   /* the erase, the start, */
                                     cut + 97u < total - 40u ? cut + 97u : total - 40u) {   /* samples, the header */
        uint8_t keep[FL_SIZE];
        memcpy(keep, fl, sizeof fl);
        reboot();
        edit(3);
        budget = (int32_t)cut;
        (void)st_save();
        reboot();
        if (!memcmp(cfg_persist(), snap[1], CFG_PERSIST_BYTES))
            olds++;
        else {
            edit(3);                        /* what the new copy holds (cfg_touch aside) */
            reboot();
            CHECK(st.seq == 3, "cut %u: neither the old nor the new copy loaded (seq %u)", cut, st.seq);
            news++;
        }
        memcpy(fl, keep, sizeof fl);        /* back to "B newest, A old" for the next cut */
    }
    CHECK(olds > 0 && news > 0, "cuts: %u old, %u new", olds, news);
    printf("power cuts: %u gave the old copy, %u the new one\n", olds, news);

    /* a damaged copy (one bit in the body): the other loads */
    reboot();
    fl[ST_SLOT_B - FL_BASE + ST_HDR + 100] ^= 0x10;
    reboot();
    CHECK(st.loaded == 1, "B damaged: A loads (loaded %u)", st.loaded);
    /* both damaged: defaults */
    fl[ST_SLOT_A - FL_BASE + ST_HDR + 7] ^= 0x01;
    reboot();
    CHECK(st.loaded == 0 && !memcmp(cfg_persist(), defaults, CFG_PERSIST_BYTES), "both damaged: defaults");
    /* another version: defaults */
    memset(fl, 0xFF, sizeof fl);
    reboot();
    edit(4);
    st_save();
    fl[ST_SLOT_A - FL_BASE + 4] = 0x00;     /* version byte */
    reboot();
    CHECK(st.loaded == 0, "other version: defaults");
    /* out-of-range values in a valid copy are clamped */
    memset(fl, 0xFF, sizeof fl);
    reboot();
    cfg.layer = 9;
    cfg.light[2][5] = 77;
    st_save();
    reboot();
    CHECK(cfg.layer == 0 && cfg.light[2][5] == LIGHT_AUTO, "layer 9 loads as 0, light 77 as AUTO");

    /* migration: version 3 and 2 copies (8 layers, 7 encoders, the old FM-1 keycodes), written as the old firmware
     * did; they load into today's layout */
    {
        static uint8_t old[OLD_V3_BYTES];
        uint32_t ver;
        for (ver = 3; ver >= 2u; ver--) {
            uint8_t h[ST_HDR];
            uint32_t len = ver == 3u ? OLD_V3_BYTES : OLD_V2_BYTES, l, e;
            memset(old, 0, sizeof old);
            for (l = 0; l < OLD_LAYERS; l++) /* a marker per layer: row 2 col 3 = 0x10 + layer */
                put_be16(&old[((l * 3u + 2u) * 16u + 3u) * 2u], (uint16_t)(0x10u + l));
            put_be16(&old[(0u * 16u + 5u) * 2u], 0x7E07);   /* layer 0 row 0 col 5: old KB OFF */
            put_be16(&old[(0u * 16u + 6u) * 2u], 0x7E02);   /* old SYSTEM */
            put_be16(&old[(0u * 16u + 0u) * 2u], 0x7E00);   /* old PRESET- */
            for (l = 0; l < OLD_LAYERS; l++)
                for (e = 0; e < OLD_NENC; e++) {             /* encoder (l, e) CCW = 0x2000 + l * 16 + e, CW + 0x100 */
                    put_be16(&old[OLD_ENC + (l * OLD_NENC + e) * 4u], (uint16_t)(0x2000u + l * 16u + e));
                    put_be16(&old[OLD_ENC + (l * OLD_NENC + e) * 4u + 2u], (uint16_t)(0x2100u + l * 16u + e));
                }
            old[OLD_MACROS] = 'm';
            old[OLD_MACROS + KB_MACRO_BYTES - 2u] = 'z';
            old[OLD_LAYER] = 2;
            if (ver == 3u) {
                old[OLD_QS] = 0x09;                          /* tapping_term 777 = 0x0309, LE */
                old[OLD_QS + 1u] = 0x03;
                old[OLD_TD] = 0x07;                          /* tap dance 0 on_tap: old KB OFF, LE */
                old[OLD_TD + 1u] = 0x7E;
                old[OLD_COMBO + 8u] = KC_Q;                  /* combo 0 output */
            }
            memset(fl, 0xFF, sizeof fl);
            memset(h, 0xFF, sizeof h);
            wr32(h, ST_MAGIC);
            wr32(h + 4, ver | len << 16);
            wr32(h + 8, 5);
            wr32(h + 12, crc32_upd(0, old, len));
            st_erase(ST_SLOT_A);
            st_write(ST_SLOT_A + ST_HDR, old, len);
            st_write(ST_SLOT_A, h, ST_HDR);
            reboot();
            CHECK(st.loaded == 1, "v%u copy loads (loaded %u)", ver, st.loaded);
            for (l = 0; l < KB_LAYERS; l++)
                CHECK(km_get(l, 2, 3) == 0x10u + l, "v%u layer %u marker %04x", ver, l, km_get(l, 2, 3));
            CHECK(km_get(0, 0, 5) == FK_KB_OFF && km_get(0, 0, 6) == KC_NO && km_get(0, 0, 0) == FK_LAYER_PREV,
                  "v%u FM-1 keycodes renumbered: %04x %04x %04x", ver, km_get(0, 0, 5), km_get(0, 0, 6),
                  km_get(0, 0, 0));
            for (l = 0; l < KB_LAYERS; l++)
                for (e = 0; e < KB_NKNOB; e++)
                    CHECK(enc_get(l, e, 0) == 0x2000u + l * 16u + ENC_KNOB1 + e &&
                          enc_get(l, e, 1) == 0x2100u + l * 16u + ENC_KNOB1 + e,
                          "v%u knob %u layer %u: %04x %04x", ver, e, l, enc_get(l, e, 0), enc_get(l, e, 1));
            CHECK(cfg.macros[0] == 'm' && cfg.macros[KB_MACRO_BYTES - 2u] == 'z' && cfg.layer == 2,
                  "v%u macros, layer", ver);
            CHECK(cfg.light[4][40] == LIGHT_AUTO, "v%u lights: defaults", ver);
            if (ver == 3u)
                CHECK(cfg.qs.tapping_term == 777 && le16(&cfg.td[0][0]) == FK_KB_OFF && le16(&cfg.combo[0][8]) == KC_Q,
                      "v3 settings, tap dance (renumbered), combo: %u %04x", cfg.qs.tapping_term, le16(&cfg.td[0][0]));
            else
                CHECK(cfg.qs.tapping_term == 200 && cfg.qs.tapping_toggle == 5 && le16(&cfg.td[0][0]) == 0,
                      "v2: settings defaults (term %u)", cfg.qs.tapping_term);
            cfg_touch();
            CHECK(st_save() == 0 && st.seq == 6, "the next save: version 4, seq 6");
            reboot();
            CHECK(st.loaded == 2 && km_get(4, 2, 3) == 0x14u && cfg.layer == 2, "v4 copy loads");
        }
    }

    printf("store_test: %s (%d failures), %u bytes stored\n", fails ? "FAILED" : "ok", fails, CFG_PERSIST_BYTES);
    return fails != 0;
}
