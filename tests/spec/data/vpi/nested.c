/* IEEE 1800-2023 37.17, 37.18, 37.29 (tests/spec/37.18-vpi-nested-containers.sv):
   containers of unpacked structs, of class handles and of containers. */
#include <stdio.h>
#include "sv_vpi_user.h"

static int ival(vpiHandle h) {
    s_vpi_value v = {vpiIntVal, {0}};
    vpi_get_value(h, &v);
    return v.value.integer;
}
static void put(vpiHandle h, int x) {
    s_vpi_value v = {vpiIntVal, {0}};
    v.value.integer = x;
    vpi_put_value(h, &v, NULL, vpiNoDelay);
}
/* vpi_get_str reuses one buffer: one string per vpi_printf */
static void head(vpiHandle a) {
    vpi_printf("%s", vpi_get_str(vpiFullName, a));
    vpi_printf(": type %d array type %d size %d", vpi_get(vpiType, a), vpi_get(vpiArrayType, a), vpi_get(vpiSize, a));
}
static void elems(vpiHandle a) {
    vpiHandle it = vpi_iterate(vpiElement, a), e = vpi_scan(it), m;
    head(a);
    vpi_printf(" elements %d\n", vpi_get(vpiType, e));
    for (; e; e = vpi_scan(it)) {
        int t = vpi_get(vpiType, e);
        vpi_printf("  %s", vpi_get_str(vpiFullName, e));
        if (t == vpiArrayVar) {
            vpiHandle ei = vpi_iterate(vpiElement, e), x;
            vpi_printf(" type %d array type %d size %d:", t, vpi_get(vpiArrayType, e), vpi_get(vpiSize, e));
            while ((x = vpi_scan(ei))) vpi_printf(" %d", ival(x));
        } else if (t == vpiClassVar) {
            vpiHandle o = vpi_handle(vpiClassObj, e), pi = vpi_iterate(vpiVariables, o);
            vpi_printf(" type %d -> object", t);
            while ((m = vpi_scan(pi))) {
                vpi_printf(" %s", vpi_get_str(vpiName, m));
                vpi_printf("=%d", ival(m));
            }
        } else {
            vpiHandle mi = vpi_iterate(vpiMember, e);
            vpi_printf(" type %d:", t);
            while ((m = vpi_scan(mi))) {
                vpi_printf(" %s", vpi_get_str(vpiName, m));
                vpi_printf("=%d", ival(m));
            }
        }
        vpi_printf("\n");
    }
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle q = vpi_handle_by_name((PLI_BYTE8*)"top.q", NULL);
    vpiHandle cs = vpi_handle_by_name((PLI_BYTE8*)"top.cs", NULL);
    vpiHandle m = vpi_handle_by_name((PLI_BYTE8*)"top.m", NULL);
    vpiHandle e, a, mx;
    (void)u;
    elems(q);
    elems(cs);
    elems(m);
    /* write a member of q[1] and an element of m["x"] */
    e = vpi_handle_by_index(q, 1);
    a = vpi_scan(vpi_iterate(vpiMember, e));
    put(a, 70);
    mx = vpi_handle_by_index(vpi_scan(vpi_iterate(vpiElement, m)), 1);
    put(mx, 20);
    vpi_printf("q[1].a = %d, m[\"x\"][1] = %d\n", ival(a), ival(mx));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$nested", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
