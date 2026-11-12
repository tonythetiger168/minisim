/* IEEE 1800-2023 38.10 (tests/spec/38.10-vpi-assign-delay.sv): the delays of
   continuous assignments through vpi_get_delays / vpi_put_delays. */
#include <stdio.h>
#include "sv_vpi_user.h"

static void delays(vpiHandle p) {
    s_vpi_time v[3];
    s_vpi_delay d = {v, 3, vpiScaledRealTime, 0, 0, 0};
    vpi_get_delays(p, &d);
    vpi_printf(" delays %g %g %g", v[0].real, v[1].real, v[2].real);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle it = vpi_iterate(vpiContAssign, vpi_handle_by_name((PLI_BYTE8*)"top", NULL)), a, first = 0;
    s_vpi_error_info e;
    (void)u;
    while ((a = vpi_scan(it))) {
        vpiHandle l = vpi_handle(vpiLhs, a);
        vpi_printf("assign %s:", vpi_get_str(vpiName, l));
        if (vpi_handle(vpiDelay, a)) {
            if (!first) first = a;
            delays(a);
        } else {
            s_vpi_time v[1];
            s_vpi_delay d = {v, 1, vpiScaledRealTime, 0, 0, 0};
            int err;
            vpi_get_delays(a, &d);
            err = vpi_chk_error(&e) != 0;
            vpi_printf(" no delay, error %d", err);
        }
        vpi_printf("\n");
    }
    {
        s_vpi_time v[1] = {{vpiScaledRealTime, 0, 0, 4.0}};
        s_vpi_delay d = {v, 1, vpiScaledRealTime, 0, 0, 0};
        vpi_put_delays(first, &d);
        vpi_printf("now:");
        delays(first);
        vpi_printf("\n");
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$assigns", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
