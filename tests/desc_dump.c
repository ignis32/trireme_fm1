/* Host test of firmware/usb.c's EP0 handling and the keyboard's report path, against a fake SIE (tests/stub).
 * Built three times (Makefile): USB_C = firmware/usb.c in the console flavour (CDC + HID on EP4; OTA off here),
 * the same with TEST_RAW (the raw flavour: no CDC, HID on EP4 + raw HID on EP3), and USB_C = felucca-bsp's
 * pristine chip/usb.c (no HID), so check_desc.py can compare the keyboard-off descriptors with upstream's.
 * Prints, per variant, the device and configuration descriptors as the host gets them through GET_DESCRIPTOR;
 * for the HID build also the HID class requests' outcomes. Exit status 1 on a failed assertion. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TEST_RAW
#define FELUCCA_CDC 0
#define FELUCCA_RAWHID 1
#define FELUCCA_MEDIA 1
#define FELUCCA_USB_SERIAL 28, 3, 'v', 0, 'i', 0, 'a', 0, 'l', 0, ':', 0, 'f', 0, '6', 0, '4', 0, 'c', 0, '2', 0, \
                           'b', 0, '3', 0, 'c', 0       /* as firmware/main.c */
#else
#define FELUCCA_CDC 1
#endif
#define FELUCCA_OTA 0
#ifndef BSP_PRISTINE
#define FELUCCA_HID 1
#define FELUCCA_HID_EP 4
#endif
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include USB_C

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { ACK = 0x48, STALL = 0x60 };

/* one control transfer: SETUP, then the IN data stage (the host takes each packet) or one OUT data packet.
 * Returns what the device wrote last to CSR0 (ACK, STALL, or 0x0A / 0x02 for an IN data stage) */
static uint32_t control(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, const uint8_t *out)
{
    uint8_t s[8] = {rt, req, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
                    (uint8_t)len, (uint8_t)(len >> 8)};
    uint32_t guard = 0, last;
    stub_in_n = 0;
    memcpy(ep0buf, s, 8);
    stub_csr0 = 0x01;                               /* RxPktRdy: the SETUP */
    ep0_service();
    last = stub_csr0_written;
    while (usb.e0_tx && guard++ < 100) {            /* IN data stage: TxPktRdy clear = the host took it */
        stub_csr0 = 0;
        ep0_service();
        last = stub_csr0_written;
    }
    if (out && len && last == 0x40) {               /* OUT data stage */
        memcpy(ep0buf, out, len);
        stub_count0 = len;
        stub_csr0 = 0x01;
        ep0_service();
        last = stub_csr0_written;
    }
    stub_csr0 = 0;
    return last;
}

static void dump(const char *name)
{
    uint32_t i;
    printf("%s", name);
    for (i = 0; i < stub_in_n; i++)
        printf(" %02x", stub_in[i]);
    printf("\n");
}

static void variant(uint32_t cdc, uint32_t hid_on)
{
#if FELUCCA_CDC
    usb_cdc_on = (uint8_t)cdc;
#else
    if (cdc)
        return;                                     /* no console in this build */
#endif
#ifndef BSP_PRISTINE
    usb_hid_on = (uint8_t)hid_on;
#else
    if (hid_on)
        return;
#endif
    printf("variant cdc=%u hid=%u\n", cdc, hid_on);
    control(0x80, 6, 0x0100, 0, 64, 0);
    dump("device");
    control(0x80, 6, 0x0200, 0, 9, 0);              /* first the header, as hosts do */
    CHECK(stub_in_n == 9, "config header: %u bytes", stub_in_n);
    control(0x80, 6, 0x0200, 0, 0xFFFF, 0);
    dump("config");
    control(0x80, 6, 0x0302, 0x0409, 255, 0);
    dump("product");
    control(0x80, 6, 0x0303, 0x0409, 255, 0);
    dump("serial");
}

int main(void)
{
    variant(0, 0);
    variant(1, 0);
    variant(0, 1);
    variant(1, 1);
#ifndef BSP_PRISTINE
    {
        uint32_t r;
        uint8_t hif;
        static const uint8_t leds[1] = {0x02};
        static const uint8_t keys_a[8] = {0, 0, 0x04, 0, 0, 0, 0, 0}, none[8] = {0};
        uint32_t i;
#if FELUCCA_CDC
        usb_cdc_on = 1;
#endif
        usb_hid_on = 1;
        hif = (uint8_t)HID_IF;
        printf("hid_if %u\n", hif);
        r = control(0x00, 9, 1, 0, 0, 0);           /* SET_CONFIGURATION 1 */
        CHECK(r == ACK && usb.config == 1, "SET_CONFIGURATION: %02x", r);
        r = control(0x81, 6, 0x2200, hif, 0x41 + 64, 0);
        CHECK(stub_in_n == sizeof HID_RD, "report descriptor: %u bytes", stub_in_n);
        dump("report");
        r = control(0x81, 6, 0x2100, hif, 9, 0);
        CHECK(stub_in_n == 9 && stub_in[1] == 0x21, "HID descriptor");
        r = control(0x81, 6, 0x2200, hif + 3, 64, 0);   /* no interface there in either flavour */
        CHECK(r == STALL, "report descriptor of another interface: %02x", r);
        r = control(0x81, 6, 0x2200, 0, 64, 0);
        CHECK(r == STALL, "report descriptor of interface 0: %02x", r);
        r = control(0x21, 0x0A, 0x7D00, hif, 0, 0);    /* SET_IDLE 500 ms */
        CHECK(r == ACK && hid.idle == 0x7D, "SET_IDLE: %02x idle %u", r, hid.idle);
        r = control(0xA1, 0x02, 0, hif, 1, 0);
        CHECK(stub_in_n == 1 && stub_in[0] == 0x7D, "GET_IDLE");
        r = control(0x21, 0x0B, 0, hif, 0, 0);          /* SET_PROTOCOL boot */
        CHECK(r == ACK && hid.protocol == 0, "SET_PROTOCOL: %02x", r);
        r = control(0xA1, 0x03, 0, hif, 1, 0);
        CHECK(stub_in_n == 1 && stub_in[0] == 0, "GET_PROTOCOL");
        r = control(0x21, 0x09, 0x0200, hif, 1, leds);  /* SET_REPORT output: Caps Lock */
        CHECK(r == ACK && hid.leds == 0x02, "SET_REPORT: %02x leds %02x", r, hid.leds);
        r = control(0x21, 0x0A, 0, hif + 3, 0, 0);
        CHECK(r == STALL, "SET_IDLE to another interface: %02x", r);
        r = control(0x02, 1, 0, 0x84, 0, 0);            /* CLEAR_FEATURE(ENDPOINT_HALT) EP4 IN */
        CHECK(r == ACK, "CLEAR_FEATURE EP 0x84: %02x", r);
        r = control(0x02, 1, 0, 0x85, 0, 0);
        CHECK(r == STALL, "CLEAR_FEATURE EP 0x85: %02x", r);
        /* reports: queued by the main loop, sent by usb_poll's hid_tx */
        CHECK(hid_push(keys_a) && hid_push(none), "hid_push");
        r = control(0xA1, 0x01, 0x0100, hif, 8, 0);     /* GET_REPORT: the newest state */
        CHECK(stub_in_n == 8 && !memcmp(stub_in, none, 8), "GET_REPORT");
        stub_ep_tx[4] = 0;
        hid_tx();
        CHECK(stub_ep_tx[4] == 8 && !memcmp(stub_ep4_last, keys_a, 8), "first report on EP4");
        hid_tx();
        CHECK(stub_ep_tx[4] == 16 && !memcmp(stub_ep4_last, none, 8), "second report on EP4");
        hid_tx();
        CHECK(stub_ep_tx[4] == 16, "nothing more queued");
        for (i = 0; i < HQ; i++)
            hid_push(none);
        CHECK(!hid_push(none), "a full ring refuses");
#if FELUCCA_RAWHID
        {   /* raw HID: descriptors, a write by interrupt OUT, a write by SET_REPORT, the reply on EP3 IN */
            uint8_t rif = (uint8_t)RAW_IF, req[32], got[32];
            static const uint8_t ver[32] = {0x01};
            printf("raw_if %u\n", rif);
            r = control(0x81, 6, 0x2200, rif, 0x100, 0);
            CHECK(stub_in_n == sizeof RAW_RD && !memcmp(stub_in, RAW_RD, sizeof RAW_RD), "raw report descriptor");
            dump("raw_report");
            r = control(0x81, 6, 0x2100, rif, 9, 0);
            CHECK(stub_in_n == 9 && stub_in[1] == 0x21 && stub_in[7] == sizeof RAW_RD, "raw HID descriptor");
            r = control(0x21, 0x0A, 0, rif, 0, 0);
            CHECK(r == ACK, "SET_IDLE raw: %02x", r);
            r = control(0x21, 0x0B, 0, rif, 0, 0);
            CHECK(r == STALL, "SET_PROTOCOL raw (not a boot device): %02x", r);
            r = control(0xA1, 0x01, 0x0100, rif, 32, 0);
            CHECK(stub_in_n == 32, "GET_REPORT raw: %u bytes", stub_in_n);
            /* interrupt OUT: a packet in the DMA buffer, RxPktRdy */
            for (i = 0; i < 32u; i++)
                rawrx[i] = (uint8_t)(0xA0 + i);
            stub_rx3_csr = 0x01;
            stub_rx3_count = 32;
            raw.rx_pend = 1;
            raw_rx();
            stub_rx3_csr = 0;
            CHECK(raw.in_full && raw.rx_pkts == 1, "raw OUT taken");
            CHECK(raw_take(req) && req[0] == 0xA0 && req[31] == 0xBF && !raw.in_full, "raw_take");
            CHECK(raw_send(req) && !raw_send(req), "raw_send holds one");
            stub_ep_tx[3] = 0;
            raw_tx();
            CHECK(stub_ep_tx[3] == 32 && !memcmp(stub_ep3_last, req, 32) && !raw.out_full, "raw reply on EP3 IN");
            /* SET_REPORT through EP0 (hosts without an OUT pipe) */
            r = control(0x21, 0x09, 0x0200, rif, 32, ver);
            CHECK(r == ACK && raw.e0_sets == 1, "SET_REPORT raw: %02x", r);
            CHECK(raw_take(got) && got[0] == 0x01 && got[1] == 0, "raw_take after SET_REPORT");
            /* a second OUT packet while in[] is full stays (NAK) until taken */
            raw.in_full = 1;
            stub_rx3_csr = 0x01;
            raw.rx_pend = 1;
            raw_rx();
            CHECK(raw.rx_pend && raw.rx_pkts == 1, "raw OUT waits while full");
            raw.in_full = 0;
            raw_rx();
            CHECK(!raw.rx_pend && raw.rx_pkts == 2, "raw OUT taken once free");
            stub_rx3_csr = 0;
            r = control(0x02, 1, 0, 0x83, 0, 0);
            CHECK(r == ACK, "CLEAR_FEATURE EP 0x83: %02x", r);
            /* the media interface: descriptors, its reports on EP2 */
            {
                uint8_t mif = (uint8_t)MEDIA_IF;
                static const uint8_t vol[3] = {3, 0xE9, 0};
                printf("media_if %u\n", mif);
                r = control(0x81, 6, 0x2200, mif, 0x100, 0);
                CHECK(stub_in_n == sizeof MEDIA_RD, "media report descriptor: %u", stub_in_n);
                dump("media_report");
                r = control(0x21, 0x0A, 0, mif, 0, 0);
                CHECK(r == ACK, "SET_IDLE media: %02x", r);
                r = control(0x02, 1, 0, 0x82, 0, 0);
                CHECK(r == ACK, "CLEAR_FEATURE EP 0x82: %02x", r);
                CHECK(media_push(vol, 3) && media_room() == MQN - 1u, "media_push");
                stub_ep_tx[2] = 0;
                media_tx();
                CHECK(stub_ep_tx[2] == 3, "media report on EP2: %u bytes", stub_ep_tx[2]);
            }
            r = control(0x02, 1, 0, 0x03, 0, 0);
            CHECK(r == ACK, "CLEAR_FEATURE EP 0x03: %02x", r);
        }
#endif
        /* the keyboard off: HID requests stall as upstream's do */
        usb_hid_on = 0;
        r = control(0x81, 6, 0x2200, hif, 64, 0);
        CHECK(r == STALL, "report descriptor with the keyboard off: %02x", r);
        r = control(0x21, 0x0A, 0, hif, 0, 0);
        CHECK(r == STALL, "SET_IDLE with the keyboard off: %02x", r);
    }
#endif
    printf("fails %d\n", fails);
    return fails != 0;
}
