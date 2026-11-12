/* IEEE 1800-2023 38.36.1 (tests/spec/38.36-vpi-sv-callbacks.sv): cbCreateObj
   and cbSizeChange. */
#include <stdio.h>
#include "sv_vpi_user.h"

static PLI_INT32 created(p_cb_data d) {
    vpiHandle c = vpi_handle(vpiClassDefn, d->obj);
    vpi_printf("%s: created %s", d->user_data, vpi_get_str(vpiType, d->obj));
    vpi_printf(" (%s)", c ? vpi_get_str(vpiName, c) : "-");
    vpi_printf(" at %d\n", (int)d->time->low);
    return 0;
}
static PLI_INT32 resized(p_cb_data d) {
    vpi_printf("%s: size of %s now %d at %d\n", d->user_data, vpi_get_str(vpiName, d->obj), vpi_get(vpiSize, d->obj),
               (int)d->time->low);
    return 0;
}
static void reg(PLI_INT32 reason, vpiHandle obj, PLI_INT32 (*f)(p_cb_data), const char* tag) {
    static s_vpi_time t = {vpiSimTime, 0, 0, 0};
    s_cb_data d = {reason, f, obj, &t, NULL, 0, (PLI_BYTE8*)tag};
    if (!vpi_register_cb(&d)) vpi_printf("%s: not registered\n", tag);
}
static PLI_INT32 start(p_cb_data u) {
    vpiHandle q = vpi_handle_by_name((PLI_BYTE8*)"top.q", NULL);
    vpiHandle aa = vpi_handle_by_name((PLI_BYTE8*)"top.aa", NULL);
    vpiHandle p = vpi_handle_by_name((PLI_BYTE8*)"top.p", NULL);
    s_vpi_error_info e;
    (void)u;
    reg(cbCreateObj, NULL, created, "any");
    reg(cbCreateObj, vpi_handle(vpiClassDefn, p), created, "Pkt");
    reg(cbSizeChange, q, resized, "q");
    reg(cbSizeChange, aa, resized, "aa");
    reg(cbStartOfSave, NULL, resized, "save");
    {
        static s_vpi_time t = {vpiSimTime, 0, 0, 0};
        s_cb_data d = {cbSizeChange, resized, p, &t, NULL, 0, (PLI_BYTE8*)"bad"};
        vpiHandle h = vpi_register_cb(&d);
        int err = vpi_chk_error(&e) != 0;
        vpi_printf("bad: handle %d error %d\n", h != NULL, err);
    }
    return 0;
}
static void boot(void) {
    s_cb_data d = {cbStartOfSimulation, start, NULL, NULL, NULL, 0, NULL};
    vpi_register_cb(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
