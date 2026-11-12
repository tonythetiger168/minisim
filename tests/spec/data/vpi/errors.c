/* IEEE 1800-2023 38.36.3 (tests/spec/38.36-vpi-cberror.sv): cbPLIError runs
   when a VPI call fails, cbError when the simulation reports a run-time
   error; vpi_chk_error in the callback gives the error. */
#include <stdio.h>
#include "sv_vpi_user.h"

static vpiHandle pliCb;
static PLI_INT32 onErr(p_cb_data d) {
    s_vpi_error_info e;
    int lvl = vpi_chk_error(&e);
    vpi_printf("%s at %d: level %d: %s\n", d->reason == cbError ? "cbError" : "cbPLIError",
               d->time ? (int)d->time->low : -1, lvl, lvl ? e.message : "");
    vpi_handle_by_name((PLI_BYTE8*)"no.such.thing", NULL);   /* fails again: no nested callback */
    return 0;
}
static PLI_INT32 run(PLI_BYTE8* u) {
    (void)u;
    vpi_handle(vpiParent, NULL);        /* an error: cbPLIError */
    vpi_get(vpiSize, NULL);             /* no error (vpiUndefined) */
    vpi_remove_cb(pliCb);
    vpi_handle(vpiParent, NULL);        /* removed: no callback */
    vpi_printf("after remove: chk_error %d\n", vpi_chk_error(NULL));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$errs", run, NULL, NULL, NULL};
    s_vpi_time t = {vpiSimTime};
    s_cb_data c = {cbPLIError, onErr, NULL, &t, NULL, 0, NULL};
    vpi_register_systf(&d);
    pliCb = vpi_register_cb(&c);
    c.reason = cbError;
    vpi_register_cb(&c);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
