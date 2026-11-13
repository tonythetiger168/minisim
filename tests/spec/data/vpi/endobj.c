/* IEEE 1800-2023 38.36.1 (tests/spec/38.36-vpi-end-of-object.sv):
   cbEndOfObject on a frame and a thread, and the handles cbReclaimObj and
   cbEndOfObject accept. */
#include <stddef.h>
#include "sv_vpi_user.h"

static PLI_INT32 ended(p_cb_data d) {
    s_vpi_time t = {vpiSimTime, 0, 0, 0};
    vpi_get_time(NULL, &t);
    vpi_printf("%u: end of %s (time %s)\n", (unsigned)t.low, (const char*)d->user_data, d->time ? "passed" : "NULL");
    return 0;
}
static void on(vpiHandle h, PLI_INT32 reason, const char* what) {
    s_vpi_time t = {vpiSimTime, 0, 0, 0};
    s_cb_data d = {reason, ended, h, &t, NULL, 0, (PLI_BYTE8*)what};
    s_vpi_error_info e;
    if (!vpi_register_cb(&d) && vpi_chk_error(&e)) vpi_printf("%s: %s\n", what, e.message);
}
static PLI_INT32 watch(PLI_BYTE8* u) {
    (void)u;
    on(vpi_handle(vpiFrame, NULL), cbEndOfObject, "frame");
    on(vpi_handle(vpiThread, NULL), cbEndOfObject, "thread");
    on(NULL, cbEndOfObject, "no object");
    on(vpi_handle_by_name((PLI_BYTE8*)"top", NULL), cbReclaimObj, "reclaim of a module");
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$watch", watch, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
