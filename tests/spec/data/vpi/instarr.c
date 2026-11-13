/* IEEE 1800-2023 37.11 (tests/spec/37.11-vpi-instance-arrays.sv): instance
   arrays, their ranges and elements, and from an element its array. */
#include <stdio.h>
#include "sv_vpi_user.h"

static int ival(vpiHandle h) {
    s_vpi_value v = {vpiIntVal, {0}};
    vpi_get_value(h, &v);
    return v.value.integer;
}
static void show(vpiHandle a) {
    vpiHandle it, r, e;
    /* vpi_get_str reuses one buffer: one string per vpi_printf */
    vpi_printf("%s", vpi_get_str(vpiType, a));
    vpi_printf(" %s", vpi_get_str(vpiFullName, a));
    vpi_printf(" name %s", vpi_get_str(vpiName, a));
    vpi_printf(" size %d range %d:%d ranges", vpi_get(vpiSize, a), ival(vpi_handle(vpiLeftRange, a)),
               ival(vpi_handle(vpiRightRange, a)));
    it = vpi_iterate(vpiRange, a);
    while ((r = vpi_scan(it)))
        vpi_printf(" [%d:%d]", ival(vpi_handle(vpiLeftRange, r)), ival(vpi_handle(vpiRightRange, r)));
    vpi_printf(":");
    it = vpi_iterate(vpiInstance, a);
    if (!it) it = vpi_iterate(vpiPrimitive, a);
    while ((e = vpi_scan(it))) vpi_printf(" %s(%d)", vpi_get_str(vpiName, e), ival(vpi_handle(vpiIndex, e)));
    vpi_printf("\n");
}
static void elem(vpiHandle e) {
    vpiHandle a = vpi_handle(vpiModuleArray, e), i = vpi_handle(vpiIndex, e);
    vpi_printf("%s: ", vpi_get_str(vpiName, e));
    if (i) vpi_printf("%s", vpi_get_str(vpiFullName, e)); else vpi_printf("index none");
    vpi_printf(" array %s\n", a ? vpi_get_str(vpiName, a) : "none");
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8*)"top", NULL), it, a, mu = 0, mm = 0;
    PLI_INT32 ix[2] = {1, 0};
    (void)u;
    it = vpi_iterate(vpiInstanceArray, top);
    while ((a = vpi_scan(it))) show(a);
    vpi_printf("modules:");
    it = vpi_iterate(vpiModuleArray, top);
    while ((a = vpi_scan(it))) {
        vpi_printf(" %s", vpi_get_str(vpiName, a));
        if (!mu) mu = a; else mm = a;
    }
    vpi_printf("\nprimitives:");
    it = vpi_iterate(vpiPrimitiveArray, top);
    while ((a = vpi_scan(it))) {
        vpiHandle pi = vpi_iterate(vpiPrimitive, a), p;
        while ((p = vpi_scan(pi))) vpi_printf(" %s", vpi_get_str(vpiName, p));
    }
    vpi_printf("\n");
    elem(vpi_handle_by_index(mu, 2));
    elem(vpi_handle_by_multi_index(mm, 2, ix));
    elem(vpi_handle_by_name((PLI_BYTE8*)"top.solo", NULL));
    vpi_printf("by name: %s\n", vpi_get_str(vpiName, vpi_handle_by_name((PLI_BYTE8*)"top.u", NULL)));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$arrays", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
