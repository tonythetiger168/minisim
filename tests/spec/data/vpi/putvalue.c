/* Writing values and time (IEEE 1800-2023 38.34, 38.35, 38.15, 38.4,
 * 38.5): vpi_put_value with no delay, with a transport delay, and to one
 * bit of a vector; names relative to a scope; scaled time in a module's
 * time unit; vpi_control(vpiFinish). */
#include <stdio.h>
#include <string.h>
#include "vpi_user.h"

static vpiHandle h(const char *name, vpiHandle scope) { return vpi_handle_by_name((PLI_BYTE8 *)name, scope); }

static PLI_INT32 drive(PLI_BYTE8 *ud) {
    (void)ud;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle scope = vpi_handle(vpiScope, call);
    vpi_printf("scope %s, unit %d, precision %d\n", vpi_get_str(vpiFullName, scope),
               (int)vpi_get(vpiTimeUnit, scope), (int)vpi_get(vpiTimePrecision, scope));
    s_vpi_time now = {vpiScaledRealTime};
    vpi_get_time(scope, &now);
    s_vpi_time ticks = {vpiSimTime};
    vpi_get_time(NULL, &ticks);
    vpi_printf("now %.1f units, %u ticks\n", now.real, ticks.low);

    s_vpi_value v = {vpiHexStrVal};
    v.value.str = "a5";
    vpi_put_value(h("data", scope), &v, NULL, vpiNoDelay);

    s_vpi_value one = {vpiScalarVal};
    one.value.scalar = vpi1;
    vpi_put_value(vpi_handle_by_index(h("flags", scope), 2), &one, NULL, vpiNoDelay);

    s_vpi_value later = {vpiIntVal};
    later.value.integer = 77;
    s_vpi_time d = {vpiScaledRealTime, 0, 0, 2.5};   /* 2.5 time units */
    vpi_put_value(h("top.u.slow", NULL), &later, &d, vpiTransportDelay);

    return 0;
}

static PLI_INT32 stop(PLI_BYTE8 *ud) {
    (void)ud;
    vpi_printf("stopping\n");
    vpi_control(vpiFinish, 0);
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$drive";
    d.calltf = drive;
    vpi_register_systf(&d);
    d.tfname = "$stop_here";
    d.calltf = stop;
    vpi_register_systf(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
