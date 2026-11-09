/* Library binding through VPI (IEEE 1800-2023 33.7): vpiLibrary, vpiCell
 * and vpiConfig of every module instance, walked from the top. */
#include <stdio.h>
#include <string.h>
#include "vpi_user.h"

static void walk(vpiHandle m) {
    /* vpi_get_str's string lasts until the next call: print one at a time */
    vpi_printf("%s:", vpi_get_str(vpiFullName, m));
    vpi_printf(" library %s,", vpi_get_str(vpiLibrary, m));
    vpi_printf(" cell %s,", vpi_get_str(vpiCell, m));
    vpi_printf(" config \"%s\"\n", vpi_get_str(vpiConfig, m));
    vpiHandle it = vpi_iterate(vpiModule, m);
    if (!it) return;
    vpiHandle c;
    while ((c = vpi_scan(it))) walk(c);
}

static PLI_INT32 start(p_cb_data d) {
    (void)d;
    vpiHandle it = vpi_iterate(vpiModule, NULL);
    vpiHandle m;
    while ((m = vpi_scan(it))) walk(m);
    return 0;
}

static void boot(void) {
    s_cb_data d;
    memset(&d, 0, sizeof d);
    d.reason = cbStartOfSimulation;
    d.cb_rtn = start;
    vpi_register_cb(&d);
}

void (*vlog_startup_routines[])(void) = {boot, 0};
