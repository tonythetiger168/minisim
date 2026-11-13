/* IEEE 1800-2023 38.36.3 (tests/spec/38.36-vpi-unresolved-systf.sv):
   cbUnresolvedSystf. */
#include <string.h>
#include "vpi_user.h"

static PLI_INT32 say(PLI_BYTE8* u) {
    vpi_printf("%s\n", u);
    return 0;
}
static void reg(const char* name, const char* text) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, say, NULL, NULL, (PLI_BYTE8*)text};
    vpi_register_systf(&d);
}
static PLI_INT32 unresolved(p_cb_data d) {
    const char* name = d->value && d->value->format == vpiStringVal ? d->value->value.str : "?";
    vpi_printf("unresolved: %s\n", name);
    if (strcmp(name, "$late") == 0) reg("$late", "late");
    return 0;
}
static void boot(void) {
    s_cb_data d = {cbUnresolvedSystf, unresolved, NULL, NULL, NULL, 0, NULL};
    reg("$known", "known");
    vpi_register_cb(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
