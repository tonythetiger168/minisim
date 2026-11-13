/* IEEE 1800-2023 38.6 and 38.36.3 (tests/spec/38.36-vpi-feature-cbs.sv):
   vpi_control(vpiReset), the save, restart and reset callbacks, and
   cbSignal. */
#include <signal.h>
#include <stddef.h>
#include "vpi_user.h"

static PLI_INT32 say(p_cb_data d) {
    vpi_printf("%s\n", (const char*)d->user_data);
    return 0;
}
static PLI_INT32 onSignal(p_cb_data d) {
    s_vpi_time t = {vpiSimTime, 0, 0, 0};
    vpi_get_time(NULL, &t);
    vpi_printf("signal %d at %u\n", (int)d->index, (unsigned)t.low);
    return 0;
}
static PLI_INT32 poke(PLI_BYTE8* u) {
    (void)u;
    raise(SIGUSR1);
    vpi_printf("raised\n");   /* the callback waits for a safe point */
    return 0;
}
static PLI_INT32 vreset(PLI_BYTE8* u) {
    (void)u;
    vpi_control(vpiReset, 0, 9, 0);
    vpi_printf("after vpi_control\n");   /* the reset comes on return */
    return 0;
}
static void cb(PLI_INT32 reason, const char* text) {
    s_cb_data d = {reason, say, NULL, NULL, NULL, 0, (PLI_BYTE8*)text};
    vpi_register_cb(&d);
}
static void task(const char* name, PLI_INT32 (*f)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, f, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    s_cb_data s = {cbSignal, onSignal, NULL, NULL, NULL, SIGUSR1, NULL};
    s_cb_data bad = {cbSignal, onSignal, NULL, NULL, NULL, SIGKILL, NULL};
    cb(cbStartOfSave, "start of save");
    cb(cbEndOfSave, "end of save");
    cb(cbStartOfRestart, "start of restart");
    cb(cbEndOfRestart, "end of restart");
    cb(cbStartOfReset, "start of reset");
    cb(cbEndOfReset, "end of reset");
    vpi_register_cb(&s);
    if (!vpi_register_cb(&bad)) vpi_printf("SIGKILL refused\n");
    task("$poke", poke);
    task("$vreset", vreset);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
