/* IEEE 1800-2023 40.5: the coverage API (tests/spec/40-vpi-coverage.sv). */
#include <stdio.h>
#include <string.h>
#include "sv_vpi_user.h"

static const char* str(PLI_INT32 p, vpiHandle h) {
    static char buf[4][256];
    static int n;
    const char* s = vpi_get_str(p, h);
    n = (n + 1) % 4;
    snprintf(buf[n], sizeof buf[n], "%s", s ? s : "(null)");
    return buf[n];
}

static PLI_INT32 report(PLI_BYTE8* u) {
    (void)u;
    vpiHandle inst = vpi_handle_by_name("top.u", NULL);
    vpi_printf("%s: statements %d toggles %d fsm %d asserts %d\n", str(vpiFullName, inst),
               vpi_get(vpiStatementCoverage, inst), vpi_get(vpiToggleCoverage, inst),
               vpi_get(vpiFsmStateCoverage, inst), vpi_get(vpiAssertCoverage, inst));
    vpiHandle it = vpi_iterate(vpiFsm, inst), f;
    while (it && (f = vpi_scan(it))) {
        vpiHandle e = vpi_handle(vpiFsmStateExpression, f);
        vpi_printf("  fsm %s (%s): covered %d of %d, entered %d\n", str(vpiName, f), str(vpiFullName, e),
                   vpi_get(vpiCovered, f), vpi_get(vpiCoveredMax, f), vpi_get(vpiCoveredCount, f));
        vpiHandle si = vpi_iterate(vpiFsmStates, f), s;
        while (si && (s = vpi_scan(si))) {
            s_vpi_value v = {vpiIntVal};
            vpi_get_value(s, &v);
            vpi_printf("    %s = %d: covered %d count %d\n", str(vpiName, s), v.value.integer,
                       vpi_get(vpiCovered, s), vpi_get(vpiCoveredCount, s));
        }
    }
    vpiHandle cnt = vpi_handle_by_name("top.u.cnt", NULL);
    vpi_printf("  cnt: %d of %d bits toggled, cnt[0]: %d\n", vpi_get(vpiCovered, cnt), vpi_get(vpiCoveredMax, cnt),
               vpi_get(vpiCovered, vpi_handle_by_index(cnt, 0)));
    const char* names[] = {"top.u.a_go", "top.u.c_done"};
    for (int k = 0; k < 2; k++) {
        vpiHandle a = vpi_handle_by_name((PLI_BYTE8*)names[k], NULL);
        vpi_printf("  %s: covered %d, attempts %d success %d failure %d vacuous %d disabled %d killed %d\n",
                   str(vpiName, a), vpi_get(vpiCovered, a), vpi_get(vpiAssertAttemptCovered, a),
                   vpi_get(vpiAssertSuccessCovered, a), vpi_get(vpiAssertFailureCovered, a),
                   vpi_get(vpiAssertVacuousSuccessCovered, a), vpi_get(vpiAssertDisableCovered, a),
                   vpi_get(vpiAssertKillCovered, a));
    }
    vpi_printf("  check: statements %d fsm %d\n", vpi_control(vpiCoverageCheck, vpiStatementCoverage, inst),
               vpi_control(vpiCoverageCheck, vpiFsmStateCoverage, inst));
    return 0;
}

static PLI_INT32 ctl(PLI_BYTE8* u) {
    (void)u;
    vpiHandle call = vpi_handle(vpiSysTfCall, NULL), args = vpi_iterate(vpiArgument, call);
    s_vpi_value v = {vpiIntVal};
    vpi_get_value(vpi_scan(args), &v);
    int op = v.value.integer;
    vpi_get_value(vpi_scan(args), &v);
    int type = v.value.integer;
    vpi_free_object(args);
    vpiHandle h = type == vpiAssertCoverage ? vpi_handle_by_name("top.u", NULL) : NULL;
    vpi_printf("control %d %d: %d\n", op, type, vpi_control(op, type, h));
    return 0;
}

static PLI_INT32 save(PLI_BYTE8* u) {
    (void)u;
    const char* path = "/tmp/msim-spec-40-vpi-coverage-saved.json";
    remove(path);
    vpi_printf("save: %d\n", vpi_control(vpiCoverageSave, vpiStatementCoverage, path));
    FILE* f = fopen(path, "r");
    char line[128] = "";
    if (f) {
        while (fgets(line, sizeof line, f) && !strstr(line, "\"format\"")) {}
        fclose(f);
    }
    vpi_printf("saved database: %s\n", strstr(line, "\"msim-cov\"") ? "msim-cov" : "missing");
    remove(path);
    vpi_printf("merge: %d\n", vpi_control(vpiCoverageMerge, vpiStatementCoverage, path));
    vpi_printf("stop under a sub-instance: %d\n",
               vpi_control(vpiCoverageStop, vpiStatementCoverage, vpi_handle_by_name("top.u", NULL)));
    return 0;
}

static void reg(const char* name, PLI_INT32 (*fn)(PLI_BYTE8*)) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)name, fn, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
static void boot(void) {
    reg("$cov_report", report);
    reg("$cov_ctl", ctl);
    reg("$cov_save", save);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
