/* ram.c - a memory for a virtual prototype. The size and the base come from
 * the environment, so the same file serves any map:
 *
 *   MVP_RAM_BASE=0x0        the base address (default 0)
 *   MVP_RAM_SIZE=0x10000    the size in bytes (default 64 KiB)
 *   MVP_RAM_INIT=prog.bin   a file loaded at offset 0 (optional)
 *
 * It is the model a design uses before the real memory controller exists: the
 * access takes no time, which is what you want while the software is the part
 * under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvp.h"

static uint8_t* g_mem;
static uint64_t g_size;

static uint64_t env_num(const char* name, uint64_t def) {
    const char* s = getenv(name);
    if (!s || !*s) return def;
    return (uint64_t)strtoull(s, NULL, 0);  /* 0x... is hex */
}

static int ram_read(void* ctx, uint64_t addr, uint32_t bytes, uint64_t* data) {
    (void)ctx;
    uint64_t v = 0;
    for (uint32_t i = 0; i < bytes; i++) v |= (uint64_t)g_mem[addr + i] << (8 * i);
    *data = v;
    return MVP_OK;
}

static int ram_write(void* ctx, uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb) {
    (void)ctx;
    for (uint32_t i = 0; i < bytes; i++)
        if (strb & (1u << i)) g_mem[addr + i] = (uint8_t)(data >> (8 * i));
    return MVP_OK;
}

MVP_MODEL(ram_init) {
    uint64_t base = env_num("MVP_RAM_BASE", 0);
    g_size = env_num("MVP_RAM_SIZE", 0x10000);
    g_mem = (uint8_t*)calloc(1, (size_t)g_size);
    if (!g_mem) {
        fprintf(stderr, "mvp ram: cannot allocate %llu bytes\n", (unsigned long long)g_size);
        exit(1);
    }

    const char* init = getenv("MVP_RAM_INIT");
    if (init && *init) {
        FILE* f = fopen(init, "rb");
        if (!f) {
            fprintf(stderr, "mvp ram: cannot read %s\n", init);
            exit(1);
        }
        size_t n = fread(g_mem, 1, (size_t)g_size, f);
        fclose(f);
        printf("mvp ram: loaded %zu bytes from %s\n", n, init);
    }

    mvp_device dev;
    memset(&dev, 0, sizeof dev);
    dev.name = "ram";
    dev.base = base;
    dev.size = g_size;
    dev.read = ram_read;
    dev.write = ram_write;
    if (mvp_add_device(&dev) < 0) exit(1);
}
