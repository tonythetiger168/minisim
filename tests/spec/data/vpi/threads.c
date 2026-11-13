/* IEEE 1800-2023 38.36.1 (tests/spec/38.36-vpi-threads.sv): thread and frame
   callbacks, and thread and frame objects. */
#include <stdio.h>
#include "sv_vpi_user.h"

static vpiHandle mainThr, thr[8];
static int nthr;
static const char* label(vpiHandle t) {
    static char b[4][8];
    static int k;
    int i;
    if (!t) return "-";
    if (vpi_compare_objects(t, mainThr)) return "main";
    for (i = 0; i < nthr; i++)
        if (vpi_compare_objects(t, thr[i])) {
            k = (k + 1) % 4;
            sprintf(b[k], "T%d", i + 1);
            return b[k];
        }
    return "?";
}
static int now(p_cb_data d) { return d->time ? (int)d->time->low : -1; }
static PLI_INT32 started(p_cb_data d) {
    thr[nthr++] = d->obj;
    vpi_printf("start: thread %s forked by %s at %d\n", label(d->obj), label(vpi_handle(vpiParent, d->obj)), now(d));
    return 0;
}
static PLI_INT32 ended(p_cb_data d) {
    vpi_printf("end: %s at %d\n", label(d->obj), now(d));
    return 0;
}
static PLI_INT32 entered(p_cb_data d) {
    vpi_printf("enter: %s at %d\n", label(d->obj), now(d));
    return 0;
}
static PLI_INT32 frameStart(p_cb_data d) {
    vpiHandle caller = vpi_handle(vpiParent, d->obj);
    vpi_printf("frame start: %s", vpi_get_str(vpiName, vpi_handle(vpiScope, d->obj)));
    vpi_printf(" (automatic %d) in %s", vpi_get(vpiAutomatic, d->obj), label(vpi_handle(vpiThread, d->obj)));
    vpi_printf(", caller %s at %d\n", caller ? vpi_get_str(vpiName, vpi_handle(vpiScope, caller)) : "-", now(d));
    return 0;
}
static PLI_INT32 frameEnd(p_cb_data d) {
    vpi_printf("frame end: %s", vpi_get_str(vpiName, vpi_handle(vpiScope, d->obj)));
    vpi_printf(" in %s at %d\n", label(vpi_handle(vpiThread, d->obj)), now(d));
    return 0;
}
static void reg(PLI_INT32 reason, vpiHandle obj, PLI_INT32 (*f)(p_cb_data)) {
    static s_vpi_time t = {vpiSimTime, 0, 0, 0};
    s_cb_data d = {reason, f, obj, &t, NULL, 0, NULL};
    if (!vpi_register_cb(&d)) vpi_printf("reason %d: not registered\n", reason);
}
static PLI_INT32 watch(PLI_BYTE8* u) {
    (void)u;
    mainThr = vpi_handle(vpiThread, NULL);
    vpi_printf("watch: %s thread, frame %d\n", label(mainThr), vpi_handle(vpiFrame, NULL) != NULL);
    reg(cbStartOfThread, mainThr, started);
    reg(cbEndOfThread, NULL, ended);
    reg(cbEnterThread, NULL, entered);
    reg(cbStartOfFrame, NULL, frameStart);
    reg(cbEndOfFrame, NULL, frameEnd);
    return 0;
}
static PLI_INT32 where(PLI_BYTE8* u) {
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL), args = vpi_iterate(vpiArgument, call);
    vpiHandle a = vpi_scan(args), fr = vpi_handle(vpiFrame, NULL), t = vpi_handle(vpiThread, NULL);
    vpiHandle it, k;
    s_vpi_value v = {vpiIntVal};
    int kids = 0;
    (void)u;
    vpi_free_object(args);
    vpi_get_value(a, &v);
    if ((it = vpi_iterate(vpiThread, mainThr)))
        while ((k = vpi_scan(it))) kids++;
    vpi_printf("where %d: frame %s", (int)v.value.integer, vpi_get_str(vpiName, vpi_handle(vpiScope, fr)));
    vpi_printf(", same thread %d, parent %s, main has %d children\n", vpi_compare_objects(t, vpi_handle(vpiThread, fr)),
               label(vpi_handle(vpiParent, t)), kids);
    return 0;
}
static PLI_INT32 check(PLI_BYTE8* u) {
    (void)u;
    vpi_printf("check: T1 valid %d, main valid %d\n", vpi_get(vpiValid, thr[0]), vpi_get(vpiValid, mainThr));
    return 0;
}
static void systf(const char* name, PLI_INT32 (*f)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, f, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    systf("$watch", watch);
    systf("$where", where);
    systf("$check", check);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
