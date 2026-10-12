/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: the configuration in flash. Two copies in two 4-KiB sectors, written in turn; at boot
 * the valid copy with the higher sequence number wins, none valid: the defaults. A copy is
 *   [header 16 B: magic, version, length, sequence, CRC32 of the body][body: kbcore.c's cfg, the persistent part]
 * written body first and header last, so a write cut short (power off, a crash) leaves a sector without a valid
 * header: the other copy, untouched, still loads. Portable: the flash comes from the includer's hooks
 *   st_erase(off)          erase the 4-KiB sector at off; 0 = ok
 *   st_write(off, p, n)    program n bytes (erased area); 0 = ok
 *   st_read(off, p, n)     read n bytes; 0 = ok
 * (main.c: felucca-bsp fm1_flash.h; tests/store_test.c: a RAM image that can lose power anywhere). */
/* The settings area of the Felucca family (felucca-bsp FL_GLOB 0xFC000..0xFEFFF; Felucca, Dinghy and Optimist keep
 * their settings there): above the region a stock (V15) install wipes (0x93000.., where 0x97000 was lost, measured
 * 2026-10-10) and the SDK's own sectors (VM 0xE8000, USR 0xEA000..0xFBFFF, key_mac 0xFF000). Another firmware of the
 * family may overwrite it, or find ours: magic + CRC, each then starts from its defaults. */
#define ST_SLOT_A 0x000FC000u
#define ST_SLOT_B 0x000FD000u
#define ST_SECTOR 0x1000u
#define ST_MAGIC 0x3143424Bu                /* "KBC1" */
#define ST_VERSION 4u                       /* bump when the body's layout changes; an older version this file knows
                                             * loads and migrates (st_load_old, cfg_migrate), any other: defaults.
                                             * 2: the copies moved from 0x97000 / 0x98000; 3: QMK settings, tap
                                             * dances, combos appended (a 2 is a prefix of a 3); 4: 5 layers (not
                                             * 8), 4 knobs (not 7 encoders), lights per layer, FM-1 keycodes
                                             * renumbered */
#define ST_HDR 16u

static int st_erase(uint32_t off);
static int st_write(uint32_t off, const void *p, uint32_t n);
static int st_read(uint32_t off, void *p, uint32_t n);

typedef char st_fits[ST_HDR + CFG_PERSIST_BYTES <= ST_SECTOR ? 1 : -1];

static struct {
    uint32_t seq;                           /* of the copy in RAM (0: defaults, never saved) */
    uint32_t slot;                          /* where it is (the next save goes to the other) */
    uint32_t saves, errors, loaded;         /* loaded: 0 defaults, 1 slot A, 2 slot B */
} st;

static uint32_t crc32_upd(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t i, k;
    c = ~c;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Versions 2 and 3: kbcore.c's cfg as it was (8 layers, 7 encoders), offsets in the body. Version 2 ends before
 * the QMK settings. */
#define OLD_LAYERS 8u
#define OLD_NENC 7u
#define OLD_ENC 768u                        /* 8 layers x 3 rows x 16 cols x 2 */
#define OLD_MACROS 992u                     /* + 8 layers x 7 encoders x 2 directions x 2 */
#define OLD_LAYER 3040u                     /* the preset byte (0..3), + 3 reserved */
#define OLD_QS 3044u                        /* 12 bytes, as today */
#define OLD_TD 3056u
#define OLD_COMBO 3216u
#define OLD_V2_BYTES OLD_QS
#define OLD_V3_BYTES 3376u
typedef char st_old_qs[sizeof cfg.qs == OLD_TD - OLD_QS && sizeof cfg.td + sizeof cfg.combo == OLD_V3_BYTES - OLD_TD ?
                       1 : -1];

/* the body length a header's version | length word allows (this version, or an older one we migrate), else 0 */
static uint32_t st_body_len(uint32_t vl)
{
    if (vl == (ST_VERSION | CFG_PERSIST_BYTES << 16))
        return CFG_PERSIST_BYTES;
    if (vl == (3u | OLD_V3_BYTES << 16))
        return OLD_V3_BYTES;
    if (vl == (2u | OLD_V2_BYTES << 16))
        return OLD_V2_BYTES;
    return 0;
}

/* an older copy's body (at off) into cfg, field by field: layers 1..5 of the 8, KNOB1..4 of the 7 encoders (ids
 * 2..5); 0 = ok */
static int st_load_old(uint32_t off, uint32_t version)
{
    uint32_t l, k;
    if (st_read(off, cfg.keymap, CFG_KEYMAP_BYTES))   /* [layer][row][col]: layers 0..4 come first */
        return -1;
    for (l = 0; l < KB_LAYERS; l++)
        for (k = 0; k < KB_NKNOB; k++)
            if (st_read(off + OLD_ENC + (l * OLD_NENC + ENC_KNOB1 + k) * 4u, &cfg.enc[enc_ofs(l, k, 0)], 4u))
                return -1;
    if (st_read(off + OLD_MACROS, cfg.macros, KB_MACRO_BYTES) || st_read(off + OLD_LAYER, &cfg.layer, 1u))
        return -1;
    if (version >= 3u && (st_read(off + OLD_QS, &cfg.qs, sizeof cfg.qs) ||
                          st_read(off + OLD_TD, cfg.td, sizeof cfg.td) ||
                          st_read(off + OLD_COMBO, cfg.combo, sizeof cfg.combo)))
        return -1;
    return 0;
}

/* a slot's sequence number if it holds a valid copy (CRC over the body, read in pieces), else 0 */
static uint32_t st_check(uint32_t slot)
{
    uint8_t h[ST_HDR], b[128];
    uint32_t crc = 0, i, len;
    if (st_read(slot, h, ST_HDR) || rd32(h) != ST_MAGIC || !(len = st_body_len(rd32(h + 4))))
        return 0;
    for (i = 0; i < len; i += sizeof b) {
        uint32_t k = len - i < sizeof b ? len - i : sizeof b;
        if (st_read(slot + ST_HDR + i, b, k))
            return 0;
        crc = crc32_upd(crc, b, k);
    }
    return crc == rd32(h + 12) && rd32(h + 8) ? rd32(h + 8) : 0;
}

/* boot: the newer valid copy into cfg (else cfg keeps the defaults kb_init put there) */
static void st_load(void)
{
    uint32_t a = st_check(ST_SLOT_A), b = st_check(ST_SLOT_B);
    uint32_t slot = a >= b ? ST_SLOT_A : ST_SLOT_B, seq = a >= b ? a : b;
    if (!seq) {
        st.seq = 0;
        st.slot = ST_SLOT_B;                /* the first save goes to A */
        st.loaded = 0;
        return;
    }
    {
        uint8_t h[ST_HDR];
        uint32_t len = st_read(slot, h, ST_HDR) ? 0 : st_body_len(rd32(h + 4)), version = rd32(h + 4) & 0xFFFFu;
        if (!len)
            goto bad;
        if (version == ST_VERSION ? st_read(slot + ST_HDR, cfg_persist(), len) : st_load_old(slot + ST_HDR, version))
            goto bad;
        if (st_check(slot) != seq)
            goto bad;
        cfg_migrate(version);               /* (an older copy: what it lacks gets its defaults) */
        cfg_loaded();
        st.seq = seq;
        st.slot = slot;
        st.loaded = slot == ST_SLOT_A ? 1u : 2u;
        return;
    }
bad:
    {
        cfg_defaults();                     /* (the flash changed under us: not expected) */
        st.errors++;
        st.seq = 0;
        st.slot = ST_SLOT_B;
        st.loaded = 0;
    }
}

/* write cfg to the other slot; 0 = saved (and verified) */
static int st_save(void)
{
    uint8_t h[ST_HDR];
    uint32_t slot = st.slot == ST_SLOT_A ? ST_SLOT_B : ST_SLOT_A, seq = st.seq + 1u, i;
    for (i = 0; i < ST_HDR; i++)
        h[i] = 0xFF;
    wr32(h, ST_MAGIC);
    wr32(h + 4, ST_VERSION | CFG_PERSIST_BYTES << 16);
    wr32(h + 8, seq);
    wr32(h + 12, crc32_upd(0, cfg_persist(), CFG_PERSIST_BYTES));
    if (st_erase(slot) || st_write(slot + ST_HDR, cfg_persist(), CFG_PERSIST_BYTES) || st_write(slot, h, ST_HDR) ||
        st_check(slot) != seq) {
        st.errors++;
        return -1;                          /* the old copy is still there; the next save tries again */
    }
    st.seq = seq;
    st.slot = slot;
    st.saves++;
    cfg.dirty = 0;
    return 0;
}
