/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* USB full-speed device on USB0: a
 * class-compliant USB-MIDI interface. All SIE traffic happens in one place,
 * usb_poll(), called from the TIMER5 ISR at 2 kHz, so INDEX is never shared
 * and no USB IRQ is needed. MIDI in goes to a ring the audio ISR drains;
 * MIDI out (the panel keys) comes from a ring the audio ISR fills.
 * SysEx F0 22 24 35 7D F7 (the stock soft key, tools/fm1_softkey.py) asks
 * the main loop to enter UBOOT. VID 0x1209 / PID 0x0001 is the pid.codes
 * TEST id: fine for the bench, must be replaced before any release.
 * FELUCCA_CDC=1 adds a CDC-ACM serial function (IAD composite: EP2 notify,
 * EP3 bulk data) for the console in console.c; FELUCCA_USB_LAYOUT picks how
 * the composite presents itself, usb_cdc_switch / FELUCCA_CDC_DEFAULT=0 leave
 * the console out (descriptors, below).
 * FELUCCA_UAC=1 adds a USB audio input (UAC1, class compliant): the master
 * output as 16-bit stereo at 44.1 kHz on an isochronous asynchronous IN
 * endpoint (EP4), so a computer can record the FM-1 over the cable. audio.c
 * fills a ring (uac_render_start, uac_tap); uac_service sends one packet per
 * USB frame from the TIMER5 ISR, nested in the render too (main.c). The
 * update loader leaves both off: MIDI only.
 * FELUCCA_UAC_48K=1 (the default with FELUCCA_UAC) offers 48 kHz too: the host picks the rate with
 * SET_CUR (UAC1 sampling frequency control, two discrete rates in the one alt setting). The engines stay
 * at 44.1 kHz; at 48 kHz uac_tap resamples (polyphase FIR 160 / 147, uac_fir.h) into the ring. 44.1 kHz,
 * the rate until a host asks for another, sends the samples as before, bit for bit.
 * FELUCCA_HID=1 (fm1_keyboard) adds a HID keyboard interface, last in the configuration: boot subclass and
 * protocol, the 8-byte boot report (modifiers, reserved, 6 keys; no report ID), the LED output report through
 * SET_REPORT on EP0, an interrupt IN endpoint FELUCCA_HID_EP (4 by default: free without the audio input; 2 needs
 * FELUCCA_CDC=0). MUSB sets an interrupt IN endpoint up as a bulk one: only ISO has a TXCSR bit (Linux
 * musb_gadget.c). The main loop queues reports (hid_push); usb_poll sends one per USB frame at most.
 * usb_hid_on (FELUCCA_HID_DEFAULT at boot; usb_hid_switch re-enumerates) 0 presents the device without it,
 * byte for byte as a FELUCCA_HID=0 build. bcdDevice 4.xx with HID, so a host never mixes the two up.
 * FELUCCA_RAWHID=1 (with FELUCCA_HID, without FELUCCA_CDC) adds a second HID interface after the keyboard: QMK's
 * raw HID as VIA and Vial expect it (usage page 0xFF60, usage 0x61, 32-byte input and output reports, no report
 * ID) on EP FELUCCA_RAWHID_EP (3: the console's bulk pair) IN + OUT, interrupt. One report each way is held:
 * raw_take (main loop) frees the OUT side, raw_send queues the IN side; a host write may also come as SET_REPORT.
 * FELUCCA_MEDIA=1 (with FELUCCA_RAWHID) adds a third HID interface, last: mouse (report ID 1: 8 buttons, X, Y,
 * wheel, pan), system control (ID 2: power, sleep, wake) and consumer control (ID 3: volume, media, browser keys),
 * QMK's report layouts, on EP2 IN (interrupt; free without the console). media_push queues a report (its ID first). */
#include "fm1_usb.h"   /* registers (chip/) */
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 0
#endif
#ifndef FELUCCA_UAC_48K
#define FELUCCA_UAC_48K 1
#endif
#if !FELUCCA_UAC
#undef FELUCCA_UAC_48K
#define FELUCCA_UAC_48K 0
#endif
#ifndef FELUCCA_HID
#define FELUCCA_HID 0
#endif
#ifndef FELUCCA_HID_EP
#define FELUCCA_HID_EP 4
#endif
#ifndef FELUCCA_HID_DEFAULT
#define FELUCCA_HID_DEFAULT 1
#endif
#ifndef FELUCCA_HID_INTERVAL
#define FELUCCA_HID_INTERVAL 1   /* ms between the host's IN tokens */
#endif
#if FELUCCA_HID && FELUCCA_HID_EP == 4 && FELUCCA_UAC
#error "FELUCCA_HID_EP 4 is the audio input's endpoint"
#endif
#if FELUCCA_HID && FELUCCA_HID_EP == 2 && FELUCCA_CDC
#error "FELUCCA_HID_EP 2 is the console's notify endpoint"
#endif
#if FELUCCA_HID && FELUCCA_HID_EP != 2 && FELUCCA_HID_EP != 4
#error "FELUCCA_HID_EP is 2 or 4"
#endif
#ifndef FELUCCA_RAWHID
#define FELUCCA_RAWHID 0
#endif
#ifndef FELUCCA_RAWHID_EP
#define FELUCCA_RAWHID_EP 3
#endif
#if FELUCCA_RAWHID && (!FELUCCA_HID || FELUCCA_CDC)
#error "FELUCCA_RAWHID needs FELUCCA_HID and no FELUCCA_CDC (EP3 is the console's)"
#endif
#if FELUCCA_RAWHID && FELUCCA_RAWHID_EP != 3
#error "FELUCCA_RAWHID_EP is 3 (an endpoint with IN and OUT buffers, other than the keyboard's)"
#endif
#ifndef FELUCCA_MEDIA
#define FELUCCA_MEDIA 0
#endif
#if FELUCCA_MEDIA && !FELUCCA_RAWHID
#error "FELUCCA_MEDIA needs FELUCCA_RAWHID (its interface comes after the raw one, on EP2: no console)"
#endif
#define MEDIA_EP 2
enum { S_FADDR = 0, S_POWER = 1, S_INTRTX1 = 2, S_INTRTX2 = 3, S_INTRRX1 = 4, S_INTRRX2 = 5, S_INTRUSB = 6,
       S_INTRTX1E = 7, S_INTRTX2E = 8, S_INTRRX1E = 9, S_INTRRX2E = 10, S_INTRUSBE = 11, S_FRAME1 = 12,
       S_FRAME2 = 13, S_INDEX = 14,
       S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
       S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23 };

static uint8_t ep0buf[64 + 4] __attribute__((aligned(4)));
static uint8_t ep1tx[64] __attribute__((aligned(4)));
static uint8_t ep1rx[64 + 4] __attribute__((aligned(4)));
#if FELUCCA_CDC
static uint8_t ep2tx[8] __attribute__((aligned(4)));
static uint8_t ep3tx[64] __attribute__((aligned(4)));
static uint8_t ep3rx[64 + 4] __attribute__((aligned(4)));
/* serial rings: in = host -> console (ISR writes), out = console -> host (ISR reads) */
#define CI_N 256u
#define CO_N 2048u
static uint8_t cdc_in[CI_N], cdc_out[CO_N];
static volatile uint32_t ci_w, ci_r, co_w, co_r;
static struct {
    uint8_t line[7];             /* line coding: just stored and echoed back */
    volatile uint8_t dtr;        /* host has the port open */
    uint8_t e0_rx;               /* SET_LINE_CODING data stage pending */
    uint8_t rx_pend;             /* EP3 OUT packet seen, not yet taken (the ring was full) */
    uint32_t rx_pkts, tx_pkts;
} cdc = {{0x00, 0xC2, 0x01, 0x00, 0, 0, 8}};   /* 115200 8N1 */
#endif

#if FELUCCA_UAC
/* USB audio input. The I2S clock is not locked to USB (about 44,117.6 Hz against the host's 44,100):
 * the endpoint is asynchronous, so each 1 ms packet carries what the ring holds, 44 or 45 frames
 * (the 44.1 pattern) and one more or one fewer while the ring's fill drifts out of its band.
 * The render writes HALF_FRAMES (128) frames every 2.9 ms; the fill measured when a render starts is
 * the low point of that saw tooth, and is kept in UA_LO..UA_HI. A frame waits in the ring for that fill
 * plus its place in the render: 80..256 frames, ~3.8 ms on average (256-frame halves and a 96..160
 * band: ~5.8 ms). Measured under load (65 % CPU): a 64..112 band started renders at
 * 48..119 with no glitch; one render was once seen 29 frames under the floor: a packet (46) must
 * still be in hand then. */
#define UA_N 512u                /* ring, frames (16-bit L | R << 16); a power of two */
#define UA_PRIME 104u            /* silence ahead of the first rendered frame */
#define UA_LO 80u                /* a render may start late: keep a packet and more in hand */
#define UA_HI 128u
#define UAC_RATE 44100u
#if FELUCCA_UAC_48K
/* 48 kHz: the same times in 48 kHz frames (x 160 / 147); a render of HALF_FRAMES gives 139..140 frames */
#define UA_PRIME48 113u
#define UA_LO48 87u
#define UA_HI48 139u
#define UA_REND48 ((HALF_FRAMES * 160u + 146u) / 147u)
#define UA_MAXF 49u              /* frames in the largest packet: 48 + 1 */
_Static_assert(UA_HI48 + UA_REND48 + UA_MAXF < UA_N, "the ring holds the band, a render and a packet (48 kHz)");
#else
#define UA_MAXF 46u              /* frames in the largest packet */
#endif
#define UAC_MAXP (UA_MAXF * 4u)
_Static_assert(UA_HI + HALF_FRAMES + UA_MAXF < UA_N, "the ring holds the band, a render and a packet");
static uint32_t ua_ring[UA_N];
static volatile uint32_t ua_w, ua_r;
static uint32_t ep4tx[UA_MAXF] __attribute__((aligned(4)));
static struct {
    volatile uint8_t alt;        /* streaming interface setting: 1 = the host records */
    volatile uint8_t r48;        /* the host's rate (SET_CUR): 1 = 48 kHz, 0 = 44.1 (bus reset) */
    uint8_t ring48;              /* the rate the ring was primed for (the render's; a change re-primes) */
    volatile uint8_t go;         /* the ring is primed: the consumer reads it (0: silence packets) */
    volatile uint8_t flowing;    /* the host takes our packets (it may set alt 1 well before it reads) */
    uint8_t queued;              /* a packet was queued since the stream (re)started */
    uint8_t feed;                /* this render feeds the ring (sampled when it starts) */
    uint8_t wait;                /* polls the queued packet has waited */
    uint8_t e0_rx;               /* SET_CUR sampling frequency: data stage pending */
    volatile uint32_t fill_min;  /* ring fill when the last render started */
    uint32_t acc;                /* 44.1 frames per ms: 0.1 frame steps */
    uint32_t last;               /* the last frame sent, repeated on an underrun */
    uint32_t starts, pkts, frames;               /* frames in packets sent from the ring */
    uint32_t underruns, overruns, missed;        /* glitches: ring empty (repeat), ring full (drop), no packet ready */
    uint32_t stalls;                             /* the host stopped reading for > 20 ms (alt still 1) */
    uint32_t adj_up, adj_down;                   /* packets one frame longer / shorter than the pattern */
    uint32_t fill_lo, fill_hi;                   /* range of fill_min since the stream started */
    uint32_t rate_sets;                          /* SET_CUR sampling frequency requests */
} uac;
#if FELUCCA_UAC_48K
#include "uac_fir.h"
/* the resampler's input history: the last 32 frames, written twice so a window of UAC_FIR_N never wraps */
#define RS_H 32u
_Static_assert(RS_H >= UAC_FIR_N && (RS_H & (RS_H - 1u)) == 0, "the history holds a window");
static struct {
    int16_t l[2u * RS_H], r[2u * RS_H];   /* r right after l (the FIR loop reads both from one pointer) */
    uint32_t w;                  /* input frames written */
    uint32_t ph;                 /* the next output's phase, 0..159 (its newest input frame: the next written) */
#if FELUCCA_UAC_TONE
    uint32_t tone;
#endif
} rs;
#endif
#endif

static struct {
    uint8_t up, config, pend_addr, has_pend_addr, e0_tx, e0_zlp;
    const uint8_t *e0_src;
    uint16_t e0_left;
    uint32_t resets, setups, rx_pkts, tx_pkts, sof_seen, timeouts, no_sof;
    uint32_t suspends, max_gap, retries, frame_stalls;   /* diagnostics: suspend events, longest run of polls without SOF */
    uint16_t frame, frame_same;  /* last SOF frame number and how long it has not moved (x 10 ms) */
    uint8_t detached;            /* usb_detach() was called: no retry */
    volatile uint8_t suspended;  /* bus idle > 3 ms (unplugged or host asleep): the UI hides "USB" */
    uint8_t last_setup[8];
    uint8_t sysex[16];
    uint8_t sx_len, sx_on;
    uint8_t rx_pend;             /* EP1 OUT packet seen, not yet taken (the MIDI ring was too full) */
    volatile uint8_t uboot_req;
    volatile uint8_t ota_req;    /* F0 22 24 35 7F F7: M-UPGRADE upgrade command (FELUCCA_OTA) */
} usb;

#if FELUCCA_OTA
/* M-UPGRADE SysEx (ota.c): one received frame at a time (7-bit bytes between
 * F0 and F7; frames arriving while one is pending are dropped, the host
 * retries), and a TX ring of SysEx event packets sent before any MIDI. */
static uint8_t sx_frame[640];
static uint32_t sx_pos;
static volatile uint32_t sx_frame_len;
static volatile uint8_t sx_ready, sx_collect, sx_busy;
#define SXQ 64u
static uint32_t sx_out_q[SXQ];
static volatile uint32_t so_w, so_r;
#endif

/* MIDI rings: 4-byte USB-MIDI event packets */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_out_q[MQ];
static uint32_t midi_in_ms[MQ];
static uint8_t midi_in_source[MQ];                    /* 1 USB, 2 TRS */
static volatile uint32_t fm1_ms;                    /* also declared by core.h; parser-only host tests */
static volatile uint32_t mi_w, mi_r, mo_w, mo_r;
static volatile uint8_t midi_in_overflow;               /* audio ISR discards a broken stream and releases notes */

static int midi_enqueue(uint32_t pkt, uint32_t source)
{
    uint32_t at = mi_w % MQ;
    if (midi_in_overflow)
        return 0;
    if (mi_w - mi_r >= MQ) {
        midi_in_overflow = 1;
        return 0;
    }
    midi_in_q[at] = pkt;
    midi_in_ms[at] = fm1_ms;
    midi_in_source[at] = (uint8_t)source;
    RING_PUBLISH();
    mi_w++;
    return 1;
}

static void midi_out_event(uint32_t pkt)            /* from the audio ISR */
{
    if (usb.config && mo_w - mo_r < MQ) {
        midi_out_q[mo_w % MQ] = pkt;
        RING_PUBLISH();
        mo_w++;
    }
}

#if FELUCCA_HID
/* HID keyboard: a ring of 8-byte boot reports, main loop -> TIMER5 (one report per slot, the whole state of the
 * keys: a full ring makes hid_push fail and the main loop tries again, it never drops a state) */
#define HQ 16u
static uint8_t hid_q[HQ][8];
static volatile uint32_t hq_w, hq_r;
static uint8_t hidtx[8] __attribute__((aligned(4)));
static struct {
    uint8_t cur[8];              /* the last report queued: GET_REPORT */
    uint8_t idle;                /* SET_IDLE duration (4 ms units; 0 = only on change, all we do) */
    uint8_t protocol;            /* SET_PROTOCOL: 0 boot, 1 report (the same 8 bytes either way) */
    uint8_t e0_rx;               /* SET_REPORT: the LED byte follows */
    volatile uint8_t leds;       /* host's LEDs: bit 0 Num, 1 Caps, 2 Scroll, 3 Compose, 4 Kana */
    uint32_t tx_pkts, sets;      /* reports sent, LED reports received */
} hid = {{0}, 0, 1, 0, 0, 0, 0};
#endif

#if FELUCCA_RAWHID
/* raw HID: one 32-byte report held each way (request / reply, as VIA and Vial talk) */
#define RAW_N 32u
static uint8_t rawtx[RAW_N] __attribute__((aligned(4)));
static uint8_t rawrx[64 + 4] __attribute__((aligned(4)));   /* the OUT DMA buffer (a packet may be up to 64) */
static struct {
    uint8_t in[RAW_N];           /* host -> device, waiting for raw_take */
    uint8_t out[RAW_N];          /* device -> host, waiting for hid_tx's sibling raw_tx */
    volatile uint8_t in_full, out_full;
    uint8_t rx_pend;             /* EP3 OUT packet seen, not yet taken (in[] was full) */
    uint8_t e0_rx;               /* SET_REPORT to the raw interface: the report follows on EP0 */
    uint32_t rx_pkts, tx_pkts, e0_sets;
} raw;
#endif

#if FELUCCA_MEDIA
/* mouse / system / consumer reports: a ring, each up to 8 bytes with its length (report ID first) */
#define MQN 16u
static uint8_t media_q[MQN][8], media_qn[MQN];
static volatile uint32_t mdq_w, mdq_r;
static uint8_t mediatx[8] __attribute__((aligned(4)));
static uint32_t media_tx_pkts;
#endif

/* ------------------------------------------------------- descriptors --- */
/* Two functions: audio + MIDI (0 audio control, 1 MIDI streaming, 2 audio streaming with FELUCCA_UAC)
 * and, with FELUCCA_CDC, the CDC-ACM console (communication + data). The endpoints never move:
 * EP1 MIDI, EP2 CDC notify, EP3 CDC data, EP4 audio.
 * bcdDevice: 3.00 MIDI, +0.10 the audio input (+0.20 since 1.1: 44.1 and 48 kHz, FELUCCA_UAC_48K), +0.01 CDC
 * (+0.02 per FELUCCA_USB_LAYOUT step, so a host never reuses what it learnt about another layout).
 * FELUCCA_USB_LAYOUT, how the CDC build presents itself (#67: macOS 13-15 attach Apple's CDC composite
 * driver to a misc / IAD device that has a CDC function, and their kernel audio driver then never
 * takes the audio interfaces; macOS 26 / 27 (usbaudiod, matched per interface) and iOS / iPadOS work):
 *   0  1.0: device class EF 02 01 (misc / IAD), audio + MIDI IF 0.., CDC last         (the default)
 *   1  CDC first: CDC IF 0-1, then audio + MIDI, both with IADs, EF 02 01
 *   2  as 0 with device class 00 00 01: outside AppleUSBCDC's device personalities
 *      (02 * *, EF 02 01, 00 00 00); the IADs stay for Windows / Linux (test build)
 *   3  as 0 with device class 00 00 00 (test build: does AppleUSBCDC's "Vendor" personality take it?)
 * usb_cdc_on (FELUCCA_CDC_DEFAULT at boot; usb_cdc_switch re-enumerates): 0 presents the device without
 * the console, byte for byte as a FELUCCA_CDC=0 build: device class 0, audio + MIDI, no IAD. */
#ifndef FELUCCA_USB_LAYOUT
#define FELUCCA_USB_LAYOUT 0
#endif
#ifndef FELUCCA_CDC_DEFAULT
#define FELUCCA_CDC_DEFAULT 1
#endif
#ifndef FELUCCA_USB_PID
#define FELUCCA_USB_PID 0x0001   /* the update loader is 0x0002 */
#endif
#if FELUCCA_USB_LAYOUT < 0 || FELUCCA_USB_LAYOUT > 3
#error "FELUCCA_USB_LAYOUT is 0..3"
#endif

#define D_CFG(len, nif) 9, 2, (len) & 0xFF, (len) >> 8, (nif), 1, 0, 0x80, 50
#define D_IAD(first, n, cls, sub, proto) 8, 0x0B, (first), (n), (cls), (sub), (proto), 0
#if FELUCCA_UAC_48K
#define D_FMT 14, 0x24, 2, 1, 2, 2, 16, 2, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00,   /* 44100, 48000 */
#define FMT_LEN 14
#elif FELUCCA_UAC
#define D_FMT 11, 0x24, 2, 1, 2, 2, 16, 1, UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16,
#define FMT_LEN 11
#else
#define FMT_LEN 0
#endif
#if FELUCCA_UAC
#define D_AC(a)                                                                                      \
    9, 4, (a), 0, 0, 1, 1, 0, 0,                                                                     \
    10, 0x24, 1, 0x00, 0x01, 31, 0, 2, (a) + 1, (a) + 2,   /* AC header 1.00: MIDI + audio streaming */ \
    12, 0x24, 2, 1, 0x03, 0x06, 0, 2, 0x03, 0x00, 0, 0,    /* input terminal 1: line, 2 ch (L R) */      \
    9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0,                    /* output terminal 2: USB streaming, from 1 */
#define D_AS(s)                                                                                      \
    9, 4, (s), 0, 0, 1, 2, 0, 0,                           /* audio streaming, alt 0: no bandwidth */    \
    9, 4, (s), 1, 1, 1, 2, 0, 0,                           /* alt 1: the stream */                      \
    7, 0x24, 1, 2, 1, 0x01, 0x00,                          /* AS general: terminal 2, delay 1, PCM */   \
    D_FMT                                                  /* type I: 2 ch, 16 bit, the rates */        \
    9, 5, 0x84, 0x05, UAC_MAXP & 0xFF, UAC_MAXP >> 8, 1, 0, 0,   /* EP4 IN isochronous async, 1 ms */   \
    7, 0x25, 1, 0x01, 0, 0, 0,                             /* CS endpoint: sampling frequency control */
#else
#define D_AC(a) 9, 4, (a), 0, 0, 1, 1, 0, 0, 9, 0x24, 1, 0x00, 0x01, 9, 0, 1, (a) + 1,
#define D_AS(s)
#endif
#define D_MIDI(m)                                                                                    \
    9, 4, (m), 0, 2, 1, 3, 0, 0,                                                                     \
    7, 0x24, 1, 0x00, 0x01, 0x41, 0x00,                                                              \
    6, 0x24, 2, 1, 1, 0,                                                                             \
    6, 0x24, 2, 2, 2, 0,                                                                             \
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,                                                                    \
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,                                                                    \
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,                                                                   \
    5, 0x25, 1, 1, 1,                                                                                \
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,                                                                   \
    5, 0x25, 1, 1, 3,
#define D_AUDIO(a) D_AC(a) D_MIDI((a) + 1) D_AS((a) + 2)   /* audio + MIDI from interface a */
#define AUD_NIF (2 + FELUCCA_UAC)
#define AUD_LEN (92 + (63 + FMT_LEN) * FELUCCA_UAC)
#define D_CDC(c)                                                                                     \
    D_IAD((c), 2, 2, 2, 1),                                /* IAD: CDC ACM */                           \
    9, 4, (c), 0, 1, 2, 2, 1, 0,                           /* communication */                          \
    5, 0x24, 0x00, 0x10, 0x01,                             /* header 1.10 */                            \
    5, 0x24, 0x01, 0x00, (c) + 1,                          /* call management: the data IF */           \
    4, 0x24, 0x02, 0x02,                                   /* ACM: line coding + state */               \
    5, 0x24, 0x06, (c), (c) + 1,                           /* union */                                  \
    7, 5, 0x82, 3, 8, 0, 16,                               /* EP2 IN interrupt (never sent) */          \
    9, 4, (c) + 1, 0, 2, 0x0A, 0, 0, 0,                    /* data */                                   \
    7, 5, 0x03, 2, 64, 0, 0,                               /* EP3 OUT bulk */                           \
    7, 5, 0x83, 2, 64, 0, 0,                               /* EP3 IN bulk */
#define CDC_LEN 66                                         /* with its IAD */
#define BCD_LO(cdc) (0x10 * (FELUCCA_UAC + FELUCCA_UAC_48K) + ((cdc) ? 0x01 + 2 * FELUCCA_USB_LAYOUT : 0))
#define D_DEVH(bcdusb, cls, sub, proto, cdc, hi, ser)                                                \
    18, 1, (bcdusb) & 0xFF, (bcdusb) >> 8, (cls), (sub), (proto), 64, 0x09, 0x12, FELUCCA_USB_PID & 0xFF, \
    FELUCCA_USB_PID >> 8, BCD_LO(cdc), (hi), 1, 2, (ser), 1
#define D_DEV(bcdusb, cls, sub, proto, cdc) D_DEVH(bcdusb, cls, sub, proto, cdc, 0x03, 0)   /* 3.xx: no HID */

/* without the console (FELUCCA_CDC=0, or usb_cdc_on = 0): class 0, no IAD, as a plain USB-MIDI / audio device */
#define CFG_LEN_PLAIN (9 + AUD_LEN)
static const uint8_t DEV_DESC_PLAIN[18] = {D_DEV(0x0110, 0, 0, 0, 0)};
static const uint8_t CFG_DESC_PLAIN[] = {D_CFG(CFG_LEN_PLAIN, AUD_NIF), D_AUDIO(0)};
typedef char cfg_plain_len_ok[sizeof CFG_DESC_PLAIN == CFG_LEN_PLAIN ? 1 : -1];
#if FELUCCA_CDC
#define CFG_LEN (9 + 8 + AUD_LEN + CDC_LEN)
#define USB_NIF (AUD_NIF + 2)
#if FELUCCA_USB_LAYOUT == 1
#define AUD_IF_CDC 2u                                      /* the audio function's first interface */
#define CDC_IF_CDC 0u
#else
#define AUD_IF_CDC 0u
#define CDC_IF_CDC AUD_NIF
#endif
#if FELUCCA_USB_LAYOUT == 2
static const uint8_t DEV_DESC[18] = {D_DEV(0x0200, 0x00, 0x00, 0x01, 1)};
#elif FELUCCA_USB_LAYOUT == 3
static const uint8_t DEV_DESC[18] = {D_DEV(0x0200, 0x00, 0x00, 0x00, 1)};
#else
static const uint8_t DEV_DESC[18] = {D_DEV(0x0200, 0xEF, 0x02, 0x01, 1)};   /* misc / IAD */
#endif
static const uint8_t CFG_DESC[] = {
    D_CFG(CFG_LEN, USB_NIF),
#if FELUCCA_USB_LAYOUT == 1
    D_CDC(CDC_IF_CDC)
#endif
    D_IAD(AUD_IF_CDC, AUD_NIF, 1, 1, 0),                  /* IAD: audio + MIDI */
    D_AUDIO(AUD_IF_CDC)
#if FELUCCA_USB_LAYOUT != 1
    D_CDC(CDC_IF_CDC)
#endif
};
typedef char cfg_len_ok[sizeof CFG_DESC == CFG_LEN ? 1 : -1];
static uint8_t usb_cdc_on = FELUCCA_CDC_DEFAULT != 0;      /* the console is presented (read by the TIMER5 ISR) */
#define USB_CDC_ON usb_cdc_on
#define AUD_IF (usb_cdc_on ? AUD_IF_CDC : 0u)
#else
#define DEV_DESC DEV_DESC_PLAIN
#define CFG_DESC CFG_DESC_PLAIN
#define USB_CDC_ON 0
#define AUD_IF 0u
#endif
#define UAC_AS_IF (AUD_IF + 2u)

#if FELUCCA_HID
/* The HID keyboard, after every other interface (so the others keep their numbers): interface, HID descriptor,
 * interrupt IN endpoint. Report: the boot keyboard (HID 1.11 appendix B.1) with usages and logical maximum 255
 * (2-byte items: 1-byte 0xFF would read as -1), so F13..F24 (0x68..0x73) and the rest of page 7 are valid
 * (TinyUSB's TUD_HID_REPORT_DESC_KEYBOARD, the same bytes). */
static const uint8_t HID_RD[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                    /* Generic Desktop, Keyboard, Application */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x08, 0x75, 0x01, 0x81, 0x02,                    /* 8 modifier bits: Input (Data, Var, Abs) */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                    /* reserved byte: Input (Const) */
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01,
    0x91, 0x02,                                            /* 5 LEDs: Output (Data, Var, Abs) */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                    /* LED padding: Output (Const) */
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x95, 0x06, 0x75, 0x08, 0x81, 0x00,                    /* 6 key codes: Input (Data, Array, Abs) */
    0xC0,
};
#define HID_RD_LEN 65
typedef char hid_rd_len_ok[sizeof HID_RD == HID_RD_LEN ? 1 : -1];
#define D_HIDDESC 9, 0x21, 0x11, 0x01, 0, 1, 0x22, HID_RD_LEN & 0xFF, HID_RD_LEN >> 8   /* HID 1.11, 1 report desc. */
#define D_HID(i)                                                                                     \
    9, 4, (i), 0, 1, 3, 1, 1, 0,                           /* HID, boot subclass, keyboard */           \
    D_HIDDESC,                                                                                       \
    7, 5, 0x80 | FELUCCA_HID_EP, 3, 8, 0, FELUCCA_HID_INTERVAL,   /* interrupt IN, 8 bytes */
#define HID_LEN 25
static const uint8_t HID_DESC[9] = {D_HIDDESC};
#if FELUCCA_RAWHID
/* raw HID, as QMK's RAW_ENABLE presents it (VIA / Vial find it by usage page 0xFF60 and usage 0x61) */
static const uint8_t RAW_RD[] = {
    0x06, 0x60, 0xFF, 0x09, 0x61, 0xA1, 0x01,             /* Vendor 0xFF60, usage 0x61, Application */
    0x09, 0x62, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x20, 0x75, 0x08,
    0x81, 0x02,                                            /* 32 bytes: Input (Data, Var, Abs) */
    0x09, 0x63, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x20, 0x75, 0x08,
    0x91, 0x02,                                            /* 32 bytes: Output (Data, Var, Abs) */
    0xC0,
};
#define RAW_RD_LEN 34
typedef char raw_rd_len_ok[sizeof RAW_RD == RAW_RD_LEN ? 1 : -1];
#define D_RAWDESC 9, 0x21, 0x11, 0x01, 0, 1, 0x22, RAW_RD_LEN, 0
#define D_RAW(i)                                                                                     \
    9, 4, (i), 0, 2, 3, 0, 0, 0,                           /* HID, no subclass, no protocol */          \
    D_RAWDESC,                                                                                       \
    7, 5, 0x80 | FELUCCA_RAWHID_EP, 3, RAW_N, 0, 1,        /* interrupt IN, 32 bytes, 1 ms */           \
    7, 5, FELUCCA_RAWHID_EP, 3, RAW_N, 0, 1,               /* interrupt OUT */
#define RAW_LEN 32
#define RAW_NIF 1
#define RAW_IF (AUD_NIF + 1u)                              /* after the keyboard (no console: no CDC) */
static const uint8_t RAW_DESC[9] = {D_RAWDESC};
#define IS_RAW_IF(itf) (USB_HID_ON && (itf) == RAW_IF)
#else
#define D_RAW(i)
#define RAW_LEN 0
#define RAW_NIF 0
#endif
#if FELUCCA_MEDIA
/* mouse (ID 1), system control (ID 2), consumer control (ID 3): QMK's layouts (tmk_core report.h) */
static const uint8_t MEDIA_RD[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,   /* Mouse, ID 1, Pointer */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,   /* 8 buttons */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,                                                                /* X, Y, wheel: -127..127, Rel */
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,         /* AC Pan */
    0xC0, 0xC0,
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02, 0x19, 0x01, 0x2A, 0xB7, 0x00, 0x15, 0x01, 0x26,   /* System, ID 2 */
    0xB7, 0x00, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xC0,                      /* one 16-bit usage, Array */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x19, 0x01, 0x2A, 0xA0, 0x02, 0x15, 0x01, 0x26,   /* Consumer, ID 3 */
    0xA0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xC0,
};
#define MEDIA_RD_LEN 113
typedef char media_rd_len_ok[sizeof MEDIA_RD == MEDIA_RD_LEN ? 1 : -1];
#define D_MEDIADESC 9, 0x21, 0x11, 0x01, 0, 1, 0x22, MEDIA_RD_LEN, 0
#define D_MEDIA(i)                                                                                   \
    9, 4, (i), 0, 1, 3, 0, 0, 0,                           /* HID, no boot */                           \
    D_MEDIADESC,                                                                                     \
    7, 5, 0x80 | MEDIA_EP, 3, 8, 0, 1,                     /* interrupt IN, 8 bytes, 1 ms */
#define MEDIA_LEN 25
#define MEDIA_NIF 1
#define MEDIA_IF (AUD_NIF + 1u + RAW_NIF)
static const uint8_t MEDIA_DESC[9] = {D_MEDIADESC};
#define IS_MEDIA_IF(itf) (USB_HID_ON && (itf) == MEDIA_IF)
#else
#define D_MEDIA(i)
#define MEDIA_LEN 0
#define MEDIA_NIF 0
#endif
#ifdef FELUCCA_USB_SERIAL
#define HID_SER 3                                          /* the serial number string (Vial: "vial:f64c2b3c") */
#else
#define HID_SER 0
#endif
static const uint8_t DEV_DESC_PLAIN_HID[18] = {D_DEVH(0x0110, 0, 0, 0, 0, 0x04, HID_SER)};
static const uint8_t CFG_DESC_PLAIN_HID[] = {
    D_CFG(CFG_LEN_PLAIN + HID_LEN + RAW_LEN + MEDIA_LEN, AUD_NIF + 1 + RAW_NIF + MEDIA_NIF),
    D_AUDIO(0) D_HID(AUD_NIF) D_RAW(AUD_NIF + 1) D_MEDIA(AUD_NIF + 1 + RAW_NIF)};
typedef char cfg_plain_hid_len_ok[sizeof CFG_DESC_PLAIN_HID == CFG_LEN_PLAIN + HID_LEN + RAW_LEN + MEDIA_LEN ? 1 : -1];
#if FELUCCA_CDC
#if FELUCCA_USB_LAYOUT == 2
static const uint8_t DEV_DESC_HID[18] = {D_DEVH(0x0200, 0x00, 0x00, 0x01, 1, 0x04, HID_SER)};
#elif FELUCCA_USB_LAYOUT == 3
static const uint8_t DEV_DESC_HID[18] = {D_DEVH(0x0200, 0x00, 0x00, 0x00, 1, 0x04, HID_SER)};
#else
static const uint8_t DEV_DESC_HID[18] = {D_DEVH(0x0200, 0xEF, 0x02, 0x01, 1, 0x04, HID_SER)};   /* misc / IAD */
#endif
static const uint8_t CFG_DESC_HID[] = {
    D_CFG(CFG_LEN + HID_LEN, USB_NIF + 1),
#if FELUCCA_USB_LAYOUT == 1
    D_CDC(CDC_IF_CDC)
#endif
    D_IAD(AUD_IF_CDC, AUD_NIF, 1, 1, 0),                  /* IAD: audio + MIDI */
    D_AUDIO(AUD_IF_CDC)
#if FELUCCA_USB_LAYOUT != 1
    D_CDC(CDC_IF_CDC)
#endif
    D_HID(USB_NIF)
};
typedef char cfg_hid_len_ok[sizeof CFG_DESC_HID == CFG_LEN + HID_LEN ? 1 : -1];
#define HID_IF (usb_cdc_on ? USB_NIF : AUD_NIF)
#else
#define DEV_DESC_HID DEV_DESC_PLAIN_HID
#define CFG_DESC_HID CFG_DESC_PLAIN_HID
#define HID_IF AUD_NIF
#endif
static uint8_t usb_hid_on = FELUCCA_HID_DEFAULT != 0;      /* the keyboard is presented (read by the TIMER5 ISR) */
#define USB_HID_ON usb_hid_on
#if FELUCCA_HID_EP == 4
#define hid_ep_txbuf(p) fm1_usb_ep4_txbuf(p)
#define hid_ep_send(p, n) fm1_usb_ep4_send((p), (n))
#else
#define hid_ep_txbuf(p) fm1_usb_ep_txbuf(FELUCCA_HID_EP, (p))
#define hid_ep_send(p, n) fm1_usb_ep_send(FELUCCA_HID_EP, (p), (n))
#endif
#else
#define USB_HID_ON 0
#endif
static const uint8_t STR0[4] = {4, 3, 0x09, 0x04};
static const uint8_t STR1[] = {42, 3, 'H', 0, 0xFC, 0, 'g', 0, 'e', 0, 'l', 0, 't', 0, 'o', 0, 'n', 0, ' ', 0, 'I', 0,
                               'n', 0, 's', 0, 't', 0, 'r', 0, 'u', 0, 'm', 0, 'e', 0, 'n', 0, 't', 0, 's', 0};
#ifdef FELUCCA_LOADER
static const uint8_t STR2[] = {30, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0, 'a', 0, ' ', 0, 'U', 0, 'p', 0,
                               'd', 0, 'a', 0, 't', 0, 'e', 0};
#elif defined FELUCCA_USB_STR2
static const uint8_t STR2[] = {FELUCCA_USB_STR2};          /* the app's product name (string descriptor bytes) */
#else
static const uint8_t STR2[] = {16, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0, 'a', 0};
#endif

#if FELUCCA_HID && defined FELUCCA_USB_SERIAL
static const uint8_t STR3[] = {FELUCCA_USB_SERIAL};        /* string descriptor bytes */
#endif

static int get_desc(uint32_t wvalue, const uint8_t **d, uint16_t *l)
{
    switch (wvalue >> 8) {
    case 1:
#if FELUCCA_HID
        if (USB_HID_ON) {
            *d = USB_CDC_ON ? DEV_DESC_HID : DEV_DESC_PLAIN_HID;
            *l = 18;
            return 1;
        }
#endif
        *d = USB_CDC_ON ? DEV_DESC : DEV_DESC_PLAIN;
        *l = 18;
        return 1;
    case 2:
#if FELUCCA_HID
        if (USB_HID_ON) {
            *d = USB_CDC_ON ? CFG_DESC_HID : CFG_DESC_PLAIN_HID;
            *l = (uint16_t)(USB_CDC_ON ? sizeof CFG_DESC_HID : sizeof CFG_DESC_PLAIN_HID);
            return 1;
        }
#endif
        *d = USB_CDC_ON ? CFG_DESC : CFG_DESC_PLAIN;
        *l = (uint16_t)(USB_CDC_ON ? sizeof CFG_DESC : sizeof CFG_DESC_PLAIN);
        return 1;
    case 3:
        switch (wvalue & 0xFFu) {
        case 0:
            *d = STR0;
            *l = sizeof STR0;
            return 1;
        case 1:
            *d = STR1;
            *l = sizeof STR1;
            return 1;
        case 2:
            *d = STR2;
            *l = sizeof STR2;
            return 1;
#if FELUCCA_HID && defined FELUCCA_USB_SERIAL
        case 3:                                         /* only the HID descriptors name it */
            if (!USB_HID_ON)
                return 0;
            *d = STR3;
            *l = sizeof STR3;
            return 1;
#endif
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------- the SIE --- */
static void sie_wr(uint32_t r, uint32_t v)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return;
    fm1_usb_sie_wr_start(r, v);
    while (!fm1_usb_sie_done() && --n)
        ;
    if (n)
        usb.timeouts = 0;                               /* consecutive failures only */
    else if (++usb.timeouts > 50u)
        usb.up = 0;                                     /* SIE dead (no clock?): stop polling it */
}

static uint32_t sie_rd(uint32_t r)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return 0;
    fm1_usb_sie_rd_start(r);
    while (!fm1_usb_sie_done())
        if (!--n) {
            if (++usb.timeouts > 50u)
                usb.up = 0;
            return 0;
        }
    usb.timeouts = 0;
    return fm1_usb_sie_data();
}

#if FELUCCA_UAC
static void uac_ep4_reset(void)                         /* drop a queued packet; isochronous mode */
{
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXCSR1, 0x48);                             /* ClrDataTog + FlushFIFO (SDK usb_g_ep_config) */
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_TXCSR1, 0);
    sie_wr(S_TXCSR2, 0x40);                             /* ISO */
}

static void uac_stream(uint32_t alt)                    /* SET_INTERFACE, configuration, bus reset */
{
    uac.go = 0;                                         /* the next render primes the ring again */
    uac.flowing = 0;                                    /* ... once the host has taken a packet */
    uac.queued = 0;
    uac.wait = 0;
    RING_PUBLISH();
    uac.alt = (uint8_t)alt;
    uac.acc = 0;
    uac.last = 0;
    if (alt) {                                          /* (the fill range of the last stream stays readable) */
        uac.starts++;
        uac.fill_lo = 0xFFFFFFFFu;
        uac.fill_hi = 0;
    }
}

/* SET_CUR sampling frequency (any time: before or after SET_INTERFACE; a bus reset goes back to 44.1).
 * 48 kHz when built with it and asked for (the nearer of the two), else 44.1. A change restarts the
 * stream: silence packets until the next render primes the ring at the new rate. */
static void uac_rate_set(uint32_t hz)
{
    uint8_t r48 = FELUCCA_UAC_48K && hz >= (UAC_RATE + 48000u) / 2u;
    if (r48 == uac.r48)
        return;
    uac.go = 0;
    RING_PUBLISH();
    uac.r48 = r48;
    uac.acc = 0;
    uac.pkts = uac.frames = 0;                          /* the measured rate (console) starts again */
}
#endif

static void ep1_config(void)
{
    fm1_usb_ep_txbuf(1, ep1tx);
    fm1_usb_ep_rxbuf(1, ep1rx);
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x02);
    fm1_usb_ep_enable(1u << 1);
#if FELUCCA_CDC
    if (!usb_cdc_on)
        goto no_cdc;                                    /* presented without the console: EP2 / EP3 unused */
    fm1_usb_ep_txbuf(2, ep2tx);
    sie_wr(S_INDEX, 2);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    fm1_usb_ep_txbuf(3, ep3tx);
    fm1_usb_ep_rxbuf(3, ep3rx);
    sie_wr(S_INDEX, 3);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x0A);
    fm1_usb_ep_enable((1u << 2) | (1u << 3));
    cdc.rx_pend = 1;                                    /* look once: a packet may already wait */
no_cdc:;
#endif
#if FELUCCA_UAC
    fm1_usb_ep4_txbuf(ep4tx);
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXMAXP, 0xFF);
    uac_ep4_reset();
    fm1_usb_ep_enable(1u << 4);
    uac_stream(0);
#endif
#if FELUCCA_HID
    if (usb_hid_on) {                                   /* interrupt IN: a bulk endpoint to MUSB (no ISO bit) */
        hid_ep_txbuf(hidtx);
        sie_wr(S_INDEX, FELUCCA_HID_EP);
        sie_wr(S_TXMAXP, 0xFF);
        sie_wr(S_TXCSR1, 0x48);                         /* ClrDataTog + FlushFIFO */
        sie_wr(S_TXCSR2, 0);
        fm1_usb_ep_enable(1u << FELUCCA_HID_EP);
        hq_r = hq_w;                                    /* nothing stale: the app queues its state again */
#if FELUCCA_RAWHID
        fm1_usb_ep_txbuf(FELUCCA_RAWHID_EP, rawtx);     /* as the console's EP3 (interrupt = bulk to MUSB) */
        fm1_usb_ep_rxbuf(FELUCCA_RAWHID_EP, rawrx);
        sie_wr(S_INDEX, FELUCCA_RAWHID_EP);
        sie_wr(S_TXMAXP, 0xFF);
        sie_wr(S_TXCSR1, 0x48);
        sie_wr(S_TXCSR2, 0);
        sie_wr(S_RXMAXP, 0xFF);
        sie_wr(S_RXCSR1, 0x90);
        sie_wr(S_RXCSR2, 0);
        sie_wr(S_INTRRX1E, 0x02 | 1u << FELUCCA_RAWHID_EP);
        fm1_usb_ep_enable(1u << FELUCCA_RAWHID_EP);
        raw.in_full = 0;
        raw.out_full = 0;
        raw.rx_pend = 1;                                /* look once: a packet may already wait */
#endif
#if FELUCCA_MEDIA
        fm1_usb_ep_txbuf(MEDIA_EP, mediatx);
        sie_wr(S_INDEX, MEDIA_EP);
        sie_wr(S_TXMAXP, 0xFF);
        sie_wr(S_TXCSR1, 0x48);                         /* ClrDataTog + FlushFIFO */
        sie_wr(S_TXCSR2, 0);
        fm1_usb_ep_enable(1u << MEDIA_EP);
        mdq_r = mdq_w;
#endif
    }
#endif
}

static void e0_chunk(void)
{
    uint32_t n = usb.e0_left > 64u ? 64u : usb.e0_left, i;
    int last;
    for (i = 0; i < n; i++)
        ep0buf[i] = usb.e0_src[i];
    fm1_usb_ep0_send(ep0buf, n);
    usb.e0_src += n;
    usb.e0_left = (uint16_t)(usb.e0_left - n);
    last = usb.e0_left == 0 && !(n == 64u && usb.e0_zlp);
    if (usb.e0_left == 0 && n == 64u)
        usb.e0_zlp = 0;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, last ? 0x0A : 0x02);
    if (last)
        usb.e0_tx = 0;
}

static void e0_send(const uint8_t *p, uint16_t len, uint16_t wlen)
{
    if (len > wlen)
        len = wlen;
    usb.e0_src = p;
    usb.e0_left = len;
    usb.e0_zlp = (len < wlen) && (len % 64u == 0);
    usb.e0_tx = 1;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x40);
    e0_chunk();
}

static void ep0_service(void)
{
    static const uint8_t zero2[2];
    uint32_t csr, i;
    uint16_t wvalue, wlength;
    uint8_t *s = usb.last_setup;
    if (usb.has_pend_addr) {
        sie_wr(S_FADDR, usb.pend_addr);
        usb.has_pend_addr = 0;
    }
    sie_wr(S_INDEX, 0);
    csr = sie_rd(S_CSR0);
    if (csr & 0x04u) {                                  /* SentStall */
        sie_wr(S_CSR0, 0);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
#if FELUCCA_HID
        hid.e0_rx = 0;
#endif
#if FELUCCA_RAWHID
        raw.e0_rx = 0;
#endif
        return;
    }
    if (csr & 0x10u) {                                  /* SetupEnd: the host abandoned the transfer */
        sie_wr(S_CSR0, 0x80);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;                                  /* else the next SETUP is taken as line coding */
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
#if FELUCCA_HID
        hid.e0_rx = 0;
#endif
#if FELUCCA_RAWHID
        raw.e0_rx = 0;
#endif
    }
    if (usb.e0_tx) {
        if (!(csr & 0x02u))
            e0_chunk();
        return;
    }
    if (!(csr & 0x01u))
        return;
    fm1_usb_rx_sync();
#if FELUCCA_CDC
    if (cdc.e0_rx) {                                    /* SET_LINE_CODING data stage */
        uint32_t n = sie_rd(S_COUNT0);
        for (i = 0; i < 7u && i < n; i++)
            cdc.line[i] = ep0buf[i];
        cdc.e0_rx = 0;
        goto ack;
    }
#endif
#if FELUCCA_UAC
    if (uac.e0_rx) {                                    /* SET_CUR sampling frequency: 3 bytes, Hz */
        uac.rate_sets++;
        if (sie_rd(S_COUNT0) >= 3u)
            uac_rate_set(ep0buf[0] | (uint32_t)ep0buf[1] << 8 | (uint32_t)ep0buf[2] << 16);
        uac.e0_rx = 0;
        goto ack;
    }
#endif
#if FELUCCA_HID
    if (hid.e0_rx) {                                    /* SET_REPORT (output): the LED byte */
        if (sie_rd(S_COUNT0) >= 1u)
            hid.leds = ep0buf[0];
        hid.sets++;
        hid.e0_rx = 0;
        goto ack;
    }
#endif
#if FELUCCA_RAWHID
    if (raw.e0_rx) {                                    /* SET_REPORT to the raw interface: a host write */
        uint32_t n = sie_rd(S_COUNT0);
        if (!raw.in_full) {                             /* (else dropped: the host waits for each reply) */
            for (i = 0; i < RAW_N; i++)
                raw.in[i] = i < n ? ep0buf[i] : 0;
            RING_PUBLISH();
            raw.in_full = 1;
        }
        raw.e0_sets++;
        raw.e0_rx = 0;
        goto ack;
    }
#endif
    for (i = 0; i < 8u; i++)
        s[i] = ep0buf[i];
    usb.setups++;
    wvalue = (uint16_t)(s[2] | s[3] << 8);
    wlength = (uint16_t)(s[6] | s[7] << 8);
    switch ((uint32_t)s[0] << 8 | s[1]) {
    case 0x0005:                                        /* SET_ADDRESS */
        usb.pend_addr = s[2] & 0x7Fu;
        usb.has_pend_addr = 1;
        goto ack;
    case 0x8006: {                                      /* GET_DESCRIPTOR */
        const uint8_t *d;
        uint16_t l;
        if (get_desc(wvalue, &d, &l)) {
            if (!wlength)
                goto ack;
            e0_send(d, l, wlength);
            return;
        }
        goto stall;
    }
    case 0x0009:                                        /* SET_CONFIGURATION: 0 or 1 only */
        if (s[2] > 1u)
            goto stall;
        usb.config = s[2];
        if (usb.config == 1u)
            ep1_config();
        else
            sie_wr(S_INTRRX1E, 0);
#if FELUCCA_UAC
        if (usb.config != 1u)
            uac_stream(0);
        uac_rate_set(UAC_RATE);                         /* a new configuration starts at 44.1 */
#endif
        goto ack;
    case 0x8008:
        e0_send(&usb.config, 1, wlength);
        return;
    case 0x8000:
    case 0x8100:
    case 0x8200:
        e0_send(zero2, 2, wlength);
        return;
    case 0x010B:                                        /* SET_INTERFACE: alt 0 (alt 1 for the audio stream) */
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF && wvalue <= 1u && usb.config) {
            uac_ep4_reset();
            uac_stream(wvalue);
            goto ack;
        }
#endif
        if (wvalue == 0)
            goto ack;
        goto stall;
    case 0x810A:
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF) {
            static uint8_t alt;
            alt = uac.alt;
            e0_send(&alt, 1, wlength);
            return;
        }
#endif
        e0_send(zero2, 1, wlength);
        return;
    case 0x0201: {                                      /* CLEAR_FEATURE(ENDPOINT_HALT): data toggle reset */
        uint32_t ep = s[4] & 0x0Fu, last = USB_CDC_ON ? 3u : 1u;
#if FELUCCA_UAC
        if (wvalue == 0 && s[4] == 0x84u)
            goto ack;                                   /* isochronous: no halt, no toggle */
#endif
#if FELUCCA_HID
        if (wvalue == 0 && USB_HID_ON && s[4] == (0x80u | FELUCCA_HID_EP)) {
            sie_wr(S_INDEX, FELUCCA_HID_EP);
            sie_wr(S_TXCSR1, 0x40);                     /* ClrDataTog */
            goto ack;
        }
#endif
#if FELUCCA_MEDIA
        if (wvalue == 0 && USB_HID_ON && s[4] == (0x80u | MEDIA_EP)) {
            sie_wr(S_INDEX, MEDIA_EP);
            sie_wr(S_TXCSR1, 0x40);
            goto ack;
        }
#endif
#if FELUCCA_RAWHID
        if (wvalue == 0 && USB_HID_ON && (s[4] & 0x7Fu) == FELUCCA_RAWHID_EP) {
            sie_wr(S_INDEX, FELUCCA_RAWHID_EP);
            if (s[4] & 0x80u)
                sie_wr(S_TXCSR1, 0x40);
            else
                sie_wr(S_RXCSR1, 0x80);
            goto ack;
        }
#endif
        if (wvalue != 0 || ep > last)
            goto stall;                                 /* not an endpoint we have */
        if (ep) {                                       /* (EP0 has no halt to clear) */
            sie_wr(S_INDEX, ep);
            if (s[4] & 0x80u)
                sie_wr(S_TXCSR1, 0x40);
            else
                sie_wr(S_RXCSR1, 0x80);
        }
        goto ack;
    }
#if FELUCCA_HID
    /* HID (1.11 section 7): its class descriptors through a standard GET_DESCRIPTOR to the interface, and the
     * class requests; anything for another interface stalls, as before */
    case 0x8106:                                        /* GET_DESCRIPTOR, interface: HID 0x21, report 0x22 */
#if FELUCCA_MEDIA
        if (IS_MEDIA_IF(s[4])) {
            if (s[3] == 0x22u)
                e0_send(MEDIA_RD, sizeof MEDIA_RD, wlength);
            else if (s[3] == 0x21u)
                e0_send(MEDIA_DESC, sizeof MEDIA_DESC, wlength);
            else
                goto stall;
            return;
        }
#endif
#if FELUCCA_RAWHID
        if (IS_RAW_IF(s[4])) {
            if (s[3] == 0x22u)
                e0_send(RAW_RD, sizeof RAW_RD, wlength);
            else if (s[3] == 0x21u)
                e0_send(RAW_DESC, sizeof RAW_DESC, wlength);
            else
                goto stall;
            return;
        }
#endif
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        if (s[3] == 0x22u) {
            e0_send(HID_RD, sizeof HID_RD, wlength);
            return;
        }
        if (s[3] == 0x21u) {
            e0_send(HID_DESC, sizeof HID_DESC, wlength);
            return;
        }
        goto stall;
    case 0x2109:                                        /* SET_REPORT: output (the LEDs), 1 byte follows */
#if FELUCCA_RAWHID
        if (IS_RAW_IF(s[4]) && wlength) {               /* raw: a host write through EP0 */
            raw.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);
            return;
        }
#endif
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        if (wlength) {
            hid.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* ServicedRxPktRdy, no DataEnd yet */
            return;
        }
        goto ack;
    case 0x210A:                                        /* SET_IDLE: kept (we send on change only) */
#if FELUCCA_MEDIA
        if (IS_MEDIA_IF(s[4]))
            goto ack;
#endif
#if FELUCCA_RAWHID
        if (IS_RAW_IF(s[4]))
            goto ack;                                   /* raw: replies only, nothing to repeat */
#endif
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        hid.idle = s[3];
        goto ack;
    case 0x210B:                                        /* SET_PROTOCOL: boot and report are the same bytes */
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        hid.protocol = s[2] & 1u;
        goto ack;
    case 0xA101:                                        /* GET_REPORT: the keys now */
#if FELUCCA_MEDIA
        if (IS_MEDIA_IF(s[4])) {                        /* nothing pressed: the report ID, then zeros */
            static uint8_t idle_rep[6];
            uint32_t n = s[2] == 1u ? 6u : 3u;
            idle_rep[0] = s[2];
            e0_send(idle_rep, (uint16_t)n, wlength);
            return;
        }
#endif
#if FELUCCA_RAWHID
        if (IS_RAW_IF(s[4])) {                          /* raw: zeros (replies go on the IN endpoint) */
            static const uint8_t zero32[RAW_N];
            e0_send(zero32, RAW_N, wlength);
            return;
        }
#endif
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        e0_send(hid.cur, sizeof hid.cur, wlength);
        return;
    case 0xA102:                                        /* GET_IDLE */
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        e0_send(&hid.idle, 1, wlength);
        return;
    case 0xA103:                                        /* GET_PROTOCOL */
        if (!USB_HID_ON || s[4] != HID_IF)
            goto stall;
        e0_send(&hid.protocol, 1, wlength);
        return;
#endif
#if FELUCCA_CDC
    case 0x2120:                                        /* SET_LINE_CODING: 7 bytes follow */
        if (!usb_cdc_on)
            goto stall;
        if (wlength) {
            cdc.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* ServicedRxPktRdy, no DataEnd yet */
            return;
        }
        goto ack;
    case 0xA121:                                        /* GET_LINE_CODING */
        if (!usb_cdc_on)
            goto stall;
        e0_send(cdc.line, 7, wlength);
        return;
    case 0x2122:                                        /* SET_CONTROL_LINE_STATE */
        if (!usb_cdc_on)
            goto stall;
        cdc.dtr = s[2] & 1u;
        goto ack;
    case 0x2123:                                        /* SEND_BREAK */
        if (!usb_cdc_on)
            goto stall;
        goto ack;
#endif
#if FELUCCA_UAC
    case 0x2201:                                        /* SET_CUR, endpoint: sampling frequency */
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
        if (wlength) {
            uac.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* the 3 bytes follow */
            return;
        }
        goto ack;
    case 0xA281:                                        /* GET_CUR / MIN / MAX: the rate / the range */
    case 0xA282:
    case 0xA283: {
        static const uint8_t RATE[3] = {UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16};
#if FELUCCA_UAC_48K
        static const uint8_t RATE48[3] = {48000u & 0xFF, (48000u >> 8) & 0xFF, 48000u >> 16};
#endif
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
#if FELUCCA_UAC_48K
        if (s[1] == 0x83u || (s[1] == 0x81u && uac.r48)) {
            e0_send(RATE48, 3, wlength);
            return;
        }
#endif
        e0_send(RATE, 3, wlength);
        return;
    }
#endif
    default:
        goto stall;
    }
ack:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x48);
    return;
stall:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x60);
}

/* SysEx assembly: only short commands matter here */
static void sysex_byte(uint8_t b)
{
    static const uint8_t UBOOT_KEY[6] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};
    if (b >= 0xF8u)
        return;                                        /* realtime may occur anywhere in SysEx */
    if ((b & 0x80u) && b != 0xF0u && b != 0xF7u) {
        usb.sx_on = 0;
#if FELUCCA_OTA
        sx_collect = 0;
#endif
        return;                                        /* another status aborts the unfinished frame */
    }
    if (b == 0xF0) {
        usb.sx_on = 1;
        usb.sx_len = 0;
#if FELUCCA_OTA
        sx_collect = !sx_ready;
        sx_pos = 0;
#endif
    }
    if (!usb.sx_on)
        return;
#if FELUCCA_OTA
    if (sx_collect && b != 0xF0 && b != 0xF7) {
        if (sx_pos < sizeof sx_frame)
            sx_frame[sx_pos++] = b;
        else
            sx_collect = 0;                             /* too long: not ours */
    }
#endif
    if (usb.sx_len < sizeof usb.sysex)
        usb.sysex[usb.sx_len++] = b;
    if (b == 0xF7) {
        uint32_t i, ok = usb.sx_len == 6u;
        for (i = 0; ok && i < 6u; i++)
            ok = usb.sysex[i] == UBOOT_KEY[i];
        if (ok)
            usb.uboot_req = 1;
#if FELUCCA_OTA
        else if (usb.sx_len == 6u && usb.sysex[1] == 0x22 && usb.sysex[2] == 0x24 && usb.sysex[3] == 0x35 &&
                 usb.sysex[4] == 0x7F)
            usb.ota_req = 1;
        else if (sx_collect && sx_pos) {
            sx_frame_len = sx_pos;
            RING_PUBLISH();
            sx_ready = 1;                               /* main loop: ota_take() / ed_service() */
        }
        sx_collect = 0;
#endif
        usb.sx_on = 0;
    }
}

static void midi_in_event(uint32_t pkt)                 /* one USB-MIDI event packet */
{
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu;
    if (cin >= 4u && cin <= 7u) {
        uint32_t k, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        for (k = 0; k < nb; k++)
            sysex_byte((uint8_t)(pkt >> (8u * (k + 1u))));
        return;
    }
    if (st >= 0x80u && st < 0xF8u) {
        usb.sx_on = 0;                                 /* channel/system common also ends SysEx */
#if FELUCCA_OTA
        sx_collect = 0;
#endif
    }
    if (cin == 0xFu && (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)) {
        midi_enqueue(0xFu | st << 8, 1u);
        return;
    }
    if (cin >= 8u && cin <= 0xEu && (st >> 4) == cin && !((pkt >> 16) & 0x80u) &&
        ((cin == 0xCu || cin == 0xDu) || !(pkt & 0x80000000u))) {
        midi_enqueue(pkt, 1u);
    }
}

/* one EP1 OUT packet (<= 16 events) into the MIDI ring, or 0: fewer than 16 + 8 slots free, so the packet
 * stays (the host is NAKed) and is retried next poll; a burst never overflows the ring (that panics), and
 * 8 slots stay for TRS MIDI, which shares the ring and cannot wait (31250 baud: about 3 messages a half).
 * The update loader never drains the ring (it wants SysEx only): no back-pressure there. */
#define EP1_ROOM (16u + 8u)
static int ep1_take(const uint8_t *b, uint32_t n)
{
    uint32_t i;
#ifndef FELUCCA_LOADER
    if (MQ - (mi_w - mi_r) < EP1_ROOM)
        return 0;
#endif
    for (i = 0; i + 3u < n; i += 4u)
        midi_in_event((uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
    return 1;
}

static void ep1_rx(void)                                /* leaves the packet (NAK) while the ring is too full */
{
    uint32_t csr, n;
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        usb.rx_pend = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    fm1_usb_rx_sync();
    if (!ep1_take(ep1rx, n))
        return;                                         /* rx_pend stays: retried next poll */
    usb.rx_pend = 0;
    usb.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

#if FELUCCA_OTA
/* ---- SysEx frames for the main loop (ota.c / editor.c hooks; felucca.c and the
 * update loader supply ota_now_ms / ota_idle) ---- */
static uint32_t ota_now_ms(void);
static void ota_idle(void);

static int ota_wire_send(const uint8_t *p, uint32_t n)   /* F0..F7 -> USB-MIDI SysEx packets */
{
    uint32_t i = 0, t0 = ota_now_ms();
    sx_busy = 1;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, pkt;
        uint32_t cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;   /* 4 continues; 5/6/7 end */
        pkt = cin | (uint32_t)p[i] << 8 | (k > 1u ? (uint32_t)p[i + 1] << 16 : 0u) |
              (k > 2u ? (uint32_t)p[i + 2] << 24 : 0u);
        while (so_w - so_r >= SXQ) {
            if (!*(volatile uint8_t *)&usb.config || ota_now_ms() - t0 > 200u) {
                sx_busy = 0;
                return -1;
            }
            ota_idle();
        }
        sx_out_q[so_w % SXQ] = pkt;
        RING_PUBLISH();
        so_w++;
        i += k;
    }
    sx_busy = 0;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!sx_ready)
        return 0;
    RING_PUBLISH();                                     /* read the frame only after the flag */
    *p = sx_frame;
    *n = sx_frame_len;
    return 1;
}
static void ota_frame_done(void)
{
    RING_PUBLISH();                                     /* done with the frame before the ISR may refill it */
    sx_ready = 0;
}
#endif

static void ep1_tx(void)
{
    uint32_t csr, n = 0;
#if FELUCCA_OTA
    if (mo_w == mo_r && so_w == so_r)
        return;
#else
    if (mo_w == mo_r)
        return;
#endif
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;                                         /* previous packet still pending */
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
#if FELUCCA_OTA
    while (so_r != so_w && n < 64u) {                   /* SysEx first, never split by notes */
        uint32_t pkt = sx_out_q[so_r % SXQ];
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        so_r++;
    }
    while (!sx_busy && so_r == so_w && mo_r != mo_w && n < 64u) {
#else
    while (mo_r != mo_w && n < 64u) {
#endif
        uint32_t pkt;
        RING_PUBLISH();                                 /* the audio ISR (producer) can preempt us: */
        pkt = midi_out_q[mo_r % MQ];                    /* slot read strictly between the index checks */
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        RING_PUBLISH();
        mo_r++;
    }
    if (!n)
        return;
    fm1_usb_ep_send(1, ep1tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    usb.tx_pkts++;
}

#if FELUCCA_HID
/* ---- the keyboard: the main loop queues (hid_push), TIMER5 sends (hid_tx), one report a packet ---- */
static void hid_tx(void)                                /* as ep1_tx */
{
    uint32_t csr, i;
    if (hq_r == hq_w)
        return;
    sie_wr(S_INDEX, FELUCCA_HID_EP);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;                                         /* the last report not taken yet */
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    RING_PUBLISH();                                     /* the slot after the index */
    for (i = 0; i < 8u; i++)
        hidtx[i] = hid_q[hq_r % HQ][i];
    RING_PUBLISH();
    hq_r++;
    hid_ep_send(hidtx, 8u);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    hid.tx_pkts++;
}

/* the keyboard can be used: presented, configured, the bus awake */
static int hid_ready(void)
{
    return usb.up && usb_hid_on && usb.config && !usb.suspended;
}

/* reports the ring can still take */
static uint32_t hid_room(void)
{
    return HQ - (hq_w - hq_r);
}

/* main loop: queue one report (r[0] modifiers, r[1] 0, r[2..7] key codes); 0: the ring is full, try again */
static int hid_push(const uint8_t r[8])
{
    uint32_t i;
    if (hq_w - hq_r >= HQ)
        return 0;
    for (i = 0; i < 8u; i++) {
        hid_q[hq_w % HQ][i] = r[i];
        hid.cur[i] = r[i];
    }
    RING_PUBLISH();
    hq_w++;
    return 1;
}
#endif

#if FELUCCA_RAWHID
/* ---- raw HID: TIMER5 receives into raw.in (NAK while it is full) and sends raw.out; the main loop takes and
 * replies (raw_take, raw_send) ---- */
static void raw_rx(void)                                /* as ep3_rx: leaves the packet (NAK) while in[] is full */
{
    uint32_t csr, n, i;
    sie_wr(S_INDEX, FELUCCA_RAWHID_EP);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        raw.rx_pend = 0;
        return;
    }
    if (raw.in_full)
        return;                                         /* rx_pend stays: retried next poll */
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    raw.rx_pend = 0;
    fm1_usb_rx_sync();
    for (i = 0; i < RAW_N; i++)
        raw.in[i] = i < n ? rawrx[i] : 0;
    RING_PUBLISH();
    raw.in_full = 1;
    raw.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

static void raw_tx(void)                                /* as ep3_tx */
{
    uint32_t csr, i;
    if (!raw.out_full)
        return;
    sie_wr(S_INDEX, FELUCCA_RAWHID_EP);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    RING_PUBLISH();
    for (i = 0; i < RAW_N; i++)
        rawtx[i] = raw.out[i];
    RING_PUBLISH();
    raw.out_full = 0;
    fm1_usb_ep_send(FELUCCA_RAWHID_EP, rawtx, RAW_N);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    raw.tx_pkts++;
}

/* main loop: the host's next 32-byte report into r (1), or 0 */
static int raw_take(uint8_t r[RAW_N])
{
    uint32_t i;
    if (!raw.in_full)
        return 0;
    RING_PUBLISH();
    for (i = 0; i < RAW_N; i++)
        r[i] = raw.in[i];
    RING_PUBLISH();
    raw.in_full = 0;
    return 1;
}

#if FELUCCA_MEDIA
static void media_tx(void)                              /* as hid_tx */
{
    uint32_t csr, i, n;
    if (mdq_r == mdq_w)
        return;
    sie_wr(S_INDEX, MEDIA_EP);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    RING_PUBLISH();
    n = media_qn[mdq_r % MQN];
    for (i = 0; i < n; i++)
        mediatx[i] = media_q[mdq_r % MQN][i];
    RING_PUBLISH();
    mdq_r++;
    fm1_usb_ep_send(MEDIA_EP, mediatx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    media_tx_pkts++;
}

static uint32_t media_room(void) { return MQN - (mdq_w - mdq_r); }

/* main loop: queue a mouse / system / consumer report (ID first, <= 8 bytes); 0: full */
static int media_push(const uint8_t *r, uint32_t n)
{
    uint32_t i;
    if (mdq_w - mdq_r >= MQN || n > 8u)
        return 0;
    for (i = 0; i < n; i++)
        media_q[mdq_w % MQN][i] = r[i];
    media_qn[mdq_w % MQN] = (uint8_t)n;
    RING_PUBLISH();
    mdq_w++;
    return 1;
}
#endif

/* main loop: queue a 32-byte report to the host; 0: the last one has not gone yet, try again */
static int raw_send(const uint8_t r[RAW_N])
{
    uint32_t i;
    if (raw.out_full)
        return 0;
    for (i = 0; i < RAW_N; i++)
        raw.out[i] = r[i];
    RING_PUBLISH();
    raw.out_full = 1;
    return 1;
}
#endif

#if FELUCCA_CDC
static void ep3_rx(void)                                /* leaves the packet (NAK) while the ring is full */
{
    uint32_t csr, n, i;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        cdc.rx_pend = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    if (CI_N - (ci_w - ci_r) < n)
        return;                                         /* rx_pend stays: retried next poll */
    cdc.rx_pend = 0;
    fm1_usb_rx_sync();
    for (i = 0; i < n; i++)
        cdc_in[(ci_w + i) % CI_N] = ep3rx[i];
    RING_PUBLISH();
    ci_w += n;
    cdc.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

static void ep3_tx(void)                                /* <= 63 bytes per packet: never needs a ZLP */
{
    uint32_t csr, n = 0;
    if (co_w == co_r)
        return;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    while (co_r != co_w && n < 63u)
        ep3tx[n++] = cdc_out[co_r++ % CO_N];
    fm1_usb_ep_send(3, ep3tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    cdc.tx_pkts++;
}
#endif

#if FELUCCA_UAC
/* ---- the audio input: audio.c produces (render start, then each block), TIMER5 consumes ---- */
static __attribute__((noinline)) void uac_render_start(void)   /* audio ISR, before a half buffer renders */
{
    uint32_t fill;
    uac.feed = uac.alt && usb.config && uac.flowing;
    if (!uac.feed) {
        uac.go = 0;                                     /* not read (yet / any more): prime when it is */
        return;
    }
    if (!uac.go || uac.ring48 != uac.r48) {             /* (re)start: silence ahead, then this render */
        uint32_t r = ua_r, i, prime = UA_PRIME;
        uac.go = 0;
#if FELUCCA_UAC_48K
        uac.ring48 = uac.r48;
        if (uac.ring48) {                               /* the resampler starts from silence */
            prime = UA_PRIME48;
            for (i = 0; i < 2u * RS_H; i++)
                rs.l[i] = rs.r[i] = 0;
            rs.w = rs.ph = 0;
        }
#endif
        for (i = 0; i < prime; i++)
            ua_ring[(r + i) & (UA_N - 1u)] = 0;
        RING_PUBLISH();
        ua_w = r + prime;
        uac.fill_min = prime;
        RING_PUBLISH();
        uac.go = 1;
        return;
    }
    fill = ua_w - ua_r;
    uac.fill_min = fill;
    if (fill < uac.fill_lo)
        uac.fill_lo = fill;
    if (fill > uac.fill_hi)
        uac.fill_hi = fill;
}

static inline int32_t uac_s16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

#if FELUCCA_UAC_48K
/* 44.1 -> 48 kHz (x 160 / 147), one block: each input frame enters the history, then the 1 or 2 output
 * frames whose newest input it is: phase ph, the window of UAC_FIR_N frames ending at it, the taps of
 * phase ph (or of 159 - ph backwards: the prototype is symmetric), Q30 into 64-bit sums. */
static __attribute__((noinline)) void uac_tap48(const int32_t *out, uint32_t n)
{
    uint32_t w = ua_w, i, j, k = 0, h = rs.w, ph = rs.ph;
    if (UA_N - (w - ua_r) < n + n / 8u + 2u) {         /* nobody reads: drop the block (<= n * 160 / 147 + 1) */
        uac.overruns++;
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t s = h & (RS_H - 1u);
        int16_t l = (int16_t)uac_s16(out[2u * i]), r = (int16_t)uac_s16(out[2u * i + 1u]);
        rs.l[s] = rs.l[s + RS_H] = l;
        rs.r[s] = rs.r[s + RS_H] = r;
        h++;
        s = (h - UAC_FIR_N) & (RS_H - 1u);              /* the window: oldest .. this frame */
        do {
            const int16_t *x = &rs.l[s];                /* R: x[2 * RS_H] */
            int64_t al = 0, ar = 0;
            int32_t yl, yr;
            if (ph < UAC_FIR_L / 2u) {
                const int32_t *c = UAC_FIR[ph];
                for (j = UAC_FIR_N; j; j--, x++) {
                    int32_t t = *c++;
                    al += (int64_t)t * x[0];
                    ar += (int64_t)t * x[2u * RS_H];
                }
            } else {
                const int32_t *c = &UAC_FIR[UAC_FIR_L - 1u - ph][UAC_FIR_N - 1u];
                for (j = UAC_FIR_N; j; j--, x++) {
                    int32_t t = *c--;
                    al += (int64_t)t * x[0];
                    ar += (int64_t)t * x[2u * RS_H];
                }
            }
            yl = uac_s16((int32_t)((al + (1 << (UAC_FIR_Q - 1))) >> UAC_FIR_Q));
            yr = uac_s16((int32_t)((ar + (1 << (UAC_FIR_Q - 1))) >> UAC_FIR_Q));
#if FELUCCA_UAC_TONE
            {   /* bench: the triangles at 48 kHz (not resampled), one per output frame */
                uint32_t t = rs.tone, q = t < 50u ? t : 100u - t, q2 = t < 50u ? 50u - t : t - 50u;
                rs.tone = t == 99u ? 0u : t + 1u;
                yl = (int32_t)q * 1000 - 25000;
                yr = (int32_t)q2 * 1000 - 25000;
            }
#endif
            ua_ring[(w + k++) & (UA_N - 1u)] = (uint32_t)(uint16_t)yl | (uint32_t)(uint16_t)yr << 16;
            ph += UAC_FIR_M;
        } while (ph < UAC_FIR_L);
        ph -= UAC_FIR_L;
    }
    rs.w = h;
    rs.ph = ph;
    RING_PUBLISH();
    ua_w = w + k;
}
#endif

static __attribute__((noinline)) void uac_tap(const int32_t *out, uint32_t n)   /* audio ISR: one block (Q15) */
{
    uint32_t w = ua_w, i;
    if (!uac.feed)
        return;
#if FELUCCA_UAC_48K
    if (uac.ring48) {
        uac_tap48(out, n);
        return;
    }
#endif
    if (UA_N - (w - ua_r) < n) {                        /* nobody reads: drop the block */
        uac.overruns++;
        return;
    }
#if FELUCCA_UAC_TONE
    {   /* bench: triangles of period 100 frames instead of the music (L, R in antiphase): every
         * recorded frame must follow the one before, so a lost or repeated frame shows exactly */
        static uint32_t ph;
        for (i = 0; i < n; i++, ph = ph == 99u ? 0u : ph + 1u) {
            uint32_t q = ph < 50u ? ph : 100u - ph, q2 = ph < 50u ? 50u - ph : ph - 50u;
            ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)(int16_t)((int32_t)q * 1000 - 25000) |
                                             (uint32_t)(uint16_t)(int16_t)((int32_t)q2 * 1000 - 25000) << 16;
        }
        (void)out;
    }
#else
    for (i = 0; i < n; i++)
        ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)uac_s16(out[2u * i]) |
                                         (uint32_t)(uint16_t)uac_s16(out[2u * i + 1u]) << 16;
#endif
    RING_PUBLISH();
    ua_w = w + n;
}

/* the next packet into d: 44 / 45 frames (+-1 to hold the ring's fill), from the ring or, until it is
 * primed, silence. An empty ring repeats the last frame. Returns the frame count. */
static uint32_t uac_packet(uint32_t *d)
{
    uint32_t n = 44u, r, fill, k, i, lo = UA_LO, hi = UA_HI;
#if FELUCCA_UAC_48K
    if (uac.r48) {                                      /* 48 frames a ms, no pattern */
        n = 48u;
        lo = UA_LO48;
        hi = UA_HI48;
    } else
#endif
    {
        uac.acc += UAC_RATE % 1000u;
        if (uac.acc >= 1000u) {
            uac.acc -= 1000u;
            n++;
        }
    }
    if (!uac.go || uac.ring48 != uac.r48) {             /* not primed (at this rate) yet */
        for (i = 0; i < n; i++)
            d[i] = 0;
        return n;
    }
    if (uac.fill_min > hi) {
        n++;
        uac.adj_up++;
    } else if (uac.fill_min < lo) {
        n--;
        uac.adj_down++;
    }
    r = ua_r;
    fill = ua_w - r;
    RING_PUBLISH();                                     /* the slots after the index */
    k = fill < n ? fill : n;
    for (i = 0; i < k; i++)
        d[i] = ua_ring[(r + i) & (UA_N - 1u)];
    if (k < n) {
        uint32_t last = k ? d[k - 1u] : uac.last;
        uac.underruns++;
        for (; i < n; i++)
            d[i] = last;
    }
    uac.last = d[n - 1u];
    RING_PUBLISH();
    ua_r = r + k;
    uac.pkts++;
    uac.frames += n;
    return n;
}

/* TIMER5, 2 kHz, also nested in the render: the host takes one packet per 1 ms frame; queue the
 * next one as soon as the last has gone (TxPktRdy clear). Not paced on SOF-pending (see usb_poll).
 * The ring is fed only while the host reads: it often selects alt 1 long before its first IN token,
 * and a recorder may stop reading without going back to alt 0 (a packet waiting > 20 ms). */
static void uac_service(void)
{
    uint32_t csr, n;
    if (!usb.up || !usb.config || !uac.alt)
        return;
    sie_wr(S_INDEX, 4);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u) {                                  /* still queued */
        if (uac.wait < 255u && ++uac.wait == 40u && uac.flowing) {
            uac.flowing = 0;
            uac.stalls++;
        }
        return;
    }
    if (uac.queued) {
        if (csr & 0x04u)
            uac.missed++;                               /* UnderRun: an IN token found nothing queued */
        uac.flowing = 1;
    }
    uac.queued = 1;
    uac.wait = 0;
    n = uac_packet(ep4tx);
    fm1_usb_ep4_send(ep4tx, n * 4u);
    sie_wr(S_TXCSR1, 0x01);                             /* TxPktRdy (and UnderRun cleared) */
}
#endif

static void usb_poll(void)                              /* TIMER5 ISR, 2 kHz */
{
    uint32_t iu, it, ir;
    if (!usb.up)
        return;
    if (fm1_usb_sof_take()) {                           /* SOF pending: NOT a reliable "host is there" */
                                                        /* (it keeps firing with the cable out) */
        usb.sof_seen++;
        usb.no_sof = 0;
    } else if (++usb.no_sof > usb.max_gap) {
        usb.max_gap = usb.no_sof;
    }
    if (usb.config) {                                   /* the frame number only advances with a real host */
        static uint32_t tick;
        if (++tick >= 20u) {                            /* every 20 polls = 10 ms */
            uint16_t f;
            tick = 0;
            f = (uint16_t)(sie_rd(S_FRAME1) | (sie_rd(S_FRAME2) & 7u) << 8);
            if (f != usb.frame) {
                usb.frame = f;
                usb.frame_same = 0;
                usb.suspended = 0;
            } else if (++usb.frame_same >= 10u && !usb.suspended) {   /* 100 ms frozen: unplugged / host asleep */
                usb.suspended = 1;
                usb.frame_stalls++;
            }
        }
    }
    iu = sie_rd(S_INTRUSB);
#ifdef FELUCCA_LOADER
    it = sie_rd(S_INTRTX1) | sie_rd(S_INTRTX2) << 8;    /* the update loader reads both */
    ir = sie_rd(S_INTRRX1) | sie_rd(S_INTRRX2) << 8;
#else
    it = sie_rd(S_INTRTX1);                             /* EP0..7 only: skipping INTR*2 saves */
    ir = sie_rd(S_INTRRX1);                             /* 2 SIE round trips per poll */
#endif
    if (iu & 0x01u) {                                   /* suspend: the bus went idle */
        usb.suspends++;
        usb.suspended = 1;
    }
    if (iu & 0x02u)                                     /* resume */
        usb.suspended = 0;
    if (iu & 0x04u) {                                   /* bus reset */
        usb.resets++;
        sie_wr(S_FADDR, 0);
        usb.config = 0;
        usb.suspended = 0;
        mo_r = mo_w;                                    /* nothing stale for the next host */
        usb.sx_on = 0;
#if FELUCCA_OTA
        so_r = so_w;
        sx_collect = 0;
#endif
        usb.e0_tx = 0;
        usb.has_pend_addr = 0;
        usb.rx_pend = 0;
#if FELUCCA_CDC
        cdc.dtr = 0;
        cdc.e0_rx = 0;
        cdc.rx_pend = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
        uac_stream(0);
        uac_rate_set(UAC_RATE);
#endif
#if FELUCCA_HID
        hid.e0_rx = 0;
        hid.idle = 0;
        hid.protocol = 1;                               /* HID 1.11 7.2.6: report protocol after a reset */
        hq_r = hq_w;
#endif
#if FELUCCA_RAWHID
        raw.e0_rx = 0;
        raw.rx_pend = 0;
        raw.in_full = 0;
        raw.out_full = 0;
#endif
#if FELUCCA_MEDIA
        mdq_r = mdq_w;
#endif
        fm1_usb_ep0_buf(ep0buf);
        sie_wr(S_INTRUSBE, 0x07);
        sie_wr(S_INTRTX1E, 0x01);
        sie_wr(S_INTRRX1E, 0);
    }
    if (it & 0x01u)
        ep0_service();
    if (ir & 0x02u)
        usb.rx_pend = 1;
    if (usb.rx_pend)                                    /* not every poll: 3 SIE round trips each */
        ep1_rx();
    if (usb.config)
        ep1_tx();
#if FELUCCA_CDC
    if (usb.config && usb_cdc_on) {
        if (ir & 0x08u)
            cdc.rx_pend = 1;
        if (cdc.rx_pend)                                /* not every poll: 3 SIE round trips each */
            ep3_rx();
        if (cdc.dtr)
            ep3_tx();
    }
#endif
#if FELUCCA_UAC
    uac_service();
#endif
#if FELUCCA_HID
    if (usb.config && usb_hid_on)
        hid_tx();
#endif
#if FELUCCA_MEDIA
    if (usb.config && usb_hid_on)
        media_tx();
#endif
#if FELUCCA_RAWHID
    if (usb.config && usb_hid_on) {
        if (ir & (1u << FELUCCA_RAWHID_EP))
            raw.rx_pend = 1;
        if (raw.rx_pend)                                /* not every poll: 3 SIE round trips each */
            raw_rx();
        raw_tx();
    }
#endif
}

static void usb_start(void)                             /* boot, or main-loop retry while usb.up == 0 */
{
    usb.timeouts = 0;
    fm1_usb_reset();                                    /* reset whatever the ROM left */
    fm1_delay_ms(25);
    fm1_usb_attach(ep0buf);
    sie_wr(S_POWER, 0x60);
    sie_wr(S_INTRUSBE, 0x07);                           /* suspend, resume, reset (polled) */
    sie_wr(S_INTRTX1E, 0x01);
    sie_wr(S_INTRTX2E, 0);
    sie_wr(S_INTRRX1E, 0);
    sie_wr(S_INTRRX2E, 0);
    fm1_usb_ep_enable(0x1Fu);
    usb.up = usb.timeouts < 3u;                         /* a dead SIE leaves USB off */
}

/* Powered on without a cable, the SIE does not answer and usb_start leaves
 * USB off: retry once a second from the main loop so a cable plugged in
 * later still enumerates. */
static void usb_retry(uint32_t now_ms)
{
    static uint32_t t;
    if (usb.up || usb.detached || now_ms - t < 1000u)
        return;
    t = now_ms;
    usb.retries++;
    usb_start();
}

static void usb_detach(void)
{
    usb.detached = 1;
    usb.up = 0;
    fm1_usb_off();
}

#if FELUCCA_CDC
/* Present the device with or without the serial console (main loop; at boot before usb_start, or later
 * from a setting). A change while attached drops off the bus; usb_retry attaches again within a second
 * with the other descriptors, and the host enumerates the device afresh. */
static __attribute__((unused)) void usb_cdc_switch(uint32_t on)
{
    on = on != 0;
    if (on == usb_cdc_on)
        return;
    if (usb.up) {
        usb.up = 0;                                     /* usb_poll (TIMER5) stops before the SIE goes */
        RING_PUBLISH();
        fm1_usb_off();
    }
    usb.config = 0;
    cdc.dtr = 0;                                        /* the console stops writing */
    cdc.rx_pend = 0;
    usb_cdc_on = (uint8_t)on;
}
#endif

#if FELUCCA_HID
/* Present the device with or without the keyboard, as usb_cdc_switch (at boot before usb_start, or later: the
 * device drops off the bus and usb_retry attaches it again with the other descriptors). */
static __attribute__((unused)) void usb_hid_switch(uint32_t on)
{
    on = on != 0;
    if (on == usb_hid_on)
        return;
    if (usb.up) {
        usb.up = 0;                                     /* usb_poll (TIMER5) stops before the SIE goes */
        RING_PUBLISH();
        fm1_usb_off();
    }
    usb.config = 0;
    hq_r = hq_w;
#if FELUCCA_RAWHID
    raw.in_full = 0;
    raw.out_full = 0;
#endif
#if FELUCCA_MEDIA
    mdq_r = mdq_w;
#endif
    usb_hid_on = (uint8_t)on;
}
#endif
