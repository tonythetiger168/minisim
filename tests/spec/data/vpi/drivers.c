/* IEEE 1800-2023 37.16, 37.17 (tests/spec/37-vpi-drivers.sv): the drivers
   and loads of nets and variables, local and across the hierarchy, and the
   instance ports a net connects to; the ports of a net's own module (an
   input drives it, an output loads it, 37.16 detail 17); the drivers and
   loads of one bit (detail 18); primitive terminals (37.46). */
#include <stdio.h>
#include "sv_vpi_user.h"

static void show(vpiHandle h) {
    const char* t = vpi_get_str(vpiType, h);
    vpi_printf(" %s", t ? t : "?");
    if (vpi_get(vpiType, h) == vpiPort) {
        char mod[128];
        snprintf(mod, sizeof mod, "%s", vpi_get_str(vpiFullName, vpi_handle(vpiModule, h)));
        vpi_printf("(%s.%s dir %d)", mod, vpi_get_str(vpiName, h), vpi_get(vpiDirection, h));
    } else if (vpi_get(vpiType, h) == vpiPrimTerm) {
        vpi_printf("(%s term %d)", vpi_get_str(vpiName, vpi_handle(vpiPrimitive, h)), vpi_get(vpiTermIndex, h));
    } else {
        vpi_printf("@%d", vpi_get(vpiLineNo, h));
    }
}
static void listH(vpiHandle h, const char* sig, PLI_INT32 rel, const char* name) {
    vpiHandle it, c;
    vpi_printf("%s %s:", sig, name);
    it = vpi_iterate(rel, h);
    while (it && (c = vpi_scan(it))) show(c);
    vpi_printf("\n");
}
static void list(const char* sig, PLI_INT32 rel, const char* name) {
    listH(vpi_handle_by_name((PLI_BYTE8*)sig, NULL), sig, rel, name);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    static const char* sigs[] = {"top.a", "top.n", "top.y", "top.v", "top.u.i", "top.u.o", "top.w", "top.g_out", 0};
    static const int bits[] = {0, 2, 3, -1};
    vpiHandle w = vpi_handle_by_name((PLI_BYTE8*)"top.w", NULL);
    (void)u;
    for (int k = 0; sigs[k]; k++) {
        list(sigs[k], vpiLocalDriver, "local drivers");
        list(sigs[k], vpiLocalLoad, "local loads");
        list(sigs[k], vpiDriver, "drivers");
        list(sigs[k], vpiLoad, "loads");
        list(sigs[k], vpiPortInst, "instance ports");
    }
    for (int k = 0; bits[k] >= 0; k++) {
        char name[32];
        vpiHandle b = vpi_handle_by_index(w, bits[k]);
        snprintf(name, sizeof name, "top.w[%d]", bits[k]);
        listH(b, name, vpiLocalDriver, "local drivers");
        listH(b, name, vpiLocalLoad, "local loads");
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$drivers", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
