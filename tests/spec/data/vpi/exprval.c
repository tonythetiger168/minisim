/* IEEE 1800-2023 37.35, 38.15 (tests/spec/37.35-vpi-expr-values.sv): the
   value of an expression now. */
#include <stdio.h>
#include "sv_vpi_user.h"

static void show(vpiHandle e) {
    s_vpi_value v = {vpiObjTypeVal, {0}};
    vpi_get_value(e, &v);
    switch (v.format) {
        case vpiRealVal: vpi_printf("%g", v.value.real); break;
        case vpiIntVal: vpi_printf("%d", v.value.integer); break;
        default: {
            s_vpi_value d = {vpiDecStrVal, {0}};
            vpi_get_value(e, &d);
            vpi_printf("%s", d.value.str);
        }
    }
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8*)"top", NULL);
    vpiHandle it = vpi_iterate(vpiContAssign, top), ca;
    (void)u;
    while ((ca = vpi_scan(it))) {
        vpiHandle lhs = vpi_handle(vpiLhs, ca), rhs = vpi_handle(vpiRhs, ca), oi, op;
        s_vpi_value d = {vpiDecStrVal, {0}};
        vpi_printf("%s: ", vpi_get_str(vpiName, lhs));
        show(rhs);
        vpi_get_value(lhs, &d);
        vpi_printf(" = %s (operands", d.value.str);
        oi = vpi_iterate(vpiArgument, rhs);
        if (!oi) oi = vpi_iterate(vpiOperand, rhs);
        while (oi && (op = vpi_scan(oi))) {
            vpi_printf(" ");
            show(op);
        }
        vpi_printf(")\n");
    }
    return 0;
}
/* Task t's assignment reads its automatic argument k: no value. */
static PLI_INT32 autos(PLI_BYTE8* u) {
    vpiHandle t = vpi_handle_by_name((PLI_BYTE8*)"top.t", NULL), st, rhs, it;
    s_vpi_value v = {vpiIntVal, {0}};
    s_vpi_error_info e;
    (void)u;
    st = vpi_handle(vpiStmt, t);
    if (vpi_get(vpiType, st) == vpiBegin) {
        it = vpi_iterate(vpiStmt, st);
        st = vpi_scan(it);
        while (st && vpi_get(vpiType, st) != vpiAssignment) st = vpi_scan(it);
    }
    rhs = vpi_handle(vpiRhs, st);
    vpi_get_value(rhs, &v);
    vpi_printf("automatic: no value, error %d\n", vpi_chk_error(&e) != 0);
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$exprs", run, NULL, NULL, NULL};
    s_vpi_systf_data a = {vpiSysTask, 0, (PLI_BYTE8*)"$autos", autos, NULL, NULL, NULL};
    vpi_register_systf(&d);
    vpi_register_systf(&a);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
