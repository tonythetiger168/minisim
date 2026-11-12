/* IEEE 1800-2023 38.21 (tests/spec/38.21-vpi-handle-multi.sv):
   vpi_handle_multi(vpiInterModPath, ...) and the path's delays. */
#include <stdio.h>
#include "sv_vpi_user.h"

static vpiHandle port(const char* inst, int k) {
    vpiHandle it = vpi_iterate(vpiPort, vpi_handle_by_name((PLI_BYTE8*)inst, NULL)), p = 0;
    while (it && (p = vpi_scan(it)))
        if (k-- == 0) { vpi_free_object(it); break; }
    return p;
}
static void show(vpiHandle path) {
    s_vpi_time t[6];
    s_vpi_delay d = {t, 3, vpiSimTime, 0, 0, 0};
    vpi_get_delays(path, &d);
    vpi_printf("delays %u %u %u\n", t[0].low, t[1].low, t[2].low);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    s_vpi_error_info e;
    vpiHandle out = port("top.u1", 1), in = port("top.u2", 0), path, it, p;
    vpiHandle path2;
    (void)u;
    path = vpi_handle_multi(vpiInterModPath, out, in);
    vpi_printf("%s\n", vpi_get_str(vpiType, path));
    it = vpi_iterate(vpiPort, path);
    while ((p = vpi_scan(it))) vpi_printf("  port %s dir %d\n", vpi_get_str(vpiName, p), vpi_get(vpiDirection, p));
    path2 = vpi_handle_multi(vpiInterModPath, in, out);
    vpi_printf("same object either order: %d\n", vpi_compare_objects(path, path2));
    show(path);
    {
        s_vpi_time t[2] = {{vpiSimTime, 0, 3}, {vpiSimTime, 0, 5}};
        s_vpi_delay d = {t, 2, vpiSimTime, 0, 0, 0};
        vpi_put_delays(path, &d);
    }
    show(path);
    if (!vpi_handle_multi(vpiInterModPath, in, port("top.u2", 1))) {
        vpi_chk_error(&e);
        vpi_printf("error: %s\n", e.message);
    }
    if (!vpi_handle_multi(vpiModule, out, in)) {
        vpi_chk_error(&e);
        vpi_printf("error: %s\n", e.message);
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$intermod", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
