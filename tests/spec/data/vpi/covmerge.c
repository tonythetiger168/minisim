/* IEEE 1800-2023 40.5.1 (tests/spec/40-vpi-coverage-merge.sv):
   vpi_control(vpiCoverageMerge, type, name) adds a saved database's counts
   to this run's. */
#include <stdio.h>
#include <stdlib.h>
#include "sv_vpi_user.h"

static const char* path = "/tmp/msim-spec-40-vpi-coverage-merge-saved.json";
static const int types[] = {vpiStatementCoverage, vpiToggleCoverage, vpiFsmStateCoverage, vpiAssertCoverage};

static void report(const char* when) {
    vpiHandle top = vpi_handle_by_name((PLI_BYTE8*)"top", NULL);
    vpiHandle fsm = vpi_scan(vpi_iterate(vpiFsm, top)), it, s;
    vpiHandle a = vpi_handle_by_name((PLI_BYTE8*)"top.chk", NULL);
    vpi_printf("%s: statements %d toggles %d fsm %d assertions %d\n", when, vpi_get(vpiStatementCoverage, top),
               vpi_get(vpiToggleCoverage, top), vpi_get(vpiFsmStateCoverage, top), vpi_get(vpiAssertCoverage, top));
    it = vpi_iterate(vpiFsmStates, fsm);
    vpi_printf("  states:");
    while ((s = vpi_scan(it))) vpi_printf(" %s=%d", vpi_get_str(vpiName, s), vpi_get(vpiCoveredCount, s));
    vpi_printf("\n  chk attempts %d successes %d\n", vpi_get(vpiAssertAttemptCovered, a), vpi_get(vpiAssertSuccessCovered, a));
}
static PLI_INT32 run(PLI_BYTE8* u) {
    s_vpi_error_info e;
    (void)u;
    report("run");
    vpi_printf("save %d\n", vpi_control(vpiCoverageSave, vpiStatementCoverage, path));
    for (int k = 0; k < 4; k++) vpi_control(vpiCoverageReset, types[k], NULL);
    report("reset");
    for (int k = 0; k < 4; k++) vpi_printf("merge %d: %d\n", types[k], vpi_control(vpiCoverageMerge, types[k], path));
    report("merged");
    vpi_control(vpiCoverageMerge, vpiFsmStateCoverage, path);
    vpi_control(vpiCoverageMerge, vpiAssertCoverage, path);
    report("merged twice (fsm, assertions)");
    remove(path);
    if (vpi_control(vpiCoverageMerge, vpiStatementCoverage, path) == -1) {   /* SV_COV_ERROR */
        vpi_chk_error(&e);
        vpi_printf("missing database: %s\n", e.message);
    }
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$merge", run, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
