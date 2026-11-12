/* IEEE 1800-2023 38.36.1 (tests/spec/38.36-vpi-cbstmt.sv). */
#include <stdio.h>
#include "sv_vpi_user.h"

static vpiHandle cb;

static PLI_INT32 onStmt(p_cb_data d) {
    vpi_printf("@%u cbStmt: %s line %d\n", d->time->low, vpi_get_str(vpiType, d->obj), vpi_get(vpiLineNo, d->obj));
    return 0;
}
/* The assignment `n = n + 1` inside the delay control inside repeat. */
static PLI_INT32 arm(PLI_BYTE8* u) {
    (void)u;
    vpiHandle top = vpi_handle_by_name("top", NULL);
    vpiHandle init = vpi_scan(vpi_iterate(vpiProcess, top));
    vpiHandle blk = vpi_handle(vpiStmt, init);
    vpiHandle it = vpi_iterate(vpiStmt, blk), s, rep = NULL;
    while ((s = vpi_scan(it)))
        if (vpi_get(vpiType, s) == vpiRepeat && !rep) rep = s;
    vpiHandle body = vpi_handle(vpiStmt, rep);
    vpiHandle first = vpi_scan(vpi_iterate(vpiStmt, body));       /* #2 n = n + 1 */
    vpiHandle assign = vpi_handle(vpiStmt, first);
    static s_vpi_time t = {vpiSimTime};
    s_cb_data d = {cbStmt, onStmt, assign, &t, NULL, 0, NULL};
    cb = vpi_register_cb(&d);
    return 0;
}
static PLI_INT32 disarm(PLI_BYTE8* u) {
    (void)u;
    vpi_remove_cb(cb);
    return 0;
}
static void reg(const char* name, PLI_INT32 (*fn)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, fn, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    reg("$arm", arm);
    reg("$disarm", disarm);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
