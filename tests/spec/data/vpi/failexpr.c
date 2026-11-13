/* IEEE 1800-2023 39.4.2 (tests/spec/39.4-vpi-fail-expr.sv): failExpr. */
#include <stdio.h>
#include "sv_vpi_user.h"

static PLI_INT32 failed(PLI_INT32 reason, p_vpi_time t, vpiHandle a, p_vpi_attempt_info info, PLI_BYTE8* ud) {
    vpiHandle e = info ? info->detail.failExpr : NULL;
    (void)reason;
    vpi_printf("%s failed at %d: ", vpi_get_str(vpiName, a), (int)t->low);
    if (!e) {
        vpi_printf("NULL\n");
        return 0;
    }
    vpi_printf("%s ", vpi_get_str(vpiType, e));
    vpi_printf("%s\n", vpi_get_str(vpiDecompile, e));
    (void)ud;
    return 0;
}
static PLI_INT32 watch(PLI_BYTE8* ud) {
    vpiHandle it = vpi_iterate(vpiAssertion, NULL), a;
    (void)ud;
    while ((a = vpi_scan(it))) vpi_register_assertion_cb(a, cbAssertionFailure, failed, NULL);
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$watch", watch, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
