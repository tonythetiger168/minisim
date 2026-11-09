/* uart.c - a 16550-style UART for a virtual prototype: the design writes a
 * byte, the model prints it, and the transmit-empty interrupt comes back a
 * character time later, so the software's wait loop is exercised rather than
 * skipped.
 *
 *   MVP_UART_BASE=0x1000    the base address (default 0x10000)
 *   MVP_UART_BAUD=115200    bits per second, for the character time (default
 *                           115200; 0 makes transmission instant)
 *   MVP_UART_IRQ=0          the mvp_irq_slave line to drive (default 0)
 *
 * Registers (byte offsets, 32-bit accesses):
 *   0x00  THR   write: the byte to send    read: the last byte received
 *   0x04  LSR   read-only: bit 0 THR empty, bit 1 TX busy, bit 2 RX ready
 *   0x08  IER   bit 0: interrupt when THR goes empty
 *   0x0c  IIR   read-only: bit 0 set while the interrupt is asserted; a write
 *               of any value clears a pending one
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvp.h"

enum { THR = 0x00, LSR = 0x04, IER = 0x08, IIR = 0x0c, UART_SIZE = 0x10 };
enum { LSR_THRE = 1u << 0, LSR_BUSY = 1u << 1, LSR_RXRDY = 1u << 2 };

static struct {
    int irq_line;
    uint64_t char_ps;   /* how long one character takes */
    uint32_t ier, lsr;
    int pending;        /* the interrupt is asserted */
    uint8_t rx;
} u = {.lsr = LSR_THRE};  /* empty and idle before the first write */

/* The character has gone out: THR is empty again. */
static void tx_done(void* ctx) {
    (void)ctx;
    u.lsr = (u.lsr & ~LSR_BUSY) | LSR_THRE;
    if (u.ier & 1u) {
        u.pending = 1;
        mvp_set_irq(u.irq_line, 1);
    }
}

static int uart_read(void* ctx, uint64_t addr, uint32_t bytes, uint64_t* data) {
    (void)ctx;
    (void)bytes;
    switch (addr & ~3ull) {
        case THR: *data = u.rx; u.lsr &= ~LSR_RXRDY; return MVP_OK;
        case LSR: *data = u.lsr; return MVP_OK;
        case IER: *data = u.ier; return MVP_OK;
        case IIR: *data = u.pending ? 1u : 0u; return MVP_OK;
        default: *data = 0; return MVP_ERR;
    }
}

static int uart_write(void* ctx, uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb) {
    (void)ctx;
    (void)bytes;
    (void)strb;
    switch (addr & ~3ull) {
        case THR: {
            if (!(u.lsr & LSR_THRE)) {
                fprintf(stderr, "mvp uart: write to a full THR at %llu ps, byte dropped\n",
                        (unsigned long long)mvp_time_ps());
                return MVP_ERR;
            }
            int c = (int)(data & 0xff);
            putchar(c);
            if (c == '\n') fflush(stdout);
            if (u.char_ps) {
                u.lsr = (u.lsr & ~LSR_THRE) | LSR_BUSY;
                mvp_after_ps(u.char_ps, tx_done, NULL);
            } else if (u.ier & 1u) {
                u.pending = 1;
                mvp_set_irq(u.irq_line, 1);
            }
            return MVP_OK;
        }
        case IER:
            u.ier = (uint32_t)data;
            if (!(u.ier & 1u)) {
                u.pending = 0;
                mvp_set_irq(u.irq_line, 0);
            }
            return MVP_OK;
        case IIR:  /* any write clears the interrupt */
            u.pending = 0;
            mvp_set_irq(u.irq_line, 0);
            return MVP_OK;
        case LSR: return MVP_ERR;  /* read-only */
        default: return MVP_ERR;
    }
}

/* Feed a byte in, as if it arrived on the wire. A testbench reaches it through
 * its own DPI import, and another model can call it directly. */
void mvp_uart_rx(int byte) {
    u.rx = (uint8_t)byte;
    u.lsr |= LSR_RXRDY;
}

static uint64_t env_num(const char* name, uint64_t def) {
    const char* s = getenv(name);
    return (s && *s) ? (uint64_t)strtoull(s, NULL, 0) : def;
}

MVP_MODEL(uart_init) {
    uint64_t baud = env_num("MVP_UART_BAUD", 115200);
    /* 10 bit times per character: start, 8 data, stop. */
    u.char_ps = baud ? (uint64_t)(10.0 * 1e12 / (double)baud) : 0;
    u.irq_line = (int)env_num("MVP_UART_IRQ", 0);

    mvp_device dev;
    memset(&dev, 0, sizeof dev);
    dev.name = "uart";
    dev.base = env_num("MVP_UART_BASE", 0x10000);
    dev.size = UART_SIZE;
    dev.read = uart_read;
    dev.write = uart_write;
    if (mvp_add_device(&dev) < 0) exit(1);
}
