/* IEEE 1800-2023 38.10, 38.30 (tests/spec/38.10-vpi-delays.sv). */
#include <stdio.h>
#include "vpi_user.h"

static PLI_INT32 delays(PLI_BYTE8* u) {
    (void)u;
    vpiHandle w = vpi_handle_by_name("top.w", NULL);
    s_vpi_time t[9];
    s_vpi_delay d = {t, 3, vpiScaledRealTime, 0, 0, 0};
    vpi_get_delays(w, &d);
    vpi_printf("w: rise %g fall %g turn-off %g ns\n", t[0].real, t[1].real, t[2].real);
    d.time_type = vpiSimTime;
    d.mtm_flag = 1;
    vpi_get_delays(w, &d);
    vpi_printf("w: ticks %u:%u:%u %u %u\n", t[0].low, t[1].low, t[2].low, t[3].low, t[6].low);
    s_vpi_delay bad = {t, 1, vpiSimTime, 0, 0, 0};
    vpi_get_delays(vpi_handle_by_name("top.d", NULL), &bad);
    s_vpi_error_info e;
    vpi_printf("on a variable: error %d\n", vpi_chk_error(&e) ? e.level : 0);
    return 0;
}
static PLI_INT32 putdelays(PLI_BYTE8* u) {
    (void)u;
    vpiHandle w = vpi_handle_by_name("top.w", NULL);
    s_vpi_time t[2] = {{vpiScaledRealTime, 0, 0, 1.0}, {vpiScaledRealTime, 0, 0, 2.5}};
    s_vpi_delay d = {t, 2, vpiScaledRealTime, 0, 1, 0};   /* append: rise 3, fall 5.5 */
    vpi_put_delays(w, &d);
    return 0;
}
static void reg(const char* name, PLI_INT32 (*fn)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, fn, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    reg("$delays", delays);
    reg("$putdelays", putdelays);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
