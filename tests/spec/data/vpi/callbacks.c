/* Callbacks (IEEE 1800-2023 38.36): cbValueChange on a reg, a net and a
 * memory word, with the time and value it reports; cbAfterDelay that
 * writes a reg with vpi_put_value; cbReadOnlySynch at the end of a time
 * slot; cbNextSimTime; cbStartOfSimulation / cbEndOfSimulation; and
 * vpi_remove_cb. */
#include <stdio.h>
#include <string.h>
#include "vpi_user.h"

static vpiHandle clk_cb;
static int clk_changes;

static PLI_INT32 next_time(p_cb_data d) {
    vpi_printf("%u: next simulation time\n", d->time->low);
    return 0;
}

static PLI_INT32 changed(p_cb_data d) {
    vpi_printf("%u: %s -> %s\n", d->time->low, vpi_get_str(vpiFullName, d->obj), d->value->value.str);
    if (strcmp((char *)d->user_data, "mem") == 0) {   /* once the time advances */
        static s_vpi_time t = {vpiSimTime};
        s_cb_data n;
        memset(&n, 0, sizeof n);
        n.reason = cbNextSimTime;
        n.cb_rtn = next_time;
        n.time = &t;
        vpi_register_cb(&n);
    }
    if (d->obj && strcmp((char *)d->user_data, "clk") == 0 && ++clk_changes == 4) {
        vpi_remove_cb(clk_cb);
        vpi_printf("removed the clk callback\n");
    }
    return 0;
}

static PLI_INT32 ro(p_cb_data d) {
    s_vpi_time t = {vpiSimTime};
    vpi_get_time(NULL, &t);
    s_vpi_value v = {vpiDecStrVal};
    vpi_get_value(d->obj, &v);
    vpi_printf("%u: read-only synch: count=%s\n", t.low, v.value.str);
    return 0;
}

static PLI_INT32 later(p_cb_data d) {
    s_vpi_time t = {vpiSimTime};
    vpi_get_time(NULL, &t);
    vpi_printf("%u: after delay: writing 9\n", t.low);
    s_vpi_value v = {vpiIntVal};
    v.value.integer = 9;
    vpi_put_value(d->obj, &v, NULL, vpiNoDelay);
    s_cb_data r;
    memset(&r, 0, sizeof r);
    r.reason = cbReadOnlySynch;
    r.cb_rtn = ro;
    r.obj = d->obj;
    s_vpi_time z = {vpiSimTime, 0, 0, 0};
    r.time = &z;
    vpi_register_cb(&r);
    return 0;
}

static void watch(const char *name, const char *tag) {
    static s_vpi_time t = {vpiSimTime};
    static s_vpi_value v = {vpiBinStrVal};
    s_cb_data d;
    memset(&d, 0, sizeof d);
    d.reason = cbValueChange;
    d.cb_rtn = changed;
    d.obj = vpi_handle_by_name((PLI_BYTE8 *)name, NULL);
    d.time = &t;
    d.value = &v;
    d.user_data = (PLI_BYTE8 *)tag;
    vpiHandle h = vpi_register_cb(&d);
    if (strcmp(tag, "clk") == 0) clk_cb = h;
}

static PLI_INT32 start(p_cb_data d) {
    (void)d;
    vpi_printf("start of simulation\n");
    watch("top.clk", "clk");
    watch("top.bus", "bus");
    watch("top.mem[1]", "mem");
    s_cb_data a;
    memset(&a, 0, sizeof a);
    s_vpi_time t = {vpiSimTime, 0, 17, 0};
    a.reason = cbAfterDelay;
    a.cb_rtn = later;
    a.obj = vpi_handle_by_name("top.count", NULL);
    a.time = &t;
    vpi_register_cb(&a);
    return 0;
}

static PLI_INT32 end(p_cb_data d) {
    (void)d;
    s_vpi_time t = {vpiSimTime};
    vpi_get_time(NULL, &t);
    vpi_printf("%u: end of simulation\n", t.low);
    return 0;
}

static void boot(void) {
    s_cb_data d;
    memset(&d, 0, sizeof d);
    d.reason = cbStartOfSimulation;
    d.cb_rtn = start;
    vpi_register_cb(&d);
    d.reason = cbEndOfSimulation;
    d.cb_rtn = end;
    vpi_register_cb(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
