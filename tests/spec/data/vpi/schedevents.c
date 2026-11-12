/* IEEE 1800-2023 38.35, 38.36 (tests/spec/38.35-vpi-sched-events.sv). */
#include <stdio.h>
#include "vpi_user.h"

static vpiHandle ev;

static void put(const char* name, int val, int delay, int flags, vpiHandle* evOut) {
    s_vpi_value v = {vpiIntVal};
    s_vpi_time t = {vpiSimTime, 0, (PLI_UINT32)delay, 0};
    v.value.integer = val;
    vpiHandle r = vpi_put_value(vpi_handle_by_name((PLI_BYTE8*)name, NULL), &v, &t, flags);
    if (evOut) *evOut = r;
}
static int now(void) {
    s_vpi_time t = {vpiSimTime};
    vpi_get_time(NULL, &t);
    return (int)t.low;
}
static int get(const char* name) {
    s_vpi_value v = {vpiIntVal};
    vpi_get_value(vpi_handle_by_name((PLI_BYTE8*)name, NULL), &v);
    return v.value.integer;
}
static PLI_INT32 sched(PLI_BYTE8* u) {
    (void)u;
    put("top.a", 1, 10, vpiInertialDelay | vpiReturnEvent, &ev);
    vpi_printf("event type %d scheduled %d\n", vpi_get(vpiType, ev), vpi_get(vpiScheduled, ev));
    put("top.b", 1, 10, vpiInertialDelay, NULL);
    put("top.b", 2, 12, vpiInertialDelay, NULL);        /* removes the write of 1 */
    put("top.c", 3, 8, vpiPureTransportDelay, NULL);
    put("top.c", 4, 15, vpiPureTransportDelay, NULL);
    put("top.c", 5, 11, vpiTransportDelay, NULL);       /* removes the write of 4 at 15 */
    return 0;
}
static PLI_INT32 cancel(PLI_BYTE8* u) {
    (void)u;
    vpi_put_value(ev, NULL, NULL, vpiCancelEvent);
    vpi_printf("@%d cancelled: scheduled %d\n", now(), vpi_get(vpiScheduled, ev));
    return 0;
}
static PLI_INT32 show(PLI_BYTE8* u) {
    (void)u;
    vpi_printf("@%d a %d b %d c %d\n", now(), get("top.a"), get("top.b"), get("top.c"));
    return 0;
}
static PLI_INT32 onHold(p_cb_data d) {
    const char* r = d->reason == cbForce ? "force" : d->reason == cbRelease ? "release" :
                    d->reason == cbAssign ? "assign" : "deassign";
    vpi_printf("@%d %s %s = %x (%s)\n", now(), r, vpi_get_str(vpiFullName, d->obj),
               d->value ? d->value->value.integer : -1, d->user_data ? d->user_data : "any");
    return 0;
}
static void onObj(int reason, const char* name, PLI_BYTE8* tag) {
    static s_vpi_time t = {vpiSuppressTime};
    static s_vpi_value v = {vpiIntVal};
    s_cb_data d = {reason, onHold, name ? vpi_handle_by_name((PLI_BYTE8*)name, NULL) : NULL, &t, &v, 0, tag};
    vpi_register_cb(&d);
}
static PLI_INT32 holds(PLI_BYTE8* u) {
    (void)u;
    onObj(cbForce, NULL, NULL);
    onObj(cbRelease, NULL, NULL);
    onObj(cbForce, "top.w", (PLI_BYTE8*)"w");
    onObj(cbAssign, "top.c", (PLI_BYTE8*)"c");
    onObj(cbDeassign, "top.c", (PLI_BYTE8*)"c");
    return 0;
}
static PLI_INT32 region(p_cb_data d) {
    vpi_printf("@%d %s: q %x\n", now(), d->reason == cbNBASynch ? "cbNBASynch" : "cbAtEndOfSimTime", get("top.q"));
    return 0;
}
static PLI_INT32 slot(PLI_BYTE8* u) {
    (void)u;
    static s_vpi_time t = {vpiSimTime, 0, 0, 0};
    s_cb_data a = {cbNBASynch, region, NULL, &t, NULL, 0, NULL};
    s_cb_data b = {cbAtEndOfSimTime, region, NULL, &t, NULL, 0, NULL};
    vpi_register_cb(&b);
    vpi_register_cb(&a);
    return 0;
}
static void reg(const char* name, PLI_INT32 (*fn)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, fn, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    reg("$sched", sched);
    reg("$cancel", cancel);
    reg("$show", show);
    reg("$holds", holds);
    reg("$slot", slot);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
