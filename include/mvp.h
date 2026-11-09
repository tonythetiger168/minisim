/* mvp.h - the virtual prototype interface of minisim.
 *
 * A virtual prototype is the part of a system that is a program rather than
 * RTL: a memory, a peripheral, a model of a block that is not written yet, or
 * a whole subsystem behind a SystemC/TLM socket. The design drives it through
 * an AXI4-Lite port, and this header is the C side of that port.
 *
 * Write a model as a C or C++ file, give it to msim on the command line (or
 * build it into a library and pass -sv_lib), and instance mvp_axil_slave in
 * the design:
 *
 *     #include "mvp.h"
 *     static uint8_t ram[0x1000];
 *     static int rd(void* c, uint64_t a, uint32_t n, uint64_t* d) {
 *         (void)c; memcpy(d, ram + a, n); return 0;
 *     }
 *     static int wr(void* c, uint64_t a, uint32_t n, uint64_t d, uint8_t strb) {
 *         (void)c; for (uint32_t i = 0; i < n; i++)
 *             if (strb & (1u << i)) ram[a + i] = (uint8_t)(d >> (8 * i));
 *         return 0;
 *     }
 *     void mvp_model_init(void) {
 *         mvp_device dev = {0};
 *         dev.name = "ram"; dev.base = 0x0000; dev.size = sizeof ram;
 *         dev.read = rd; dev.write = wr;
 *         mvp_add_device(&dev);
 *     }
 *
 *     msim soc.sv soc_tb.sv ram.c -top soc_tb
 *
 * Addresses reaching a device are relative to its base. The runtime is in the
 * same library as the models, so it needs nothing from msim but DPI-C.
 */
#ifndef MVP_H
#define MVP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status of one access, returned by a model and turned into an AXI response:
 * MVP_OK -> OKAY, anything else -> SLVERR. */
enum {
    MVP_OK = 0,
    MVP_ERR = 1,       /* the device rejects this access */
    MVP_DECODE = 2     /* no device holds the address (the runtime's answer) */
};

/* One model in the memory map. `read` and `write` are called from the design's
 * clock process, so they must return without blocking; to take time, arm a
 * callback with mvp_after_ps and keep the register side ready. `bytes` is 1, 2,
 * 4 or 8 and the access does not cross the device's end. In `write`, bit i of
 * `strb` enables byte i of `data`. */
typedef struct mvp_device {
    const char* name;
    uint64_t base, size;
    void* ctx;
    int (*read)(void* ctx, uint64_t addr, uint32_t bytes, uint64_t* data);
    int (*write)(void* ctx, uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb);
    /* Optional: called once before time 0, after every device is registered. */
    void (*reset)(void* ctx);
} mvp_device;

/* Add a device to the map. Returns its index, or -1 if it overlaps one that is
 * already there, the map is full, or the device has no callbacks. Call it from
 * mvp_model_init, or from a constructor. */
int mvp_add_device(const mvp_device* dev);

/* Models define this; the runtime calls it once before time 0. It is weak, so
 * a library whose models register another way need not define it. One library
 * holds one mvp_model_init, so with several model files use MVP_MODEL. */
void mvp_model_init(void);

/* MVP_MODEL(name) { ... } defines a registration function that runs when the
 * library loads, so every model file in a library registers its own devices:
 *
 *     MVP_MODEL(uart_init) { ... mvp_add_device(&dev); }
 */
#if defined(__GNUC__) || defined(__clang__)
#define MVP_MODEL(name)                                                     \
    static void name(void);                                                 \
    __attribute__((constructor)) static void name##_mvp_ctor(void) { name(); } \
    static void name(void)
#else
#define MVP_MODEL(name) void mvp_model_init(void)
#endif

/* The design's time, in picoseconds, as of the last clock edge. */
uint64_t mvp_time_ps(void);

/* Call `fn` once, at the first clock edge at or after `delay_ps` from now.
 * The design's clock is the resolution: a delay shorter than a clock period
 * runs at the next edge. Returns 0, or -1 if there is no room. */
int mvp_after_ps(uint64_t delay_ps, void (*fn)(void* ctx), void* ctx);

/* Drive an interrupt line (0 to 31) of mvp_irq_slave. Level sensitive: it
 * stays until the model clears it. */
void mvp_set_irq(int line, int level);

/* Read one interrupt line back. */
int mvp_get_irq(int line);

/* Print the memory map on stdout, in the order the devices were added. */
void mvp_print_map(void);

/* An access from a model into the map, for a model that is a bus master (a
 * DMA engine, or a CPU model fetching instructions). It does not pass through
 * the design: it reaches the other models only. */
int mvp_bus_read(uint64_t addr, uint32_t bytes, uint64_t* data);
int mvp_bus_write(uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb);

#ifdef __cplusplus
}
#endif

#endif /* MVP_H */
