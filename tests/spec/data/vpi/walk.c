/* Walking the design (IEEE 1800-2023 37.10, 37.12-37.17, 38.6, 38.21):
 * $walk prints every module from the top, with its nets, regs, integer
 * and real variables, memories, parameters and ports. */
#include <stdio.h>
#include <string.h>
#include "vpi_user.h"

static void indent(int d) { while (d-- > 0) vpi_printf("  "); }

static const char *dir_name(int d) {
    return d == vpiInput ? "input" : d == vpiOutput ? "output" : d == vpiInout ? "inout" : "?";
}

static void objects(vpiHandle mod, int type, const char *what, int d) {
    vpiHandle it = vpi_iterate(type, mod), o;
    while (it && (o = vpi_scan(it))) {
        indent(d);
        vpi_printf("%s %s size %d", what, vpi_get_str(vpiName, o), (int)vpi_get(vpiSize, o));
        if (type == vpiMemory) {
            vpiHandle w = vpi_handle_by_index(o, 2);
            s_vpi_value v = {vpiHexStrVal};
            vpi_get_value(w, &v);
            vpi_printf(" [2]=%s (%s)", v.value.str, vpi_get_str(vpiFullName, w));
        } else if (type == vpiParameter) {
            s_vpi_value v = {vpiDecStrVal};
            vpi_get_value(o, &v);
            vpi_printf(" = %s", v.value.str);
        } else if (type != vpiPort) {
            s_vpi_value v = {vpiBinStrVal};
            vpi_get_value(o, &v);
            vpi_printf(" = %s", v.value.str);
        }
        if (type == vpiPort) vpi_printf(" %s", dir_name((int)vpi_get(vpiDirection, o)));
        vpi_printf("\n");
    }
}

static void walk(vpiHandle mod, int d) {
    indent(d);
    /* One string per call: vpi_get_str reuses its buffer (38.21). */
    vpi_printf("module %s", vpi_get_str(vpiFullName, mod));
    vpi_printf(" (%s)", vpi_get_str(vpiDefName, mod));
    vpi_printf(" %s top=%d\n", vpi_get_str(vpiName, mod), (int)vpi_get(vpiTopModule, mod));
    objects(mod, vpiPort, "port", d + 1);
    objects(mod, vpiNet, "net", d + 1);
    objects(mod, vpiReg, "reg", d + 1);
    objects(mod, vpiIntegerVar, "integer", d + 1);
    objects(mod, vpiMemory, "memory", d + 1);
    objects(mod, vpiParameter, "parameter", d + 1);
    vpiHandle it = vpi_iterate(vpiModule, mod), m;
    while (it && (m = vpi_scan(it))) {
        vpiHandle up = vpi_handle(vpiModule, m);
        if (!vpi_compare_objects(up, mod)) vpi_printf("parent mismatch\n");
        walk(m, d + 1);
    }
}

static PLI_INT32 walk_all(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle it = vpi_iterate(vpiModule, NULL), m;
    while (it && (m = vpi_scan(it))) walk(m, 0);
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$walk";
    d.calltf = walk_all;
    vpi_register_systf(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
