/* IEEE 1800-2023 37.16 details 14-15, 37.21 (tests/spec/37-vpi-force-drivers.sv):
   force and procedural assign statements in effect as drivers and loads. */
#include <stddef.h>
#include "sv_vpi_user.h"

static void list(const char* sig, PLI_INT32 rel, const char* name) {
    vpiHandle h = vpi_handle_by_name((PLI_BYTE8*)sig, NULL), it, c;
    vpi_printf("  %s %s:", sig, name);
    it = vpi_iterate(rel, h);
    while (it && (c = vpi_scan(it))) vpi_printf(" %s@%d", vpi_get_str(vpiType, c), vpi_get(vpiLineNo, c));
    vpi_printf("\n");
}
static PLI_INT32 show(PLI_BYTE8* u) {
    s_vpi_time t = {vpiSimTime, 0, 0, 0};
    (void)u;
    vpi_get_time(NULL, &t);
    vpi_printf("time %u\n", (unsigned)t.low);
    list("top.n", vpiDriver, "drivers");
    list("top.r", vpiDriver, "drivers");
    list("top.r", vpiLocalDriver, "local drivers");
    list("top.src", vpiLoad, "loads");
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$show", show, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
