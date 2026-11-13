/* IEEE 1800-2023 40.5.1 (tests/spec/40.5-vpi-cov-instance.sv): coverage
   stopped and started on one instance below the top. */
#include <stddef.h>
#include "sv_vpi_user.h"

static vpiHandle inst(const char* n) { return vpi_handle_by_name((PLI_BYTE8*)n, NULL); }
static PLI_INT32 show(PLI_BYTE8* u) {
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL), it = vpi_iterate(vpiArgument, call);
    s_vpi_value v = {vpiStringVal};
    (void)u;
    vpi_get_value(vpi_scan(it), &v);
    vpi_free_object(it);
    vpi_printf("%s: u1 %d", v.value.str, vpi_get(vpiToggleCoverage, inst("top.u1")));
    vpi_printf(" u2 %d\n", vpi_get(vpiToggleCoverage, inst("top.u2")));
    return 0;
}
static PLI_INT32 ctl(PLI_BYTE8* u) {
    int op = u[0] == 's' ? (u[2] == 'o' ? vpiCoverageStop : vpiCoverageStart) : vpiCoverageReset;
    if (!vpi_control(op, vpiToggleCoverage, inst("top.u1"))) vpi_printf("%s failed\n", u);
    return 0;
}
static void reg(const char* name, PLI_INT32 (*f)(PLI_BYTE8*), const char* ud) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, f, NULL, NULL, (PLI_BYTE8*)ud};
    vpi_register_systf(&d);
}
static void boot(void) {
    reg("$show", show, NULL);
    reg("$stop_u1", ctl, "stop");
    reg("$start_u1", ctl, "start");
    reg("$reset_u1", ctl, "reset");
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
