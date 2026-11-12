/* Compatibility modes (IEEE 1800-2023 36.12, annex L): this file is built
 * in the default mode; compat_old.c asks for IEEE 1364-2005. */
#include <string.h>
#include "sv_vpi_user.h"

void register_old(void);

static PLI_INT32 mode_new(PLI_BYTE8 *ud) {
    (void)ud;
    int m = (int)vpi_get(vpiCompatibilityMode, NULL);
    vpi_printf("default mode %d (%s)\n", m, m == vpiMode1800v2009 ? "vpiMode1800v2009" : "?");
    return 0;
}

static void boot(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$mode_new";
    d.calltf = mode_new;
    vpi_register_systf(&d);
    register_old();
}

void (*vlog_startup_routines[])(void) = {boot, 0};
