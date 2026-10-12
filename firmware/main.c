/* SPDX-License-Identifier: GPL-3.0-only
 * fm1_keyboard: the FM-1 as a QMK-style keyboard configured with Vial. Built on felucca-bsp
 * (third_party/, MPL-2.0 + GPL-3.0 usb.c); boot, update entry and console in the style of Felucca's firmware, without
 * the sound:
 *   - boot: WDT, boot-loop guard (two boots in a row that never reach the 30 s healthy point -> UBOOT),
 *     .bss / .pool zeroed, .data and .ram_text copied, memory guards on
 *   - TIMER5 at 10 kHz: key scan and milliseconds; USB at 2 kHz (no audio, so never nested)
 *   - USB: MIDI (only for the updater and the UBOOT soft key) + HID keyboard + raw HID for Vial, or the CDC console
 *     instead of raw HID (firmware/usb.c, felucca-bsp's chip/usb.c + patches/usb-hid.patch)
 *   - main loop: the panel -> kbcore.c (keymap, layers, encoders -> HID reports), Vial's requests ->
 *     kbcore.c vial_cmd, the configuration saved to flash 3 s after the last change (store.c), the screen, the
 *     LEDs, the updater
 *   - updates: the standard M-UPGRADE entry of chip/ota.c (fm1_install.py, web installers, the official updater)
 * Fixed in firmware (kbcore.c): PRESETS switches the layer, OCT+ opens the editor (editor.c), OCT- + OCT+ held
 * 10 s: UBOOT; SELECT / ALGORITHM: the UI's. At power-on: OCT- held, no keyboard on USB (MIDI only,
 * upstream's descriptors); OCT+ held, UBOOT. Two failed boots: UBOOT. */
#include <stdint.h>
/* Two flavours (Makefile): KB_CONSOLE=1 the USB serial console (EP2 + EP3); 0 (default) raw HID on EP3 instead,
 * the channel Vial uses (kbcore.c vial_cmd), with the serial number Vial looks for */
#ifndef KB_CONSOLE
#define KB_CONSOLE 0
#endif
#define FELUCCA_CDC KB_CONSOLE
#define FELUCCA_RAWHID (!KB_CONSOLE)
#define FELUCCA_MEDIA (!KB_CONSOLE)         /* mouse / system / consumer on EP2: only without the console */
#define FELUCCA_OTA 1                       /* the standard update entry (M-UPGRADE protocol, chip/ota.c) */
#define FELUCCA_HID 1                       /* the keyboard (firmware/usb.c) */
#define FELUCCA_HID_EP 4                    /* EP2/EP3 are the console's or raw HID's; EP4 is free without audio */
#define FELUCCA_ID "FM-1_961"               /* the package identity: Makefile PRODUCT, checked by the installer */
#define FELUCCA_USB_STR2 28, 3, 'F', 0, 'M', 0, '-', 0, '1', 0, ' ', 0, 'K', 0, 'e', 0, 'y', 0, 'b', 0, 'o', 0, \
                         'a', 0, 'r', 0, 'd', 0  /* the product name: "FM-1 Keyboard" */
#if !KB_CONSOLE                             /* "vial:f64c2b3c": Vial finds its keyboards by it (build_vial.mk) */
#define FELUCCA_USB_SERIAL 28, 3, 'v', 0, 'i', 0, 'a', 0, 'l', 0, ':', 0, 'f', 0, '6', 0, '4', 0, 'c', 0, '2', 0, \
                           'b', 0, '3', 0, 'c', 0
#endif
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "fm1_input.h"
#include "fm1_lcd_hw.h"
/* the flash this firmware may erase or write (not even its own app): the update's staging area and the two
 * configuration sectors (store.c ST_SLOT_A / B: 0xFC000, 0xFD000; never 0xFF000, the chip's key_mac) */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, 0xFC000u, 0xFE000u))
#include "fm1_flash.h"
#include "libc.c"
#include "usb.c"                            /* firmware/usb.c: the include's own directory comes first */
#include "ota.c"
#define LCD_BAUD 4u                         /* SPI1 = 60 MHz / 5 = 12 MHz (Felucca's) */
#include "lcd.c"
#include "kbcore.c"                         /* the keymap engine and Vial; its hooks are below */
#include "store.c"                          /* the configuration in flash; its hooks are below */
/* ui.c's drawing (gfx.c): the LCD */
#define GFX_PANEL_SWAP 1
static void gfx_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { lcd_fill(x, y, w, h, c); }
static void gfx_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { lcd_blit(x, y, w, h, px); }
static void gfx_sync(void) { lcd_sync(); }
#include "ui.c"                             /* the screen */
#include "editor.c"                         /* the on-device editor */
/* a line of big text, centred (the update and UBOOT screens) */
static void text_draw(uint32_t y, const char *s, uint16_t fg, uint16_t bg) { gfx_text_c(0, GFX_W, y, s, F_L, 2, fg, bg); }

#define BTN_OCTDN 0u                        /* felucca-bsp FM1_BTN_OCT_DOWN / _UP */
#define BTN_OCTUP 1u
#define LED_NOTE0 14u                       /* note key n's LED id: 14 + n (felucca-bsp fm1_input.h) */

#define C_UPD_BLUE 0x0010u                  /* the update screens (ui.c has the rest) */
#define C_UPD_RED 0xA000u

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
extern void isr_timer5(void);

static volatile uint32_t ms;
/* .noinit objects are volatile and not static: nothing initialises them, and a static one's contents may be assumed
 * by the compiler (a cause of soft-bricks seen in practice). tools/check_noinit.sh enforces this. */
volatile struct { uint32_t magic, pending, failed; } kb_guard __attribute__((section(".noinit")));
/* boot breadcrumbs, kept across resets: how far the last boots got (in NOINIT, 0x01C7C000..: read through UBOOT) */
volatile struct { uint32_t magic, stage, boots, max_stage; } kb_crumbs __attribute__((section(".noinit")));
#define CRUMBS_MAGIC 0x4B435231u             /* "KCR1" */
#define GUARD_MAGIC 0x4B424431u              /* "KBD1" */
enum { ST_BOOT = 1, ST_FLASH, ST_LCD, ST_IO, ST_USB, ST_IRQ, ST_LOOP };
static void stage(uint32_t st)
{
    kb_crumbs.stage = st;
    if (st > kb_crumbs.max_stage)
        kb_crumbs.max_stage = st;
}

/* ---------------------------------------------------------------- interrupts */
void fm1_timer5_irq(void)                   /* 10 kHz: key scan, ms; USB at 2 kHz */
{
    static uint32_t sub, last, acc;
    uint32_t t0 = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
    }
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
}

void fm1_alnk0_irq(void) {}                 /* isr_alnk0 is linked (chip/fm1_isr.S), no audio runs */

static void fm1_fault(const fm1_crash_t *c)   /* chip/fm1_irq.h: report, then reset */
{
    (void)c;
    lcd_fill(0, 0, 240, 240, 0xF800);
    fm1_delay_ms(1500);
    fm1_reboot();
}

/* ---------------------------------------------------------------- the update entry (chip/ota.c hooks) */
static uint8_t flash_ok;                    /* the expected 1 MiB part answered at boot: else no updates from here */
static uint8_t screen_dirty;                /* redraw everything (after the update screens) */

static uint32_t ota_now_ms(void) { return ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)          /* IRQs off while the flash is busy */
{
    uint32_t took, f;
    int rc;
    if (!flash_ok || !ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    f = irq_save();
    rc = FL_FAR(fl_erase4k_ram)(off, &took);
    irq_restore(f);
    return rc;
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!flash_ok || !ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n)   /* 256-byte IRQ-off windows (Felucca's st_read) */
{
    uint8_t *d = p;
    if (!flash_ok)
        return -1;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    char b[16] = "FAILED -";
    screen_dirty = 1;
    lcd_fill(0, 0, 240, 240, C_UPD_BLUE);
    text_draw(80, "UPDATE", C_WHITE, C_UPD_BLUE);
    if (step < 9u) {
        text_draw(124, STEP[step < 6u ? step : 0], C_WHITE, C_UPD_BLUE);
        return;
    }
    if (code == 1)
        text_draw(124, "DRY RUN OK", C_WHITE, C_UPD_BLUE);
    else {
        uint32_t v = (uint32_t)-code;
        b[8] = (char)(v >= 10u ? '0' + v / 10u : '0' + v);
        b[9] = v >= 10u ? (char)('0' + v % 10u) : 0;
        b[10] = 0;
        lcd_fill(0, 0, 240, 240, C_UPD_RED);
        text_draw(80, "UPDATE", C_WHITE, C_UPD_RED);
        text_draw(124, b, C_WHITE, C_UPD_RED);
    }
    fm1_delay_ms(1500);
}
static void cfg_flush(void);
static void ota_commit(const uint8_t *parm)
{
    cfg_flush();                            /* a change not saved yet: before the loader takes over */
    kb_guard.pending = 0;                      /* intentional reset */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);                 /* record into RAM, core reset (fm1_sys.h): the loader takes over */
}

/* ---------------------------------------------------------------- kbcore.c's hooks */
static void enter_uboot(void);
static uint32_t kb_now(void) { return ms; }
static int kb_out_ready(void) { return hid_ready(); }
static uint32_t kb_out_room(void) { return hid_room(); }
static int kb_out_push(const uint8_t r[8]) { return hid_push(r); }
static void kb_uboot(void) { enter_uboot(); }
#if FELUCCA_MEDIA
static int kb_media_ok(void) { return hid_ready(); }
static uint32_t kb_media_room(void) { return media_room(); }
static int kb_media_push(const uint8_t *r, uint32_t n) { return media_push(r, n); }
#else
static int kb_media_ok(void) { return 0; }  /* the console flavour: EP2 is the console's, no media keys */
static uint32_t kb_media_room(void) { return 0; }
static int kb_media_push(const uint8_t *r, uint32_t n) { (void)r; (void)n; return 0; }
#endif
static void kb_diag(uint8_t *out);          /* (below: needs store.c's counters) */

static void enter_uboot(void)
{
    cfg_flush();
    lcd_fill(0, 0, 240, 240, 0);
    text_draw(110, "UBOOT", C_WHITE, 0);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    kb_guard.pending = 0;                      /* intentional: not a failed boot */
    fm1_enter_uboot();
}

/* ---------------------------------------------------------------- store.c's hooks: felucca-bsp's flash driver */
static int st_erase(uint32_t off)
{
    uint32_t took;
    return flash_ok ? fl_erase4k(off, &took) : -1;      /* IRQs off for the erase (~45 ms typical) */
}
static int st_write(uint32_t off, const void *p, uint32_t n) { return flash_ok ? fl_write(off, p, n) : -1; }
static int st_read(uint32_t off, void *p, uint32_t n) { return ota_fread(off, p, n); }

/* save a change once nothing has changed for SAVE_IDLE_MS and no control is held (an erase stops the key scan and
 * USB for tens of ms); at once before UBOOT or an update */
#define SAVE_IDLE_MS 3000u
static uint32_t saved_at;
static void cfg_flush(void)
{
    if (cfg.dirty && flash_ok && st_save() == 0)
        saved_at = ms;
}
static void save_service(void)
{
    if (!cfg.dirty || ms - cfg.changed_ms < SAVE_IDLE_MS || fm1_in.buttons || fm1_in.notes)
        return;
    if (st.errors > 8u) {                   /* the flash refuses: stop trying (the screen says so) */
        cfg.dirty = 0;
        return;
    }
    cfg_flush();
}

/* the vendor status query (kbcore.c, VIA get keyboard value 0x80): flash store counters, LE u32s */
static void kb_diag(uint8_t *out)
{
    const uint32_t v[6] = {st.saves, st.errors, st.loaded, st.seq, st.slot, cfg.dirty};
    uint32_t i;
    for (i = 0; i < 6u; i++)
        wr32(out + 4u * i, v[i]);
}

/* ---------------------------------------------------------------- the screen (ui.c) */
enum { S_HID_OFF, S_NO_USB, S_NO_HOST, S_SUSPENDED, S_READY };
static uint32_t usb_state(void)
{
    if (!usb_hid_on)
        return S_HID_OFF;
    if (!usb.up)
        return S_NO_USB;
    if (!usb.config)
        return S_NO_HOST;
    if (usb.suspended)
        return S_SUSPENDED;
    return S_READY;
}

static void screen_service(void)
{
    ui_status_t us;
    if (screen_dirty) {                     /* everything again (after the update screens, at start) */
        screen_dirty = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        ui_invalidate();
        un.drawn_page = 0xFF;
        un.cleared = 0;
    }
    us.usb = (uint8_t)usb_state();
    us.vial = FELUCCA_RAWHID ? (uint8_t)(vl.in_progress ? 2u : vl.unlocked) : 0u;
    us.unlock_permille = (uint16_t)unlock_permille();
    us.saved = saved_at && ms - saved_at < 1000u;
    us.save_error = st.errors > 8u;
    us.now = ms;
    ui_normal(&us);
}

/* ---------------------------------------------------------------- the LEDs: a level per control (ui.c / editor.c
 * decide it) -> felucca-bsp's LED bits: lit (fm1_led), the glow (fm1_led_dim), mid (fm1_led_mid + the glow), breath
 * (fm1_led_breath); blinking here, in time */
static uint8_t led_col[KB_NPOS], led_row[KB_NPOS], led_shown[KB_NPOS];

static void leds_init(void)
{
    uint32_t p, r, id;
    for (id = 0; id < KB_NPOS; id++) {
        led_col[id] = 0xFF;
        led_shown[id] = 0xFF;
    }
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++) {
            int32_t k = FM1_KEYMAP[r][p];
            if (k >= 0 && k < (int32_t)KB_NPOS) {
                led_col[k] = (uint8_t)p;
                led_row[k] = (uint8_t)r;
            }
        }
}

static void led_put(uint32_t id, uint32_t lv)
{
    uint32_t c = led_col[id];
    uint8_t m = (uint8_t)(1u << led_row[id]);
    if (c == 0xFFu || led_shown[id] == lv)
        return;
    led_shown[id] = (uint8_t)lv;
    fm1_led[c] = (uint8_t)(lv == LV_ON ? fm1_led[c] | m : fm1_led[c] & ~m);
    fm1_led_dim[c] = (uint8_t)(lv == LV_DIM || lv == LV_MID ? fm1_led_dim[c] | m : fm1_led_dim[c] & ~m);
    fm1_led_mid[c] = (uint8_t)(lv == LV_MID ? fm1_led_mid[c] | m : fm1_led_mid[c] & ~m);
    fm1_led_breath[c] = (uint8_t)(lv == LV_BREATHE ? fm1_led_breath[c] | m : fm1_led_breath[c] & ~m);
}

static void leds_service(uint32_t buttons, uint32_t notes)
{
    uint32_t p, fast = (ms / 125u) & 1u, slow = (ms / 500u) & 1u;
    for (p = 0; p < KB_NPOS; p++) {
        uint32_t held = p < KB_NBTN ? buttons >> p & 1u : notes >> (p - KB_NBTN) & 1u;
        uint32_t lv = ed_on() ? ed_led(p) : ui_led_normal(p, held);
        if (lv == LV_BLINK)
            lv = fast ? LV_ON : LV_OFF;
        else if (lv == LV_BLINK_SLOW)
            lv = slow ? LV_ON : LV_DIM;
        led_put(p, lv);
    }
}

#if FELUCCA_RAWHID
/* ---------------------------------------------------------------- Vial: raw HID requests -> kbcore.c vial_cmd */
static uint8_t raw_reply[RAW_N], raw_reply_pending, uboot_after_reply;
static uint32_t raw_reqs, uboot_at;

static void raw_service(void)
{
    uint8_t r[RAW_N];
    uint32_t i;
    if (raw_reply_pending && raw_send(raw_reply))
        raw_reply_pending = 0;
    if (uboot_after_reply) {                    /* bootloader jump: the reply first, then ~100 ms (via.c) */
        if (!raw_reply_pending && !raw.out_full && ms - uboot_at > 100u)
            enter_uboot();
        return;
    }
    if (raw_reply_pending || !raw_take(r))
        return;
    raw_reqs++;
    if (vial_cmd(r)) {
        uboot_after_reply = 1;
        uboot_at = ms;
    }
    for (i = 0; i < RAW_N; i++)
        raw_reply[i] = r[i];
    raw_reply_pending = 1;
    if (raw_send(raw_reply))
        raw_reply_pending = 0;
}
#endif

/* ---------------------------------------------------------------- USB serial console (the console flavour) */
#if KB_CONSOLE
static void put(char c)
{
    if (!cdc.dtr || co_w - co_r >= CO_N)
        return;
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}
static void puts_(const char *s) { while (*s) put(*s++); }
static void putd(const char *k, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    puts_(k);
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        put(b[--n]);
    put(' ');
}
static void puthex(const char *k, uint32_t v, uint32_t digits)
{
    puts_(k);
    while (digits--)
        put("0123456789ABCDEF"[(v >> (4u * digits)) & 15u]);
    put(' ');
}

static void status(void)
{
    uint32_t i;
    putd("ms=", ms);
    putd("cfg=", usb.config);
    putd("susp=", usb.suspended);
    putd("hid=", usb_hid_on);
    puthex("leds=", hid.leds, 2);
    putd("tx=", hid.tx_pkts);
    putd("queued=", hq_w - hq_r);
    putd("layer=", cfg.layer);
    putd("top=", layer_top());
    putd("temp=", kb.ls_n);
    putd("reports=", kb.reports);
    putd("rollovers=", kb.rollovers);
    putd("taps_dropped=", kb.taps_dropped);
    putd("editor=", kb.editor_req);
    putd("saves=", st.saves);
    putd("save_errors=", st.errors);
    putd("loaded=", st.loaded);
    putd("dirty=", cfg.dirty);
    puts_("report=");
    for (i = 0; i < 8u; i++)
        puthex("", kb.sent[i], 2);
    puthex("btn=", fm1_in.buttons, 4);
    puthex("notes=", fm1_in.notes, 7);
    putd("boots=", kb_crumbs.boots);
    puts_("\r\n");
}

static void command(char c)
{
    switch (c) {
    case 'h':
        puts_("fm1_keyboard: h help, p status, k keyboard on/off (re-enumerates), u UBOOT, r reboot\r\n");
        break;
    case 'p':
        status();
        break;
    case 'k':
        puts_(usb_hid_on ? "keyboard off\r\n" : "keyboard on\r\n");
        fm1_delay_ms(30);
        usb_hid_switch(!usb_hid_on);
        break;
    case 'u':
        puts_("UBOOT\r\n");
        enter_uboot();
        break;
    case 'r':
        puts_("reboot\r\n");
        cfg_flush();
        fm1_delay_ms(30);
        usb_detach();
        kb_guard.pending = 0;
        fm1_reboot();
        break;
    }
}
#endif

/* ---------------------------------------------------------------- main loop */
/* the buttons held at power-on (Dinghy's recovery_key: a polled scan for 50 ms): OCT- the keyboard stays off,
 * OCT+ UBOOT */
static uint32_t boot_keys(void)
{
    uint32_t start = fm1_ticks();
    fm1_input_init();
    do {
        fm1_wdt_feed();
        fm1_input_scan();
    } while ((uint32_t)(fm1_ticks() - start) < 50u * 1000u * FM1_TICKS_PER_US);
    return fm1_in.buttons;
}

static void kb_setup(void)
{
    stage(ST_FLASH);
    {   /* the flash: the expected part, then any staging left by an update cleared (Felucca's boot order) */
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
        irq_restore(f);
        if (flash_ok)
            ota_boot_cleanup();
    }
    kb_init();                              /* the defaults, then the saved configuration over them (store.c) */
    if (flash_ok)
        st_load();
    stage(ST_LCD);
    lcd_init();
    text_draw(100, "FM-1 KEYBOARD", C_WHITE, C_BLACK);
    stage(ST_IO);
    {
        uint32_t keys = boot_keys();
        if (keys >> BTN_OCTUP & 1u) {       /* UBOOT, before USB starts (nothing to detach, nothing unsaved) */
            text_draw(110, "UBOOT", C_WHITE, C_BLACK);
            kb_guard.pending = 0;
            fm1_enter_uboot();
        }
        if (keys >> BTN_OCTDN & 1u)
            usb_hid_switch(0);              /* before usb_start: MIDI only (+ the console in that flavour) */
    }
    fm1_input_init();                       /* afresh for the TIMER5 scan */
    leds_init();
    stage(ST_USB);
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    stage(ST_IRQ);
    fm1_irq_enable_all();
    /* (the backlight is on since fm1_lcd_hw_init: PA2 shares port A with the key scan, so no write to it here,
     * with TIMER5 running; felucca-bsp fm1_lcd_hw.h) */
    screen_dirty = 1;
    stage(ST_LOOP);
}

static void kb_loop_once(void)
{
    uint32_t now = ms, rel, pressed, notes_pressed, e;
    int32_t enc[KB_NENC];
    fm1_wdt_feed();
    usb_retry(now);
    if (kb_guard.pending && now > 30000u) {        /* a crash or hang in the first 30 s counts (Felucca's) */
        kb_guard.pending = 0;
        kb_guard.failed = 0;
    }
    if (usb.uboot_req)                          /* SysEx F0 22 24 35 7D F7 from the host */
        enter_uboot();
    mi_r = mi_w;                                /* USB MIDI in: not used, but drained, or a full ring NAKs the */
    midi_in_overflow = 0;                       /* host and the UBOOT soft key above could not get through */
    ota_service();                              /* the updater's handshake: our identity */
    if (usb.ota_req) {                          /* the updater's upgrade command */
        usb.ota_req = 0;
        if (flash_ok)
            ota_session();                      /* returns only if nothing was committed */
        else
            ota_show(9, -15);
        screen_dirty = 1;
    }
#if KB_CONSOLE
    while (ci_r != ci_w) {
        char c = (char)cdc_in[ci_r % CI_N];
        ci_r++;
        command(c);
    }
#else
    raw_service();
#endif
    /* the panel -> the engine, or the editor while it is open (the chord opens it) */
    pressed = fm1_input_edges(&rel);
    notes_pressed = fm1_input_note_edges();
    for (e = 0; e < KB_NENC; e++)
        enc[e] = fm1_enc_take(e);
    if (ed_on()) {
        ed_input(fm1_in.buttons, fm1_in.notes, pressed, notes_pressed, enc, now);
        if (!ed_on())
            screen_dirty = 1;                   /* left: the NORMAL screen again */
    } else {
        kb_input(fm1_in.buttons, fm1_in.notes, pressed, notes_pressed, enc);
        kb.alg_steps = 0;                       /* (SELECT: the screen takes it) */
        if (kb.editor_req) {
            kb.editor_req = 0;
            ed_enter(now);
        }
    }
    kb_task();
    save_service();
    leds_service(fm1_in.buttons | pressed, fm1_in.notes | notes_pressed);
    if (ed_on())
        ed_draw(ms, FELUCCA_ID, st.saves, st.loaded);
    else
        screen_service();
}

static void kb_boot(void)                    /* WDT, boot-loop guard, memory: before anything else */
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_wdt_arm(0x0D);
    if (kb_guard.magic != GUARD_MAGIC) {
        kb_guard.magic = GUARD_MAGIC;
        kb_guard.failed = 0;
        kb_guard.pending = 0;
    }
    if (kb_guard.pending)
        kb_guard.failed++;
    kb_guard.pending = 1;
    if (kb_guard.failed >= 2u) {
        kb_guard.failed = 0;
        kb_guard.pending = 0;
        fm1_enter_uboot();
    }
    if (kb_crumbs.magic != CRUMBS_MAGIC) {
        kb_crumbs.magic = CRUMBS_MAGIC;
        kb_crumbs.boots = 0;
        kb_crumbs.max_stage = 0;
    }
    kb_crumbs.boots++;
    stage(ST_BOOT);
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* the flash driver: runs from RAM while the flash is off */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
}

void fm1_cstart(void)
{
    kb_boot();
    kb_setup();
    for (;;)
        kb_loop_once();
}
