/* IEEE 1800-2023 37 (module paths), 38.10 (tests/spec/38.10-vpi-modpath.sv):
   module path objects, their terms and their delays. */
#include <stdio.h>
#include "sv_vpi_user.h"

static void delays(vpiHandle p) {
    s_vpi_time v[3];
    s_vpi_delay d = {v, 3, vpiScaledRealTime, 0, 0, 0};
    vpi_get_delays(p, &d);
    vpi_printf(" delays %g %g %g", v[0].real, v[1].real, v[2].real);
}
static PLI_INT32 run(PLI_BYTE8* u) {
    vpiHandle it = vpi_iterate(vpiModPath, vpi_handle_by_name((PLI_BYTE8*)"top.b", NULL)), p, t, first = 0;
    (void)u;
    while ((p = vpi_scan(it))) {
        vpiHandle ti = vpi_iterate(vpiModPathIn, p);
        if (!first) first = p;
        vpi_printf("%s ifnone %d:", vpi_get_str(vpiType, p), vpi_get(vpiModPathHasIfNone, p));
        while ((t = vpi_scan(ti))) vpi_printf(" in %s edge %d", vpi_get_str(vpiName, t), vpi_get(vpiEdge, t));
        t = vpi_scan(vpi_iterate(vpiModPathOut, p));
        vpi_printf(" out %s", vpi_get_str(vpiName, t));
        vpi_printf(" (%s)", vpi_get_str(vpiFullName, vpi_handle(vpiExpr, t)));
        delays(p);
        vpi_printf("\n");
        vpi_printf("  type %d polarity %d", vpi_get(vpiPathType, p), vpi_get(vpiPolarity, p));
        if ((t = vpi_handle(vpiCondition, p))) vpi_printf(" cond %s", vpi_get_str(vpiDecompile, t));
        ti = vpi_iterate(vpiModDataPathIn, p);
        if (ti && (t = vpi_scan(ti))) {
            vpi_printf(" data %s", vpi_get_str(vpiName, t));
            vpi_printf(" polarity %d", vpi_get(vpiDataPolarity, p));
            vpi_scan(ti);
        }
        vpi_printf("\n");
    }
    {
        s_vpi_time v[2] = {{vpiScaledRealTime, 0, 0, 5.0}, {vpiScaledRealTime, 0, 0, 6.0}};
        s_vpi_delay d = {v, 2, vpiScaledRealTime, 0, 0, 0};
        vpi_put_delays(first, &d);
        vpi_printf("a => y now:");
        delays(first);
        vpi_printf("\n");
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$paths", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
