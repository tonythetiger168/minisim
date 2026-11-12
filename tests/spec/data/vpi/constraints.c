/* IEEE 1800-2023 37.31 (tests/spec/37-vpi-constraints.sv): the constraint
   blocks of a class definition and their items. */
#include <stdio.h>
#include "sv_vpi_user.h"

static const char* s(PLI_INT32 p, vpiHandle h) {
    static char buf[4][256];
    static int k;
    const char* r = h ? vpi_get_str(p, h) : NULL;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "%s", r ? r : "-");
    return buf[k];
}
static void items(vpiHandle it, int depth) {
    vpiHandle x, y;
    while (it && (x = vpi_scan(it))) {
        PLI_INT32 t = vpi_get(vpiType, x);
        vpi_printf("%*s%s", depth * 2, "", s(vpiType, x));
        if (t == vpiImplication || t == vpiConstrIf || t == vpiConstrIfElse) {
            vpi_printf(" if %s\n", s(vpiDecompile, vpi_handle(vpiCondition, x)));
            items(vpi_iterate(vpiConstraintExpr, x), depth + 1);
            if (t == vpiConstrIfElse) {
                vpi_printf("%*selse\n", depth * 2, "");
                items(vpi_iterate(vpiElseConst, x), depth + 1);
            }
        } else if (t == vpiConstrForEach) {
            vpi_printf(" %s\n", s(vpiDecompile, vpi_handle(vpiVariables, x)));
            items(vpi_iterate(vpiConstraintExpr, x), depth + 1);
        } else if (t == vpiSoftDisable) {
            vpi_printf(" %s\n", s(vpiDecompile, vpi_handle(vpiExpr, x)));
        } else if (t == vpiConstraintOrdering) {
            vpiHandle bi = vpi_iterate(vpiSolveBefore, x), ai = vpi_iterate(vpiSolveAfter, x);
            while (bi && (y = vpi_scan(bi))) vpi_printf(" %s", s(vpiDecompile, y));
            vpi_printf(" before");
            while (ai && (y = vpi_scan(ai))) vpi_printf(" %s", s(vpiDecompile, y));
            vpi_printf("\n");
        } else {
            vpi_printf(" %s\n", s(vpiDecompile, x));
        }
    }
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle v = vpi_handle_by_name((PLI_BYTE8*)"top.p", NULL);
    vpiHandle d = vpi_handle(vpiClassDefn, v), it, c;
    (void)u;
    it = vpi_iterate(vpiConstraint, d);
    while (it && (c = vpi_scan(it))) {
        vpi_printf("%s %s enabled %d virtual %d automatic %d of %s\n", s(vpiType, c), s(vpiName, c),
                   vpi_get(vpiIsConstraintEnabled, c), vpi_get(vpiVirtual, c), vpi_get(vpiAutomatic, c),
                   s(vpiName, vpi_handle(vpiClassDefn, c)));
        items(vpi_iterate(vpiConstraintItem, c), 1);
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$constraints", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
