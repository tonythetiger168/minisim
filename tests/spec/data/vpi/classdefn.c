/* IEEE 1800-2023 37.31 (tests/spec/37.31-vpi-class-defn.sv): the
   declarations of a class defn. */
#include <stdio.h>
#include "sv_vpi_user.h"

static void names(vpiHandle d, PLI_INT32 type, const char* what) {
    vpiHandle it = vpi_iterate(type, d), m;
    while (it && (m = vpi_scan(it))) {
        vpi_printf("  %s %s", what, vpi_get_str(vpiName, m));
        if (type == vpiVariables)
            vpi_printf(" type %d automatic %d", vpi_get(vpiType, m), vpi_get(vpiAutomatic, m));
        else if (type == vpiNamedEvent)
            vpi_printf(" type %d", vpi_get(vpiType, m));
        else if (type == vpiParameter) {
            s_vpi_value v = {vpiIntVal, {0}};
            vpi_get_value(m, &v);
            vpi_printf(" = %d local %d", v.value.integer, vpi_get(vpiLocalParam, m));
        }
        vpi_printf("\n");
    }
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle d = vpi_handle(vpiClassDefn, vpi_handle_by_name((PLI_BYTE8*)"top.f", NULL)), v;
    s_vpi_value val = {vpiIntVal, {0}};
    s_vpi_error_info e;
    (void)u;
    vpi_printf("class %s", vpi_get_str(vpiName, d));
    vpi_printf(" automatic %d\n", vpi_get(vpiAutomatic, d));
    names(d, vpiVariables, "variable");
    names(d, vpiNamedEvent, "event");
    names(d, vpiParameter, "parameter");
    names(d, vpiTypedef, "typedef");
    names(d, vpiInternalScope, "scope");
    v = vpi_scan(vpi_iterate(vpiVariables, d));
    vpi_printf("  parent %s\n", vpi_get_str(vpiName, vpi_handle(vpiParent, v)));
    vpi_get_value(v, &val);
    vpi_printf("  value refused: %d\n", vpi_chk_error(&e) != 0);
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$defn", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
