/* IEEE 1800-2023 37.32-37.51 (tests/spec/37-vpi-statements.sv): print the
   processes and continuous assignments of top as a tree. */
#include <stdio.h>
#include <string.h>
#include "sv_vpi_user.h"

static void walk(vpiHandle h, int depth, const char* rel);

static void str(PLI_INT32 p, vpiHandle h, char* out, size_t n) {
    const char* s = vpi_get_str(p, h);
    snprintf(out, n, "%s", s ? s : "");
}
static void child(vpiHandle h, PLI_INT32 rel, const char* name, int depth) {
    vpiHandle c = vpi_handle(rel, h);
    if (c) walk(c, depth, name);
}
static void children(vpiHandle h, PLI_INT32 rel, const char* name, int depth) {
    vpiHandle it = vpi_iterate(rel, h), c;
    while (it && (c = vpi_scan(it))) walk(c, depth, name);
}
static void walk(vpiHandle h, int depth, const char* rel) {
    char type[64], name[128], text[128];
    int t = vpi_get(vpiType, h);
    str(vpiType, h, type, sizeof type);
    str(vpiName, h, name, sizeof name);
    vpi_printf("%*s%s%s%s line %d", depth * 2, "", rel, *rel ? ": " : "", type, vpi_get(vpiLineNo, h));
    if (*name) vpi_printf(" %s", name);
    switch (t) {
        case vpiAlways: vpi_printf(" type %d", vpi_get(vpiAlwaysType, h)); break;
        case vpiAssignment: vpi_printf(" blocking %d", vpi_get(vpiBlocking, h)); break;
        case vpiCase: vpi_printf(" case type %d qualifier %d", vpi_get(vpiCaseType, h), vpi_get(vpiQualifier, h)); break;
        case vpiFork: vpi_printf(" join %d", vpi_get(vpiJoinType, h)); break;
        case vpiOperation: vpi_printf(" op %d", vpi_get(vpiOpType, h)); break;
        case vpiConstant: {
            s_vpi_value v = {vpiDecStrVal};
            vpi_get_value(h, &v);
            vpi_printf(" const type %d value %s size %d", vpi_get(vpiConstType, h), v.value.str, vpi_get(vpiSize, h));
            break;
        }
        default: break;
    }
    if (t == vpiOperation || t == vpiConstant || t == vpiFuncCall || t == vpiSysFuncCall || t == vpiBitSelect || t == vpiPartSelect) {
        str(vpiDecompile, h, text, sizeof text);
        if (*text) vpi_printf(" `%s`", text);
    }
    vpi_printf("\n");
    switch (t) {
        case vpiInitial: case vpiAlways: case vpiFinal: child(h, vpiStmt, "stmt", depth + 1); break;
        case vpiContAssign:
            child(h, vpiLhs, "lhs", depth + 1);
            child(h, vpiRhs, "rhs", depth + 1);
            child(h, vpiDelay, "delay", depth + 1);
            break;
        case vpiBegin: case vpiNamedBegin: case vpiFork: case vpiNamedFork: children(h, vpiStmt, "", depth + 1); break;
        case vpiAssignment: case vpiAssignStmt: case vpiForce:
            child(h, vpiLhs, "lhs", depth + 1);
            child(h, vpiRhs, "rhs", depth + 1);
            break;
        case vpiRelease: case vpiDeassign: child(h, vpiLhs, "lhs", depth + 1); break;
        case vpiIf: case vpiIfElse:
            child(h, vpiCondition, "cond", depth + 1);
            child(h, vpiStmt, "then", depth + 1);
            child(h, vpiElseStmt, "else", depth + 1);
            break;
        case vpiCase:
            child(h, vpiCondition, "cond", depth + 1);
            children(h, vpiCaseItem, "item", depth + 1);
            break;
        case vpiCaseItem:
            children(h, vpiExpr, "expr", depth + 1);
            child(h, vpiStmt, "stmt", depth + 1);
            break;
        case vpiFor:
            children(h, vpiForInitStmt, "init", depth + 1);
            child(h, vpiCondition, "cond", depth + 1);
            children(h, vpiForIncStmt, "inc", depth + 1);
            child(h, vpiStmt, "body", depth + 1);
            break;
        case vpiRepeat: case vpiWhile: case vpiWait:
            child(h, vpiCondition, "cond", depth + 1);
            child(h, vpiStmt, "body", depth + 1);
            break;
        case vpiDelayControl:
            child(h, vpiDelay, "delay", depth + 1);
            child(h, vpiStmt, "stmt", depth + 1);
            break;
        case vpiEventControl:
            child(h, vpiCondition, "event", depth + 1);
            child(h, vpiStmt, "stmt", depth + 1);
            break;
        case vpiTaskCall: case vpiSysTaskCall: case vpiFuncCall: case vpiSysFuncCall:
            children(h, vpiArgument, "arg", depth + 1);
            break;
        case vpiOperation: children(h, vpiOperand, "", depth + 1); break;
        case vpiBitSelect:
            child(h, vpiParent, "of", depth + 1);
            child(h, vpiIndex, "index", depth + 1);
            break;
        default: break;
    }
}

static PLI_INT32 walkTop(PLI_BYTE8* u) {
    (void)u;
    vpiHandle top = vpi_handle_by_name("top", NULL);
    children(top, vpiContAssign, "", 0);
    children(top, vpiProcess, "", 0);
    vpiHandle p = vpi_scan(vpi_iterate(vpiProcess, top));
    char name[128];
    str(vpiFullName, vpi_handle(vpiModule, vpi_handle(vpiStmt, p)), name, sizeof name);
    vpi_printf("module of the first statement: %s\n", name);
    return 0;
}
static void boot(void) {
    s_vpi_systf_data d = {vpiSysTask, 0, (PLI_BYTE8*)"$walk", walkTop, NULL, NULL, NULL};
    vpi_register_systf(&d);
}
void (*vlog_startup_routines[])(void) = {boot, NULL};
