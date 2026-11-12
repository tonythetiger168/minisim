/* The SystemVerilog object model (IEEE 1800-2023 37.3, 37.5, 37.10 to
 * 37.25): $sv_walk prints the packages and the design from the top, with
 * the SystemVerilog types, typespecs, scopes, ranges and connections. */
#include <stdio.h>
#include <string.h>
#include "sv_vpi_user.h"

static void indent(int d) { while (d-- > 0) vpi_printf("  "); }

static void value(vpiHandle o, int format) {
    s_vpi_value v;
    v.format = format;
    vpi_get_value(o, &v);
    if (format == vpiIntVal) vpi_printf("%d", (int)v.value.integer);
    else if (format == vpiRealVal) vpi_printf("%g", v.value.real);
    else vpi_printf("%s", v.value.str);
}

static void ranges(vpiHandle o) {
    vpiHandle it = vpi_iterate(vpiRange, o), r;
    while (it && (r = vpi_scan(it))) {
        vpi_printf("[");
        value(vpi_handle(vpiLeftRange, r), vpiIntVal);
        vpi_printf(":");
        value(vpi_handle(vpiRightRange, r), vpiIntVal);
        vpi_printf("]");
    }
}

static void typespec(vpiHandle ts, int d) {
    indent(d);
    /* One string per call: vpi_get_str reuses its buffer (38.11). */
    vpi_printf("typespec %s", vpi_get_str(vpiType, ts));
    const char *name = vpi_get_str(vpiName, ts);
    if (name && *name) vpi_printf(" %s", name);
    vpi_printf(" size %d", (int)vpi_get(vpiSize, ts));
    ranges(ts);
    vpi_printf("\n");
    vpiHandle it = vpi_iterate(vpiEnumConst, ts), e;
    while (it && (e = vpi_scan(it))) {
        indent(d + 1);
        vpi_printf("%s = ", vpi_get_str(vpiName, e));
        value(e, vpiDecStrVal);
        vpi_printf("\n");
    }
    it = vpi_iterate(vpiTypespecMember, ts);
    while (it && (e = vpi_scan(it))) {
        indent(d + 1);
        vpi_printf("member %s: ", vpi_get_str(vpiName, e));
        vpi_printf("%s\n", vpi_get_str(vpiType, vpi_handle(vpiTypespec, e)));
    }
}

static void variables(vpiHandle scope, int type, int d) {
    vpiHandle it = vpi_iterate(type, scope), o;
    while (it && (o = vpi_scan(it))) {
        indent(d);
        vpi_printf("%s", vpi_get_str(vpiType, o));
        vpi_printf(" %s", vpi_get_str(vpiName, o));
        ranges(o);
        vpi_printf(" size %d", (int)vpi_get(vpiSize, o));
        int t = (int)vpi_get(vpiType, o);
        if (vpi_get(vpiArray, o)) {
            vpi_printf(" array\n");
            continue;
        }
        vpi_printf(" = ");
        value(o, t == vpiStringVar ? vpiStringVal : t == vpiRealVar ? vpiRealVal : t == vpiIntVar || t == vpiShortIntVar ? vpiIntVal
                                                                               : vpiHexStrVal);
        vpiHandle ts = vpi_handle(vpiTypespec, o);
        vpi_printf(" (%s)\n", ts ? vpi_get_str(vpiType, ts) : "no typespec");
    }
}

static void scope(vpiHandle sc, int d) {
    indent(d);
    vpi_printf("%s", vpi_get_str(vpiType, sc));
    vpi_printf(" %s", vpi_get_str(vpiName, sc));
    vpi_printf(" (%s)\n", vpi_get_str(vpiFullName, sc));
    vpiHandle it, o;
    it = vpi_iterate(vpiParameter, sc);
    while (it && (o = vpi_scan(it))) {
        indent(d + 1);
        vpi_printf("parameter %s = ", vpi_get_str(vpiName, o));
        value(o, vpiDecStrVal);
        vpi_printf("\n");
    }
    it = vpi_iterate(vpiTypedef, sc);
    while (it && (o = vpi_scan(it))) typespec(o, d + 1);
    it = vpi_iterate(vpiPort, sc);
    while (it && (o = vpi_scan(it))) {
        vpiHandle hi = vpi_handle(vpiHighConn, o), lo = vpi_handle(vpiLowConn, o);
        indent(d + 1);
        vpi_printf("port %s", vpi_get_str(vpiName, o));
        vpi_printf(" low %s", lo ? vpi_get_str(vpiFullName, lo) : "-");
        vpi_printf(" high %s\n", hi ? vpi_get_str(vpiFullName, hi) : "-");
    }
    it = vpi_iterate(vpiModport, sc);
    while (it && (o = vpi_scan(it))) {
        indent(d + 1);
        vpi_printf("modport %s\n", vpi_get_str(vpiName, o));
    }
    variables(sc, vpiVariables, d + 1);
    variables(sc, vpiNet, d + 1);
    it = vpi_iterate(vpiGenScopeArray, sc);
    while (it && (o = vpi_scan(it))) {
        indent(d + 1);
        vpi_printf("vpiGenScopeArray %s size %d\n", vpi_get_str(vpiName, o), (int)vpi_get(vpiSize, o));
    }
    it = vpi_iterate(vpiInternalScope, sc);
    while (it && (o = vpi_scan(it))) {
        if (vpi_handle(vpiScope, o) != sc && vpi_get(vpiType, vpi_handle(vpiScope, o)) != vpiGenScopeArray)
            vpi_printf("scope mismatch\n");
        scope(o, d + 1);
    }
    it = vpi_iterate(vpiInstance, sc);
    while (it && (o = vpi_scan(it))) scope(o, d + 1);
}

static PLI_INT32 sv_walk(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle it = vpi_iterate(vpiInstance, NULL), o;
    while (it && (o = vpi_scan(it))) scope(o, 0);

    /* Elements of a two-dimensional array, by name and by indices. */
    vpiHandle m2 = vpi_handle_by_name("top.m2", NULL);
    PLI_INT32 ix[2] = {1, 2};
    vpiHandle e = vpi_handle_by_multi_index(m2, 2, ix);
    vpi_printf("%s", vpi_get_str(vpiFullName, e));
    vpi_printf(" is a %s = ", vpi_get_str(vpiType, e));
    value(e, vpiHexStrVal);
    vpi_printf(", same as by name: %d\n", e == vpi_handle_by_name("top.m2[1][2]", NULL));
    vpiHandle bit = vpi_handle_by_name("top.m2[1][2][3]", NULL);
    vpi_printf("%s = ", vpi_get_str(vpiFullName, bit));
    value(bit, vpiBinStrVal);
    vpi_printf("\n");
    int n = 0;
    it = vpi_iterate(vpiReg, m2);
    while (it && vpi_scan(it)) n++;
    vpi_printf("m2 has %d elements\n", n);

    /* Names through a package and a generate scope; a function's type. */
    vpiHandle c = vpi_handle_by_name("pk::counter", NULL);
    vpi_printf("%s in a ", vpi_get_str(vpiFullName, c));
    vpi_printf("%s = ", vpi_get_str(vpiType, vpi_handle(vpiInstance, c)));
    value(c, vpiIntVal);
    vpi_printf("\n");
    vpiHandle gv = vpi_handle_by_name("top.gen[1].gv", NULL);
    vpi_printf("%s in a ", vpi_get_str(vpiFullName, gv));
    vpi_printf("%s", vpi_get_str(vpiType, vpi_handle(vpiScope, gv)));
    vpi_printf(", module %s\n", vpi_get_str(vpiFullName, vpi_handle(vpiModule, gv)));
    vpiHandle f = vpi_handle_by_name("top.twice", NULL);
    vpi_printf("%s returns ", vpi_get_str(vpiName, f));
    vpi_printf("%s, automatic %d, vpiIntFunc %d\n", vpi_get_str(vpiType, vpi_handle(vpiTypespec, f)),
               (int)vpi_get(vpiAutomatic, f), vpi_get(vpiFuncType, f) == vpiIntFunc);

    /* A value written through a SystemVerilog variable. */
    vpiHandle col = vpi_handle_by_name("top.col", NULL);
    s_vpi_value v = {vpiIntVal};
    v.value.integer = 2;
    vpi_put_value(col, &v, NULL, vpiNoDelay);
    vpi_printf("col = ");
    value(col, vpiDecStrVal);
    vpi_printf("\n");
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$sv_walk";
    d.calltf = sv_walk;
    vpi_register_systf(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
