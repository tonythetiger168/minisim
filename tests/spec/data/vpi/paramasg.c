/* IEEE 1800-2023 37.28, 37.11, 37.40 (tests/spec/37.28-vpi-param-assign.sv):
   type parameters, defaults, param assigns, def params, connection lists
   and net declaration assignments. */
#include <stdio.h>
#include "sv_vpi_user.h"

static int ival(vpiHandle h) {
    s_vpi_value v = {vpiIntVal, {0}};
    vpi_get_value(h, &v);
    return v.value.integer;
}
/* vpi_get_str reuses one buffer: one string per vpi_printf */
static void inst(const char* path) {
    vpiHandle m = vpi_handle_by_name((PLI_BYTE8*)path, NULL), it, p;
    vpi_printf("%s:", vpi_get_str(vpiName, m));
    it = vpi_iterate(vpiParameter, m);
    while ((p = vpi_scan(it))) {
        vpi_printf(" %s", vpi_get_str(vpiName, p));
        if (vpi_get(vpiType, p) == vpiTypeParameter) {
            vpi_printf("=%s", vpi_get_str(vpiType, p));
            continue;
        }
        vpi_printf("=%d (default %d, local %d)", ival(p), ival(vpi_handle(vpiExpr, p)), vpi_get(vpiLocalParam, p));
    }
    vpi_printf("\n");
    it = vpi_iterate(vpiParamAssign, m);
    while (it && (p = vpi_scan(it))) {
        vpiHandle l = vpi_handle(vpiLhs, p), r = vpi_handle(vpiRhs, p);
        vpi_printf("  assign %s", vpi_get_str(vpiName, l));
        if (vpi_get(vpiType, l) == vpiTypeParameter) vpi_printf(" -> %s", r ? "typespec" : "none");
        else vpi_printf(" = %d", ival(r));
        vpi_printf(" by name %d\n", vpi_get(vpiConnByName, p));
    }
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8*)"top", NULL), it, d, a, op, ca;
    (void)u;
    inst("top.u1");
    inst("top.u2");
    it = vpi_iterate(vpiDefParam, top);
    while (it && (d = vpi_scan(it))) {
        vpi_printf("top defparam %s", vpi_get_str(vpiName, vpi_handle(vpiLhs, d)));
        vpi_printf(" = %d\n", ival(vpi_handle(vpiRhs, d)));
    }
    a = vpi_handle_by_name((PLI_BYTE8*)"top.u3", NULL);
    op = vpi_handle(vpiExpr, a);
    vpi_printf("u3 connections: %s op %d:", vpi_get_str(vpiType, op), vpi_get(vpiOpType, op));
    it = vpi_iterate(vpiOperand, op);
    while (it && (d = vpi_scan(it))) vpi_printf(" %s", vpi_get_str(vpiDecompile, d));
    vpi_printf("\n");
    it = vpi_iterate(vpiContAssign, top);
    while (it && (ca = vpi_scan(it)))
        if (vpi_get(vpiNetDeclAssign, ca)) {
            vpi_printf("%s: net decl 1", vpi_get_str(vpiName, vpi_handle(vpiLhs, ca)));
            vpi_printf(" lhs %s rhs %d\n", vpi_get_str(vpiName, vpi_handle(vpiLhs, ca)), ival(vpi_handle(vpiRhs, ca)));
        }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$params", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
