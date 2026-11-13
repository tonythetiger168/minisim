/* IEEE 1800-2023 39.5.2 (tests/spec/39.5-vpi-assert-step.sv): stepping of
   one attempt with vpiAssertionEnableStep / DisableStep (attempt start
   time, vpiAssertionClockSteps), the cbAssertionStepSuccess /
   cbAssertionStepFailure callbacks, and vpiAssertionKill of one attempt. */
#include <stddef.h>
#include "sv_vpi_user.h"

static vpiHandle a;
static PLI_INT32 cb(PLI_INT32 reason, p_vpi_time t, vpiHandle h, p_vpi_attempt_info info, PLI_BYTE8* u) {
    (void)h; (void)u;
    if (reason == cbAssertionStepSuccess || reason == cbAssertionStepFailure) {
        int k;
        vpi_printf("%u: step %s, attempt from %u, state %d -> %d\n", t->low,
                   reason == cbAssertionStepSuccess ? "success" : "failure", info->attemptStartTime.low,
                   info->detail.step->stateFrom, info->detail.step->stateTo);
        for (k = 0; k < info->detail.step->matched_expression_count; k++)   /* the expressions that matched */
            vpi_printf("  matched %s\n", vpi_get_str(vpiDecompile, info->detail.step->matched_exprs[k]));
    } else if (reason == cbAssertionKill)
        vpi_printf("%u: kill%s\n", t->low, info ? " (with info)" : "");
    else {
        vpi_printf("%u: %s, attempt from %u\n", t->low, reason == cbAssertionSuccess ? "success" : "failure",
                   info->attemptStartTime.low);
        if (reason == cbAssertionFailure && info->detail.failExpr)   /* the expression that failed */
            vpi_printf("  failExpr %s\n", vpi_get_str(vpiDecompile, info->detail.failExpr));
    }
    return 0;
}
static s_vpi_time at(PLI_UINT32 t) {
    s_vpi_time r = {vpiSimTime, 0, t, 0.0};
    return r;
}
static void err(const char* what) {
    s_vpi_error_info e;
    if (vpi_chk_error(&e)) vpi_printf("%s: %s\n", what, e.message);
}
static PLI_INT32 on(PLI_BYTE8* u) {
    s_vpi_time t5 = at(5), t25 = at(25), t65 = at(65);
    (void)u;
    a = vpi_handle_by_name((PLI_BYTE8*)"top.p", NULL);
    vpi_register_assertion_cb(a, cbAssertionStepSuccess, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionStepFailure, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionSuccess, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionFailure, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionKill, cb, NULL);
    vpi_printf("enable step 5: %d\n", vpi_control(vpiAssertionEnableStep, a, &t5, vpiAssertionClockSteps));
    vpi_printf("enable step 25: %d\n", vpi_control(vpiAssertionEnableStep, a, &t25, vpiAssertionClockSteps));
    vpi_printf("enable step 65: %d\n", vpi_control(vpiAssertionEnableStep, a, &t65, vpiAssertionClockSteps));
    if (!vpi_control(vpiAssertionEnableStep, vpi_handle_by_name((PLI_BYTE8*)"top.imm", NULL), &t5, vpiAssertionClockSteps))
        err("immediate");
    if (!vpi_control(vpiAssertionEnableStep, a, &t5, 0)) err("no step control");
    if (!vpi_control(vpiAssertionSysOff, a)) err("system control on an assertion");
    return 0;
}
static PLI_INT32 off(PLI_BYTE8* u) {
    s_vpi_time t65 = at(65);
    (void)u;
    vpi_printf("disable step 65: %d\n", vpi_control(vpiAssertionDisableStep, a, &t65));
    return 0;
}
static PLI_INT32 kill(PLI_BYTE8* u) {
    s_vpi_time t65 = at(65);
    (void)u;
    vpi_printf("kill 65: %d\n", vpi_control(vpiAssertionKill, a, &t65));
    return 0;
}
static void task(const char* name, PLI_INT32 (*f)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, f, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    task("$step_on", on);
    task("$step_off", off);
    task("$kill_attempt", kill);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
