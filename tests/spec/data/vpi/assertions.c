/* The assertion API (IEEE 1800-2023 39.3 to 39.5). */
#include <string.h>
#include "sv_vpi_user.h"

static const char *why(PLI_INT32 r) {
    switch (r) {
        case cbAssertionStart: return "start";
        case cbAssertionSuccess: return "success";
        case cbAssertionVacuousSuccess: return "vacuous success";
        case cbAssertionFailure: return "failure";
        case cbAssertionDisabledEvaluation: return "disabled";
        case cbAssertionDisable: return "control: disable";
        case cbAssertionEnable: return "control: enable";
        case cbAssertionReset: return "control: reset";
        case cbAssertionDisablePassAction: return "control: pass action off";
        default: return "?";
    }
}

static PLI_INT32 on_assert(PLI_INT32 reason, p_vpi_time t, vpiHandle a, p_vpi_attempt_info info, PLI_BYTE8 *ud) {
    (void)ud;
    vpi_printf("%u %s: %s", t->low, vpi_get_str(vpiName, a), why(reason));
    if (info) vpi_printf(" (attempt from %u)", info->attemptStartTime.low);
    vpi_printf("\n");
    return 0;
}

static PLI_INT32 on_sys(p_cb_data d) {
    vpi_printf("%u assertion system: %s\n", d->time->low,
               d->reason == cbAssertionSysInitialized ? "initialized" : d->reason == cbAssertionSysEnd ? "end" : "?");
    return 0;
}

static vpiHandle a_req, c_ack;

static PLI_INT32 watch(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle it = vpi_iterate(vpiAssertion, NULL), a;
    while (it && (a = vpi_scan(it))) {
        vpi_printf("%s", vpi_get_str(vpiType, a));
        vpi_printf(" %s", vpi_get_str(vpiFullName, a));
        vpi_printf(" line %d in %s\n", (int)vpi_get(vpiLineNo, a), vpi_get_str(vpiFullName, vpi_handle(vpiScope, a)));
        const char *n = vpi_get_str(vpiName, a);
        if (!strcmp(n, "a_req")) a_req = a;
        if (!strcmp(n, "c_ack")) c_ack = a;
    }
    PLI_INT32 rs[] = {cbAssertionStart, cbAssertionSuccess, cbAssertionVacuousSuccess, cbAssertionFailure,
                      cbAssertionDisabledEvaluation, cbAssertionDisable, cbAssertionEnable, cbAssertionReset};
    for (unsigned k = 0; k < sizeof rs / sizeof rs[0]; k++) vpi_register_assertion_cb(a_req, rs[k], on_assert, NULL);
    vpi_register_assertion_cb(c_ack, cbAssertionSuccess, on_assert, NULL);
    vpi_register_assertion_cb(c_ack, cbAssertionDisable, on_assert, NULL);
    /* Wrong uses are errors. */
    if (vpi_register_assertion_cb(a_req, cbValueChange, on_assert, NULL) || !vpi_chk_error(NULL))
        vpi_printf("no error for a bad reason\n");
    return 0;
}

static PLI_INT32 control(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle it = vpi_iterate(vpiArgument, vpi_handle(vpiSysTfCall, NULL));
    s_vpi_value v = {vpiIntVal};
    vpi_get_value(vpi_scan(it), &v);
    vpi_free_object(it);
    if (v.value.integer == 0) vpi_control(vpiAssertionDisable, a_req);
    else {
        vpi_control(vpiAssertionEnable, a_req);
        vpi_control(vpiAssertionReset, a_req);
        vpi_control(vpiAssertionDisablePassAction, a_req);
        /* The immediate assertion inside the named block. */
        vpiHandle blk = vpi_handle_by_name("top.blk", NULL);
        vpiHandle ai = vpi_iterate(vpiAssertion, blk), im;
        while (ai && (im = vpi_scan(ai))) {
            vpi_printf("in %s: ", vpi_get_str(vpiFullName, blk));
            vpi_printf("%s", vpi_get_str(vpiType, im));
            vpi_printf(" %s\n", vpi_get_str(vpiName, im));
            vpi_register_assertion_cb(im, cbAssertionSuccess, on_assert, NULL);
        }
    }
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$watch";
    d.calltf = watch;
    vpi_register_systf(&d);
    d.tfname = "$control";
    d.calltf = control;
    vpi_register_systf(&d);
    s_cb_data cb;
    memset(&cb, 0, sizeof cb);
    s_vpi_time t = {vpiSimTime};
    cb.time = &t;
    cb.cb_rtn = on_sys;
    cb.reason = cbAssertionSysInitialized;
    vpi_register_cb(&cb);
    cb.reason = cbAssertionSysEnd;
    vpi_register_cb(&cb);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
