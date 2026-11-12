/* An application written for IEEE 1364-2001 (IEEE 1800-2023 36.12.2): the
 * macro below binds vpi_get, vpi_handle, vpi_iterate and the other routines
 * whose results differ between standards to their _1364v2001 variants. */
#define VPI_COMPATIBILITY_VERSION_1364v2001
#include <string.h>
#include "sv_vpi_user.h"

#define STR2(x) #x
#define STR(x) STR2(x)

static PLI_INT32 mode_old(PLI_BYTE8 *ud) {
    (void)ud;
    int m = (int)vpi_get(vpiCompatibilityMode, NULL);
    vpi_printf("old application: mode %d (%s), bound to %s\n", m, m == vpiMode1364v2001 ? "vpiMode1364v2001" : "?",
               STR(vpi_get));
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL);
    vpiHandle it = vpi_iterate(vpiArgument, call);
    vpiHandle r = vpi_scan(it);
    s_vpi_value v = {vpiDecStrVal};
    vpi_get_value(r, &v);
    vpi_printf("old application: %s is a %s of size %d = %s\n", vpi_get_str(vpiName, r),
               vpi_get(vpiType, r) == vpiReg ? "vpiReg" : "?", (int)vpi_get(vpiSize, r), v.value.str);
    vpiHandle b = vpi_scan(it), mem = vpi_scan(it);
    vpi_printf("old application: b is a %s, ", vpi_get_str(vpiType, b));
    vpi_printf("mem a %s\n", vpi_get_str(vpiType, mem));
    vpi_free_object(it);
    return 0;
}

void register_old(void) {
    s_vpi_systf_data d;
    memset(&d, 0, sizeof d);
    d.type = vpiSysTask;
    d.tfname = "$mode_old";
    d.calltf = mode_old;
    vpi_register_systf(&d);
}
