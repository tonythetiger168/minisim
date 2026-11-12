/* IEEE 1800-2023 37.29 (tests/spec/37-vpi-classes.sv): class definitions,
   their methods, extends and derived classes, and the dynamic type of a
   class object. */
#include <stdio.h>
#include "sv_vpi_user.h"

static const char* s(PLI_INT32 p, vpiHandle h) {
    static char buf[4][128];
    static int k;
    const char* r = h ? vpi_get_str(p, h) : 0;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "%s", r ? r : "-");
    return buf[k];
}
static void defn(vpiHandle d) {
    vpiHandle it, m, e;
    vpi_printf("%s %s (%s) virtual %d line %d\n", s(vpiType, d), s(vpiName, d), s(vpiFullName, d),
               vpi_get(vpiVirtual, d), vpi_get(vpiLineNo, d));
    e = vpi_handle(vpiExtends, d);
    if (e) vpi_printf("  %s %s -> %s\n", s(vpiType, e), s(vpiName, vpi_handle(vpiClassTypespec, e)),
                      s(vpiName, vpi_handle(vpiClassDefn, e)));
    it = vpi_iterate(vpiMethods, d);
    while (it && (m = vpi_scan(it)))
        vpi_printf("  method %s %s virtual %d of %s\n", s(vpiType, m), s(vpiName, m), vpi_get(vpiVirtual, m),
                   s(vpiName, vpi_handle(vpiClassDefn, m)));
    it = vpi_iterate(vpiDerivedClasses, d);
    while (it && (m = vpi_scan(it))) vpi_printf("  derived %s\n", s(vpiName, m));
    it = vpi_iterate(vpiVariables, d);
    while (it && (m = vpi_scan(it))) vpi_printf("  static %s\n", s(vpiName, m));
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle v = vpi_handle_by_name((PLI_BYTE8*)"top.h", NULL), o, it, p;
    (void)u;
    vpi_printf("variable %s: class %s\n", s(vpiType, v), s(vpiName, vpi_handle(vpiClassDefn, v)));
    o = vpi_handle(vpiClassObj, v);
    vpi_printf("object: class %s\n", s(vpiName, vpi_handle(vpiClassDefn, o)));
    it = vpi_iterate(vpiVariables, o);
    while (it && (p = vpi_scan(it))) {
        s_vpi_value val = {vpiIntVal};
        vpi_get_value(p, &val);
        vpi_printf("  %s = %d\n", s(vpiName, p), (int)val.value.integer);
    }
    vpi_printf("typespec: class %s\n", s(vpiName, vpi_handle(vpiClassDefn, vpi_handle(vpiTypespec, v))));
    defn(vpi_handle(vpiClassDefn, v));
    defn(vpi_handle(vpiClassDefn, o));
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$classes", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
