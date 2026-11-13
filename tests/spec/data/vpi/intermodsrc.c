/* IEEE 1800-2023 38.21 (tests/spec/38.21-vpi-intermod-sources.sv): the
   delay of each inter-module path on a net with several sources. */
#include <stddef.h>
#include "sv_vpi_user.h"

static vpiHandle port(const char* inst, int k) {
    vpiHandle it = vpi_iterate(vpiPort, vpi_handle_by_name((PLI_BYTE8*)inst, NULL)), p = 0;
    while (it && (p = vpi_scan(it)))
        if (k-- == 0) { vpi_free_object(it); break; }
    return p;
}
static void put(vpiHandle path, PLI_UINT32 v) {
    s_vpi_time t[1] = {{vpiSimTime, 0, 0, 0.0}};
    s_vpi_delay d = {t, 1, vpiSimTime, 0, 0, 0};
    t[0].low = v;
    vpi_put_delays(path, &d);
}
static void show(const char* what, vpiHandle path) {
    s_vpi_time t[3];
    s_vpi_delay d = {t, 3, vpiSimTime, 0, 0, 0};
    vpi_get_delays(path, &d);
    vpi_printf("%s path delays %u %u %u\n", what, t[0].low, t[1].low, t[2].low);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle in = port("top.u3", 0);
    vpiHandle p1 = vpi_handle_multi(vpiInterModPath, port("top.u1", 2), in);
    vpiHandle p2 = vpi_handle_multi(vpiInterModPath, port("top.u2", 2), in);
    (void)u;
    put(p1, 3);
    put(p2, 7);
    show("u1", p1);
    show("u2", p2);
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$intermod_src", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
