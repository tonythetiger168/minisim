/* IEEE 1800-2023 37 (primitive objects), 38.10 (tests/spec/38.10-vpi-primitives.sv):
   gates, switches and UDPs, their terminals and their delays. */
#include <stdio.h>
#include "sv_vpi_user.h"

static void delays(vpiHandle p) {
    s_vpi_time v[3];
    s_vpi_delay d = {v, 3, vpiScaledRealTime, 0, 0, 0};
    vpi_get_delays(p, &d);
    vpi_printf(" delays %g %g %g", v[0].real, v[1].real, v[2].real);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8*)"top", NULL);
    vpiHandle it = vpi_iterate(vpiPrimitive, top), p, t, g = 0;
    (void)u;
    while ((p = vpi_scan(it))) {
        vpiHandle ti = vpi_iterate(vpiPrimTerm, p);
        if (!g) g = p;
        vpi_printf("%s", vpi_get_str(vpiType, p));
        vpi_printf(" %s", vpi_get_str(vpiFullName, p));
        vpi_printf(" def %s", vpi_get_str(vpiDefName, p));
        vpi_printf(" prim %d size %d:", vpi_get(vpiPrimType, p), vpi_get(vpiSize, p));
        while ((t = vpi_scan(ti))) {
            vpi_printf(" [%d dir %d %s]", vpi_get(vpiTermIndex, t), vpi_get(vpiDirection, t),
                       vpi_get_str(vpiName, vpi_handle(vpiExpr, t)));
        }
        if (vpi_get(vpiType, p) != vpiUdp) delays(p);
        vpi_printf("\n");
    }
    it = vpi_iterate(vpiSwitch, top);
    vpi_printf("switches:");
    while ((p = vpi_scan(it))) vpi_printf(" %s", vpi_get_str(vpiName, p));
    vpi_printf("\n");
    {
        s_vpi_time v[2] = {{vpiScaledRealTime, 0, 0, 5.0}, {vpiScaledRealTime, 0, 0, 7.0}};
        s_vpi_delay d = {v, 2, vpiScaledRealTime, 0, 0, 0};
        vpi_put_delays(g, &d);
        vpi_printf("%s now:", vpi_get_str(vpiName, g));
        delays(g);
        vpi_printf("\n");
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$prims", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
