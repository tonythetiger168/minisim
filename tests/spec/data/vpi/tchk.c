/* IEEE 1800-2023 37 (timing checks), 38.10, 38.36.1
   (tests/spec/38.36-vpi-tchk.sv): timing check objects, their terms and
   limits, and cbTchkViolation. */
#include <stdio.h>
#include "sv_vpi_user.h"

static PLI_INT32 onViol(p_cb_data d) {
    vpi_printf("  cbTchkViolation at %u: type %d line %d\n", d->time->low, vpi_get(vpiTchkType, d->obj), vpi_get(vpiLineNo, d->obj));
    return 0;
}
static void limits(vpiHandle t) {
    s_vpi_time v[2];
    s_vpi_delay d = {v, 2, vpiScaledRealTime, 0, 0, 0};
    vpi_get_delays(t, &d);
    vpi_printf(" limits %g %g", v[0].real, v[1].real);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle m = vpi_handle_by_name((PLI_BYTE8*)"top.ff", NULL), it, t, setup = 0;
    (void)u;
    it = vpi_iterate(vpiTchk, m);
    while ((t = vpi_scan(it))) {
        vpiHandle r = vpi_handle(vpiTchkRefTerm, t), dt = vpi_handle(vpiTchkDataTerm, t), n = vpi_handle(vpiTchkNotifier, t);
        vpi_printf("%s type %d line %d:", vpi_get_str(vpiType, t), vpi_get(vpiTchkType, t), vpi_get(vpiLineNo, t));
        vpiHandle e = vpi_handle(vpiExpr, r);
        vpi_printf(" ref %s edge %d", vpi_get_str(vpiName, r), vpi_get(vpiEdge, r));
        vpi_printf(" (%s)", e ? vpi_get_str(vpiFullName, e) : "-");
        if (dt) vpi_printf(" data %s edge %d", vpi_get_str(vpiName, dt), vpi_get(vpiEdge, dt));
        if (n) vpi_printf(" notifier %s", vpi_get_str(vpiFullName, n));
        limits(t);
        vpi_printf("\n");
        if (vpi_get(vpiTchkType, t) == vpiSetup) setup = t;
    }
    {   /* every violation, and the setup check again */
        s_vpi_time tm = {vpiSimTime};
        s_cb_data c = {cbTchkViolation, onViol, NULL, &tm, NULL, 0, NULL};
        vpi_register_cb(&c);
    }
    {   /* widen the setup limit to 4 */
        s_vpi_time v[1] = {{vpiScaledRealTime, 0, 0, 4.0}};
        s_vpi_delay d = {v, 1, vpiScaledRealTime, 0, 0, 0};
        vpi_put_delays(setup, &d);
        vpi_printf("setup now:");
        limits(setup);
        vpi_printf("\n");
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$tchks", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
