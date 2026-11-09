/* mvp.c - the virtual prototype runtime: the memory map, the timed callbacks,
 * the interrupt lines, and the DPI-C functions that mvp_axil_slave calls.
 *
 * It is built into the same library as the models (msim builds every C file on
 * its command line into one library), so a model reaches it by including
 * mvp.h and nothing else.
 */
#include "mvp.h"

#include <stdio.h>
#include <string.h>

#include "svdpi.h"

#define MVP_MAX_DEVICES 32
#define MVP_MAX_TIMERS 64

static mvp_device g_dev[MVP_MAX_DEVICES];
static int g_ndev;

static struct {
    uint64_t when_ps;
    void (*fn)(void*);
    void* ctx;
    int used;
} g_timer[MVP_MAX_TIMERS];

static uint64_t g_now_ps;
static uint32_t g_irq;
static int g_inited;

/* Weak, so a library that registers from constructors need not define it. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void mvp_model_init(void) {}
#else
void mvp_model_init(void) {}
#endif

int mvp_add_device(const mvp_device* dev) {
    if (!dev || (!dev->read && !dev->write) || !dev->size) {
        fprintf(stderr, "mvp: a device needs a size and at least one of read/write\n");
        return -1;
    }
    if (g_ndev == MVP_MAX_DEVICES) {
        fprintf(stderr, "mvp: more than %d devices\n", MVP_MAX_DEVICES);
        return -1;
    }
    for (int i = 0; i < g_ndev; i++) {
        /* Two devices may not answer one address. */
        if (dev->base < g_dev[i].base + g_dev[i].size && g_dev[i].base < dev->base + dev->size) {
            fprintf(stderr, "mvp: '%s' at 0x%llx..0x%llx overlaps '%s' at 0x%llx..0x%llx\n",
                    dev->name ? dev->name : "?", (unsigned long long)dev->base,
                    (unsigned long long)(dev->base + dev->size - 1),
                    g_dev[i].name ? g_dev[i].name : "?", (unsigned long long)g_dev[i].base,
                    (unsigned long long)(g_dev[i].base + g_dev[i].size - 1));
            return -1;
        }
    }
    g_dev[g_ndev] = *dev;
    return g_ndev++;
}

void mvp_print_map(void) {
    printf("mvp: %d device%s\n", g_ndev, g_ndev == 1 ? "" : "s");
    for (int i = 0; i < g_ndev; i++)
        printf("  0x%08llx..0x%08llx  %s\n", (unsigned long long)g_dev[i].base,
               (unsigned long long)(g_dev[i].base + g_dev[i].size - 1),
               g_dev[i].name ? g_dev[i].name : "?");
}

uint64_t mvp_time_ps(void) { return g_now_ps; }

int mvp_after_ps(uint64_t delay_ps, void (*fn)(void* ctx), void* ctx) {
    if (!fn) return -1;
    for (int i = 0; i < MVP_MAX_TIMERS; i++) {
        if (g_timer[i].used) continue;
        g_timer[i].used = 1;
        g_timer[i].when_ps = g_now_ps + delay_ps;
        g_timer[i].fn = fn;
        g_timer[i].ctx = ctx;
        return 0;
    }
    fprintf(stderr, "mvp: more than %d pending callbacks\n", MVP_MAX_TIMERS);
    return -1;
}

void mvp_set_irq(int line, int level) {
    if (line < 0 || line > 31) return;
    if (level) g_irq |= 1u << line;
    else g_irq &= ~(1u << line);
}

int mvp_get_irq(int line) {
    if (line < 0 || line > 31) return 0;
    return (g_irq >> line) & 1u;
}

/* Find the device an address belongs to, and make the address device-relative. */
static mvp_device* decode(uint64_t addr, uint32_t bytes, uint64_t* off) {
    for (int i = 0; i < g_ndev; i++) {
        if (addr < g_dev[i].base || addr >= g_dev[i].base + g_dev[i].size) continue;
        if (addr + bytes > g_dev[i].base + g_dev[i].size) return NULL;  // crosses the end
        *off = addr - g_dev[i].base;
        return &g_dev[i];
    }
    return NULL;
}

int mvp_bus_read(uint64_t addr, uint32_t bytes, uint64_t* data) {
    uint64_t off;
    mvp_device* d = decode(addr, bytes, &off);
    *data = 0;
    if (!d || !d->read) return MVP_DECODE;
    return d->read(d->ctx, off, bytes, data);
}

int mvp_bus_write(uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb) {
    uint64_t off;
    mvp_device* d = decode(addr, bytes, &off);
    if (!d || !d->write) return MVP_DECODE;
    return d->write(d->ctx, off, bytes, data, strb);
}

/* ---------------------------------------------------------------- DPI-C side
 *
 * mvp_axil_slave and mvp_irq_slave call these. They are the only entry points
 * the design needs; everything else is between the runtime and the models.
 */

/* Registers the models and resets them. Called once, at time 0. */
int mvp_dpi_init(int verbose) {
    if (g_inited) return g_ndev;
    g_inited = 1;
    mvp_model_init();
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].reset) g_dev[i].reset(g_dev[i].ctx);
    if (verbose) mvp_print_map();
    if (!g_ndev) fprintf(stderr, "mvp: no device registered; every access will be SLVERR\n");
    return g_ndev;
}

int mvp_dpi_read(long long addr, int bytes, long long* data) {
    uint64_t v = 0;
    int rc = mvp_bus_read((uint64_t)addr, (uint32_t)bytes, &v);
    *data = (long long)v;
    return rc;
}

int mvp_dpi_write(long long addr, int bytes, long long data, int strb) {
    return mvp_bus_write((uint64_t)addr, (uint32_t)bytes, (uint64_t)data, (uint8_t)strb);
}

/* The design's clock edge: set the time, then run the callbacks that are due.
 * A callback may arm another one; that one waits for a later edge, so one edge
 * cannot loop forever. */
void mvp_dpi_tick(long long now_ps) {
    g_now_ps = (uint64_t)now_ps;
    for (int i = 0; i < MVP_MAX_TIMERS; i++) {
        if (!g_timer[i].used || g_timer[i].when_ps > g_now_ps) continue;
        void (*fn)(void*) = g_timer[i].fn;
        void* ctx = g_timer[i].ctx;
        g_timer[i].used = 0;  /* freed first, so the callback may re-arm this slot */
        fn(ctx);
    }
}

int mvp_dpi_irq(void) { return (int)g_irq; }
