/* User system tasks and functions (IEEE 1800-2023 38.37, 38.35, 38.10,
 * 38.34): $show prints each argument's name, size and value; $sum is an
 * int function; $mask is a sized function (sizetf); $calls counts the
 * call sites compiletf saw; $poke writes its first argument. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vpi_user.h"

static int sites;

static PLI_INT32 show(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle it = vpi_iterate(vpiArgument, call), a;
    s_vpi_time t = {vpiSimTime, 0, 0, 0};
    vpi_get_time(NULL, &t);
    vpi_printf("%u %s line %d:", t.low, vpi_get_str(vpiName, call), (int)vpi_get(vpiLineNo, call));
    while (it && (a = vpi_scan(it))) {
        s_vpi_value v;
        int type = (int)vpi_get(vpiType, a);
        if (type == vpiConstant && vpi_get(vpiConstType, a) == vpiStringConst) {
            v.format = vpiStringVal;
            vpi_get_value(a, &v);
            vpi_printf(" \"%s\"", v.value.str);
            continue;
        }
        if (type == vpiModule) {
            vpi_printf(" module %s", vpi_get_str(vpiFullName, a));
            continue;
        }
        v.format = vpiBinStrVal;
        vpi_get_value(a, &v);
        char bin[256];
        snprintf(bin, sizeof bin, "%s", v.value.str);
        v.format = vpiHexStrVal;
        vpi_get_value(a, &v);
        char hex[256];
        snprintf(hex, sizeof hex, "%s", v.value.str);
        v.format = vpiDecStrVal;
        vpi_get_value(a, &v);
        if (type == vpiNet || type == vpiReg || type == vpiIntegerVar)
            vpi_printf(" %s(%d)=b%s,h%s,d%s", vpi_get_str(vpiName, a), (int)vpi_get(vpiSize, a), bin, hex, v.value.str);
        else
            vpi_printf(" expr(%d)=b%s,h%s,d%s", (int)vpi_get(vpiSize, a), bin, hex, v.value.str);
    }
    vpi_printf("\n");
    return 0;
}

static PLI_INT32 sum(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle it = vpi_iterate(vpiArgument, call), a;
    int total = 0;
    while (it && (a = vpi_scan(it))) {
        s_vpi_value v = {vpiIntVal};
        vpi_get_value(a, &v);
        total += v.value.integer;
    }
    s_vpi_value r = {vpiIntVal};
    r.value.integer = total;
    vpi_put_value(call, &r, NULL, vpiNoDelay);
    return 0;
}

static PLI_INT32 mask_size(PLI_BYTE8 *ud) { (void)ud; return 12; }
static PLI_INT32 mask(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    static s_vpi_vecval vec = {0xf0f, 0x0ff};   /* 1111 zzzz xxxx */
    s_vpi_value r = {vpiVectorVal};
    r.value.vector = &vec;
    vpi_put_value(call, &r, NULL, vpiNoDelay);
    return 0;
}

static PLI_INT32 count_site(PLI_BYTE8 *ud) { (void)ud; sites++; return 0; }
static PLI_INT32 calls(PLI_BYTE8 *ud) {
    (void)ud;
    s_vpi_value r = {vpiIntVal};
    r.value.integer = sites;
    vpi_put_value(vpi_handle(vpiSysTfCall, NULL), &r, NULL, vpiNoDelay);
    return 0;
}

static PLI_INT32 poke(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle it = vpi_iterate(vpiArgument, vpi_handle(vpiSysTfCall, NULL));
    vpiHandle dst = vpi_scan(it), val = vpi_scan(it);
    vpi_free_object(it);
    s_vpi_value v = {vpiVectorVal};
    vpi_get_value(val, &v);
    vpi_put_value(dst, &v, NULL, vpiNoDelay);
    return 0;
}

static PLI_INT32 half(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle it = vpi_iterate(vpiArgument, call);
    vpiHandle a = vpi_scan(it);
    vpi_free_object(it);
    s_vpi_value v = {vpiRealVal};
    vpi_get_value(a, &v);
    v.value.real /= 2;
    vpi_put_value(call, &v, NULL, vpiNoDelay);
    return 0;
}

static void reg(PLI_INT32 type, PLI_INT32 ftype, const char *name, PLI_INT32 (*calltf)(PLI_BYTE8 *),
                PLI_INT32 (*compiletf)(PLI_BYTE8 *), PLI_INT32 (*sizetf)(PLI_BYTE8 *)) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.sysfunctype = ftype;
    d.tfname = (PLI_BYTE8 *)name;
    d.calltf = calltf;
    d.compiletf = compiletf;
    d.sizetf = sizetf;
    vpi_register_systf(&d);
}

static void boot(void) {
    reg(vpiSysTask, 0, "$show", show, count_site, NULL);
    reg(vpiSysFunc, vpiIntFunc, "$sum", sum, count_site, NULL);
    reg(vpiSysFunc, vpiSizedFunc, "$mask", mask, NULL, mask_size);
    reg(vpiSysFunc, vpiIntFunc, "$calls", calls, NULL, NULL);
    reg(vpiSysTask, 0, "$poke", poke, NULL, NULL);
    reg(vpiSysFunc, vpiRealFunc, "$half", half, NULL, NULL);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
