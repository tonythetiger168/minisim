/* IEEE 1800-2023 39.5.2 (tests/spec/39.5-vpi-assert-step.sv): assertion
   stepping with vpiAssertionEnableStep / DisableStep and the
   cbAssertionStepSuccess / cbAssertionStepFailure callbacks. */
#include <stdio.h>
#include "sv_vpi_user.h"

static vpiHandle a;
static PLI_INT32 cb(PLI_INT32 reason, p_vpi_time t, vpiHandle h, p_vpi_attempt_info info, PLI_BYTE8* u) {
    (void)h; (void)u;
    if (reason == cbAssertionStepSuccess || reason == cbAssertionStepFailure)
        vpi_printf("%u: step %s, attempt from %u, state %d -> %d\n", t->low,
                   reason == cbAssertionStepSuccess ? "success" : "failure", info->attemptStartTime.low,
                   info->detail.step->stateFrom, info->detail.step->stateTo);
    else
        vpi_printf("%u: %s, attempt from %u\n", t->low, reason == cbAssertionSuccess ? "success" : "failure",
                   info->attemptStartTime.low);
    return 0;
}
static PLI_INT32 on(PLI_BYTE8* u) {
    s_vpi_error_info e;
    (void)u;
    a = vpi_handle_by_name((PLI_BYTE8*)"top.p", NULL);
    vpi_register_assertion_cb(a, cbAssertionStepSuccess, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionStepFailure, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionSuccess, cb, NULL);
    vpi_register_assertion_cb(a, cbAssertionFailure, cb, NULL);
    vpi_printf("enable step: %d\n", vpi_control(vpiAssertionEnableStep, a));
    if (!vpi_control(vpiAssertionEnableStep, vpi_handle_by_name((PLI_BYTE8*)"top.imm", NULL))) {
        vpi_chk_error(&e);
        vpi_printf("immediate: %s\n", e.message);
    }
    return 0;
}
static PLI_INT32 off(PLI_BYTE8* u) {
    (void)u;
    vpi_printf("disable step: %d\n", vpi_control(vpiAssertionDisableStep, a));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$step_on", on, NULL, NULL, NULL};
    vpi_register_systf(&d);
    d.tfname = (PLI_BYTE8*)"$step_off";
    d.calltf = off;
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
