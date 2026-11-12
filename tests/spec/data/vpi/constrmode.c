/* IEEE 1800-2023 37.31 (tests/spec/37-vpi-constraint-modes.sv): vpiSoft,
   dist constraints, and the constraint blocks of a class object. */
#include <stdio.h>
#include "sv_vpi_user.h"

static int ival(vpiHandle h) {
    s_vpi_value v = {vpiIntVal};
    vpi_get_value(h, &v);
    return (int)v.value.integer;
}
static void dist(vpiHandle d) {
    vpiHandle it = vpi_iterate(vpiDistItem, d), k, r;
    vpi_printf(" %s", vpi_get_str(vpiType, d));
    vpi_printf(" of %s\n", vpi_get_str(vpiName, vpi_handle(vpiExpr, d)));
    while ((k = vpi_scan(it))) {
        vpi_printf("item %s", vpi_get(vpiDistType, k) == vpiEqualDist ? "vpiEqualDist" : "vpiDivDist");
        r = vpi_handle(vpiValueRange, k);
        if (vpi_get(vpiType, r) == vpiValueRange)
            vpi_printf(" range %d:%d", ival(vpi_handle(vpiLeftRange, r)), ival(vpi_handle(vpiRightRange, r)));
        else
            vpi_printf(" value %d", ival(r));
        vpi_printf(" weight %d\n", ival(vpi_handle(vpiWeight, k)));
    }
}
static int shown;
static PLI_INT32 show(PLI_BYTE8* u) {
    vpiHandle var = vpi_handle_by_name((PLI_BYTE8*)"top.p", NULL), obj = vpi_handle(vpiClassObj, var);
    vpiHandle it, c, items, x;
    (void)u;
    if (!shown++) {
        it = vpi_iterate(vpiConstraint, vpi_handle(vpiClassDefn, var));
        while ((c = vpi_scan(it))) {
            if (vpi_get_str(vpiName, c)[1] != 'b') continue;
            vpi_printf("class cb:");
            items = vpi_iterate(vpiConstraintItem, c);
            while ((x = vpi_scan(items))) {
                vpi_printf(" soft %d", vpi_get(vpiSoft, x));
                if (vpi_get(vpiType, x) == vpiDistribution) dist(x);
                else vpi_printf(", then");
            }
        }
    }
    vpi_printf("object:");
    it = vpi_iterate(vpiConstraint, obj);
    while ((c = vpi_scan(it))) {
        vpi_printf(" %s", vpi_get_str(vpiName, c));
        vpi_printf(" %d,", vpi_get(vpiIsConstraintEnabled, c));
        x = c;
    }
    vpi_printf(" parent is object %d\n", vpi_compare_objects(vpi_handle(vpiParent, x), obj));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$show", show, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
