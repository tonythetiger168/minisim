/* IEEE 1800-2023 38.36.3 (tests/spec/38.36-vpi-interactive-cbs.sv):
   cbEnterInteractive, cbExitInteractive and cbInteractiveScopeChange. */
#include <stddef.h>
#include "sv_vpi_user.h"

static PLI_INT32 cb(p_cb_data d) {
    if (d->reason == cbInteractiveScopeChange)
        vpi_printf("%u: scope change to %s\n", d->time->low, d->obj ? vpi_get_str(vpiFullName, d->obj) : "NULL");
    else
        vpi_printf("%u: %s interactive\n", d->time->low, d->reason == cbEnterInteractive ? "enter" : "exit");
    return 0;
}
static PLI_INT32 start(p_cb_data u) {
    static const PLI_INT32 rs[] = {cbEnterInteractive, cbExitInteractive, cbInteractiveScopeChange};
    size_t k;
    (void)u;
    for (k = 0; k < 3; k++) {
        s_cb_data d = {0};
        d.reason = rs[k];
        d.cb_rtn = cb;
        if (!vpi_register_cb(&d)) vpi_printf("cannot register %d\n", (int)rs[k]);
    }
    return 0;
}
static void boot(void) {
    s_cb_data d = {0};
    d.reason = cbStartOfSimulation;
    d.cb_rtn = start;
    vpi_register_cb(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
