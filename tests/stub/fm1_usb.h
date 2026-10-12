/* Host stub of felucca-bsp's chip/fm1_usb.h: a fake SIE for tests/desc_dump.c. usb.c's own code runs unchanged;
 * the registers it reads come from stub_*, and what it sends on EP0 / writes to CSR0 is recorded. */
#pragma once
#include <stdint.h>
#include <string.h>

static uint32_t stub_index;                 /* S_INDEX */
static uint32_t stub_csr0 = 0;              /* what S_CSR0 reads (index 0): bit 0 RxPktRdy, 1 TxPktRdy, ... */
static uint32_t stub_count0 = 0;            /* S_COUNT0 */
static uint32_t stub_csr0_written;          /* the last value written to S_CSR0 */
static uint32_t stub_data;                  /* the pending read */
static uint8_t stub_in[4096];               /* EP0 IN data sent, appended */
static uint32_t stub_in_n;
static uint32_t stub_ep_tx[8];              /* bytes queued per endpoint (ep_send / ep4_send) */

static inline void fm1_usb_reset(void) {}
static inline void fm1_usb_attach(void *ep0) { (void)ep0; }
static inline void fm1_usb_off(void) {}
static inline uint32_t fm1_usb_sie_on(void) { return 1; }
static inline void fm1_usb_sie_wr_start(uint32_t r, uint32_t v)
{
    if (r == 14)
        stub_index = v;
    if (r == 17 && stub_index == 0)
        stub_csr0_written = v;
}
static uint32_t stub_rx3_csr, stub_rx3_count;   /* EP3 OUT: RXCSR1 bit 0 RxPktRdy, RXCOUNT1 */
static uint8_t stub_ep3_last[64];                /* the last EP3 IN packet */
static inline void fm1_usb_sie_rd_start(uint32_t r)
{
    stub_data = 0;
    if (r == 17 && stub_index == 0)
        stub_data = stub_csr0;
    if (r == 22 && stub_index == 0)
        stub_data = stub_count0;
    if (r == 20 && stub_index == 3)
        stub_data = stub_rx3_csr;
    if (r == 22 && stub_index == 3)
        stub_data = stub_rx3_count;
}
static inline uint32_t fm1_usb_sie_done(void) { return 1; }
static inline uint32_t fm1_usb_sie_data(void) { return stub_data & 0xFF; }
static inline void fm1_usb_ep0_buf(void *p) { (void)p; }
static inline void fm1_usb_ep_txbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
static inline void fm1_usb_ep_rxbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
static inline void fm1_usb_ep0_send(void *p, uint32_t n)
{
    memcpy(stub_in + stub_in_n, p, n);
    stub_in_n += n;
}
static inline void fm1_usb_ep_send(uint32_t ep, void *p, uint32_t n)
{
    if (ep == 3)
        memcpy(stub_ep3_last, p, n);
    stub_ep_tx[ep] += n;
}
static inline void fm1_usb_ep4_txbuf(void *p) { (void)p; }
static uint8_t stub_ep4_last[256];          /* the last EP4 packet */
static inline void fm1_usb_ep4_send(void *p, uint32_t n) { memcpy(stub_ep4_last, p, n); stub_ep_tx[4] += n; }
static inline void fm1_usb_rx_sync(void) {}
static inline void fm1_usb_ep_enable(uint32_t eps) { (void)eps; }
static inline uint32_t fm1_usb_sof_take(void) { return 0; }
static inline void fm1_delay_ms(uint32_t ms) { (void)ms; }
